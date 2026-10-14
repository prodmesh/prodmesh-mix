// CLAP adapter. Bundles are loaded once per path and kept while any
// instance uses them (clap_entry init/deinit bracket all instances).
#include <clap/clap.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "core/rt_guard.h"
#include "plugin/plugin.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <dlfcn.h>
#endif

namespace pm::plugin {
namespace {

// PM_PLUGIN_TRACE=1 logs every host->plugin call (control thread only, and
// the first process call), to pin down which call a misbehaving plugin
// dies in.
bool tracing() {
    static const bool on = std::getenv("PM_PLUGIN_TRACE") != nullptr;
    return on;
}
#define PM_TRACE(...)                                  \
    do {                                               \
        if (tracing()) {                               \
            std::fprintf(stderr, "[clap] " __VA_ARGS__); \
            std::fputc('\n', stderr);                  \
        }                                              \
    } while (0)

// --- module registry (control thread only) ------------------------------

struct Module {
    std::string path;
    const clap_plugin_entry_t* entry = nullptr;
#if defined(__APPLE__)
    CFBundleRef bundle = nullptr;
#else
    void* handle = nullptr;
#endif
    ~Module() {
        if (entry) entry->deinit();
#if defined(__APPLE__)
        if (bundle) CFRelease(bundle);
#else
        if (handle) dlclose(handle);
#endif
    }
};

std::map<std::string, std::weak_ptr<Module>>& registry() {
    static std::map<std::string, std::weak_ptr<Module>> r;
    return r;
}

std::shared_ptr<Module> openModule(const std::string& path,
                                   std::string* error) {
    if (auto m = registry()[path].lock()) return m;
    auto m = std::make_shared<Module>();
    m->path = path;
#if defined(__APPLE__)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
        CFIndex(path.size()), true);
    m->bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
    if (url) CFRelease(url);
    if (!m->bundle || !CFBundleLoadExecutable(m->bundle)) {
        if (error) *error = path + ": cannot load bundle";
        return nullptr;
    }
    m->entry = static_cast<const clap_plugin_entry_t*>(
        CFBundleGetDataPointerForName(m->bundle, CFSTR("clap_entry")));
#else
    m->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (m->handle)
        m->entry = static_cast<const clap_plugin_entry_t*>(
            dlsym(m->handle, "clap_entry"));
#endif
    if (!m->entry || !clap_version_is_compatible(m->entry->clap_version)) {
        m->entry = nullptr;
        if (error) *error = path + ": no compatible clap_entry";
        return nullptr;
    }
    if (!m->entry->init(path.c_str())) {
        m->entry = nullptr;
        if (error) *error = path + ": clap_entry init failed";
        return nullptr;
    }
    registry()[path] = m;
    return m;
}

const clap_plugin_factory_t* factoryOf(const Module& m) {
    return static_cast<const clap_plugin_factory_t*>(
        m.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
}

// --- host callbacks -----------------------------------------------------

const void* hostGetExtension(const clap_host_t*, const char* id) {
    PM_TRACE("host.get_extension(%s)", id);
    return nullptr;
}
void hostRequestRestart(const clap_host_t*) {}
void hostRequestProcess(const clap_host_t*) {}
void hostRequestCallback(const clap_host_t*) {}

uint32_t noEventsSize(const clap_input_events_t*) { return 0; }
const clap_event_header_t* noEventsGet(const clap_input_events_t*, uint32_t) {
    return nullptr;
}
bool discardEvent(const clap_output_events_t*, const clap_event_header_t*) {
    return true;
}
const clap_input_events_t kNoEvents{nullptr, noEventsSize, noEventsGet};
const clap_output_events_t kDiscard{nullptr, discardEvent};

class ClapProcessor final : public Processor {
public:
    ClapProcessor() {
        host_.clap_version = CLAP_VERSION;
        host_.host_data = this;
        host_.name = "ProdMesh Mix";
        host_.vendor = "ProdMesh";
        host_.url = "";
        host_.version = "0.0.1";
        host_.get_extension = hostGetExtension;
        host_.request_restart = hostRequestRestart;
        host_.request_process = hostRequestProcess;
        host_.request_callback = hostRequestCallback;
    }

    ~ClapProcessor() override {
        if (!plugin_) return;
        destroyEditor();
        // The engine stops processing on the audio thread before an
        // instance is retired; this is the backstop.
        if (activated_) plugin_->deactivate(plugin_);
        plugin_->destroy(plugin_);
    }

    bool init(std::shared_ptr<Module> mod, const std::string& id, double fs,
              int maxBlock, int channels, std::string* error) {
        module_ = std::move(mod);
        auto fail = [&](const std::string& why) {
            if (error) *error = id + ": " + why;
            return false;
        };
        const auto* factory = factoryOf(*module_);
        if (!factory) return fail("no plugin factory");
        PM_TRACE("create_plugin(%s)", id.c_str());
        plugin_ = factory->create_plugin(factory, &host_, id.c_str());
        if (!plugin_) return fail("not found in bundle");
        PM_TRACE("init");
        name_ = plugin_->desc && plugin_->desc->name ? plugin_->desc->name : id;
        if (!plugin_->init(plugin_)) {
            plugin_->destroy(plugin_);
            plugin_ = nullptr;
            return fail("init failed");
        }
        nodeChannels_ = channels;
        maxBlock_ = maxBlock;
        // Every port the plugin declares gets a buffer, not just the main
        // pair: plugins with a sidechain input read it unconditionally
        // (FabFilter crashes otherwise). Sidechains are fed silence.
        auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
        auto setup = [&](bool input, std::vector<Port>& out, int& main) {
            const int n = ports ? int(ports->count(plugin_, input)) : 1;
            main = 0;
            for (int i = 0; i < n; ++i) {
                clap_audio_port_info_t info{};
                int nch = channels;
                if (ports && ports->get(plugin_, std::uint32_t(i), input, &info)) {
                    nch = int(info.channel_count);
                    if (info.flags & CLAP_AUDIO_PORT_IS_MAIN) main = i;
                }
                if (nch < 1 || nch > 8) return false;
                Port port;
                port.data.assign(std::size_t(nch * maxBlock), 0.0f);
                for (int c = 0; c < nch; ++c)
                    port.ptr.push_back(&port.data[std::size_t(c * maxBlock)]);
                out.push_back(std::move(port));
            }
            return true;
        };
        if (!setup(true, inPorts_, mainIn_) || !setup(false, outPorts_, mainOut_))
            return fail("unsupported channel layout");
        if (inPorts_.empty()) return fail("has no audio input (instrument?)");
        if (outPorts_.empty()) return fail("has no audio output");
        for (auto& p : inPorts_)
            inBufs_.push_back({p.ptr.data(), nullptr,
                               std::uint32_t(p.ptr.size()), 0, 0});
        for (auto& p : outPorts_)
            outBufs_.push_back({p.ptr.data(), nullptr,
                                std::uint32_t(p.ptr.size()), 0, 0});

        PM_TRACE("activate(%.0f, 1, %d)", fs, maxBlock);
        if (!plugin_->activate(plugin_, fs, 1, std::uint32_t(maxBlock)))
            return fail("activate failed");
        activated_ = true;
        if (auto* lat = static_cast<const clap_plugin_latency_t*>(
                plugin_->get_extension(plugin_, CLAP_EXT_LATENCY)))
            latency_ = int(lat->get(plugin_));
        gui_ = static_cast<const clap_plugin_gui_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_GUI));
        return true;
    }

    // --- editor (main thread of the hosting process) --------------------
    bool hasEditor() const override {
#if defined(__APPLE__)
        return gui_ && gui_->is_api_supported(plugin_, CLAP_WINDOW_API_COCOA, false);
#else
        return false;
#endif
    }

    bool createEditor(void* parent, int* w, int* h) override {
#if defined(__APPLE__)
        if (!hasEditor() || editorOpen_) return false;
        PM_TRACE("gui.create(cocoa)");
        if (!gui_->create(plugin_, CLAP_WINDOW_API_COCOA, false)) return false;
        std::uint32_t width = 600, height = 400;
        gui_->get_size(plugin_, &width, &height);
        clap_window_t win{};
        win.api = CLAP_WINDOW_API_COCOA;
        win.cocoa = parent;
        if (!gui_->set_parent(plugin_, &win)) {
            gui_->destroy(plugin_);
            return false;
        }
        gui_->show(plugin_);
        editorOpen_ = true;
        if (w) *w = int(width);
        if (h) *h = int(height);
        return true;
#else
        return false;
#endif
    }

    // --- state (main thread) ---------------------------------------------
    bool saveState(std::string& out) override {
        auto* st = static_cast<const clap_plugin_state_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_STATE));
        if (!st) return false;
        out.clear();
        clap_ostream_t os{&out, [](const clap_ostream_t* s, const void* buf,
                                   uint64_t size) -> int64_t {
                              static_cast<std::string*>(s->ctx)->append(
                                  static_cast<const char*>(buf), std::size_t(size));
                              return int64_t(size);
                          }};
        return st->save(plugin_, &os);
    }

    bool loadState(const std::string& in) override {
        auto* st = static_cast<const clap_plugin_state_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_STATE));
        if (!st) return false;
        struct Reader {
            const std::string* data;
            std::size_t pos;
        } r{&in, 0};
        clap_istream_t is{&r, [](const clap_istream_t* s, void* buf,
                                 uint64_t size) -> int64_t {
                              auto* rd = static_cast<Reader*>(s->ctx);
                              const auto n = std::min<std::size_t>(
                                  std::size_t(size), rd->data->size() - rd->pos);
                              std::memcpy(buf, rd->data->data() + rd->pos, n);
                              rd->pos += n;
                              return int64_t(n);
                          }};
        return st->load(plugin_, &is);
    }

    void destroyEditor() override {
        if (!editorOpen_) return;
        gui_->hide(plugin_);
        gui_->destroy(plugin_);
        editorOpen_ = false;
    }

    void process(float* const* ch, int nch, int n) override {
        rt::ScopedThirdParty attribute(&rtAllocs_);
        if (failed_ || n > maxBlock_) return;  // pass-through
        if (!processing_) {
            PM_TRACE("start_processing");
            if (!plugin_->start_processing(plugin_)) {
                failed_ = true;
                return;
            }
            processing_ = true;
        }
        // Adapt widths: a mono strip feeds every main input channel;
        // plugin outputs beyond the strip's width are dropped.
        auto& in = inPorts_[std::size_t(mainIn_)].ptr;
        const int inCh = int(in.size());
        for (int c = 0; c < inCh; ++c)
            std::memcpy(in[std::size_t(c)], ch[std::min(c, nch - 1)],
                        sizeof(float) * std::size_t(n));
        for (std::size_t i = 0; i < inPorts_.size(); ++i)
            if (int(i) != mainIn_)
                for (float* x : inPorts_[i].ptr)
                    std::memset(x, 0, sizeof(float) * std::size_t(n));
        clap_process_t p{};
        p.steady_time = steady_;
        p.frames_count = std::uint32_t(n);
        p.audio_inputs = inBufs_.data();
        p.audio_outputs = outBufs_.data();
        p.audio_inputs_count = std::uint32_t(inBufs_.size());
        p.audio_outputs_count = std::uint32_t(outBufs_.size());
        p.in_events = &kNoEvents;
        p.out_events = &kDiscard;
        if (steady_ == 0) PM_TRACE("process (first, %d frames)", n);
        steady_ += n;
        if (plugin_->process(plugin_, &p) == CLAP_PROCESS_ERROR) return;
        const auto& out = outPorts_[std::size_t(mainOut_)].ptr;
        const int outCh = int(out.size());
        for (int c = 0; c < nch; ++c)
            std::memcpy(ch[c], out[std::size_t(std::min(c, outCh - 1))],
                        sizeof(float) * std::size_t(n));
    }

    void stopProcessing() override {
        rt::ScopedThirdParty attribute(&rtAllocs_);
        if (processing_) plugin_->stop_processing(plugin_);
        processing_ = false;
    }

    int latencySamples() const override { return latency_; }
    std::string name() const override { return name_; }

private:
    std::shared_ptr<Module> module_;
    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_gui_t* gui_ = nullptr;
    bool editorOpen_ = false;
    bool activated_ = false, processing_ = false, failed_ = false;
    struct Port {
        std::vector<float> data;
        std::vector<float*> ptr;
    };
    int nodeChannels_ = 1, maxBlock_ = 0, mainIn_ = 0, mainOut_ = 0;
    std::string name_;
    int latency_ = 0;
    std::int64_t steady_ = 0;
    std::vector<Port> inPorts_, outPorts_;
    std::vector<clap_audio_buffer_t> inBufs_, outBufs_;
};

}  // namespace

bool describeClap(const std::string& path, std::vector<PluginInfo>& out,
                  std::string* error) {
    auto mod = openModule(path, error);
    if (!mod) return false;
    const auto* f = factoryOf(*mod);
    if (!f) {
        if (error) *error = path + ": no plugin factory";
        return false;
    }
    for (std::uint32_t i = 0; i < f->get_plugin_count(f); ++i) {
        const auto* d = f->get_plugin_descriptor(f, i);
        if (!d) continue;
        bool instrument = false;
        for (auto feat = d->features; feat && *feat; ++feat)
            instrument = instrument || !std::strcmp(*feat, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        out.push_back({"clap", path, d->id ? d->id : "", d->name ? d->name : "",
                       d->vendor ? d->vendor : "",
                       d->version ? d->version : "", instrument});
    }
    return true;
}

std::unique_ptr<Processor> loadClap(const std::string& path,
                                    const std::string& id, double fs,
                                    int maxBlock, int channels,
                                    std::string* error) {
    auto mod = openModule(path, error);
    if (!mod) return nullptr;
    auto p = std::make_unique<ClapProcessor>();
    if (!p->init(std::move(mod), id, fs, maxBlock, channels, error))
        return nullptr;
    return p;
}

}  // namespace pm::plugin
