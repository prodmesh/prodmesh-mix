// VST3 adapter, written against the SDK's interface headers only (no SDK
// hosting library). Audio-only: the edit controller is not instantiated
// yet, so parameters and editors come later.
//
// Plugins dereference things hosts are allowed to leave null; we always
// pass an (empty) parameter-change list, event list and a transport
// context, and a buffer for every bus the plugin declares.
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <atomic>
#include <map>
#include <variant>

#include "core/rt_guard.h"
#include "core/spsc_queue.h"
#include "plugin/plugin.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

// The SDK's vstinitiids.cpp does not cover the GUI interfaces.
namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
}  // namespace Steinberg

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace pm::plugin {
namespace {

bool tracing() {
    static const bool on = std::getenv("PM_PLUGIN_TRACE") != nullptr;
    return on;
}
#define PM_TRACE(...)                                  \
    do {                                               \
        if (tracing()) {                               \
            std::fprintf(stderr, "[vst3] " __VA_ARGS__); \
            std::fputc('\n', stderr);                  \
        }                                              \
    } while (0)

// --- host-side COM objects (static lifetime, so refcounts are no-ops) ---

#define PM_NO_REFCOUNT                                   \
    uint32 PLUGIN_API addRef() override { return 1; }    \
    uint32 PLUGIN_API release() override { return 1; }

#define PM_REFCOUNT                                                  \
    std::atomic<uint32> refs_{1};                                    \
    uint32 PLUGIN_API addRef() override { return ++refs_; }          \
    uint32 PLUGIN_API release() override {                           \
        const uint32 r = --refs_;                                    \
        if (r == 0) delete this;                                     \
        return r;                                                    \
    }

// Messages between a plugin's processor and controller travel through
// host-created objects; plugins create them with IHostApplication.
class HostAttributes final : public IAttributeList {
public:
    using Value = std::variant<int64, double, std::u16string, std::string>;
    tresult PLUGIN_API setInt(AttrID id, int64 v) override { return set(id, v); }
    tresult PLUGIN_API getInt(AttrID id, int64& v) override { return get(id, v); }
    tresult PLUGIN_API setFloat(AttrID id, double v) override { return set(id, v); }
    tresult PLUGIN_API getFloat(AttrID id, double& v) override { return get(id, v); }
    tresult PLUGIN_API setString(AttrID id, const TChar* s) override {
        std::u16string str;
        for (; s && *s; ++s) str += char16_t(*s);
        return set(id, str);
    }
    tresult PLUGIN_API getString(AttrID id, TChar* out, uint32 sizeBytes) override {
        auto it = values_.find(id);
        if (it == values_.end() || !std::holds_alternative<std::u16string>(it->second))
            return kResultFalse;
        const auto& str = std::get<std::u16string>(it->second);
        const uint32 n = std::min<uint32>(uint32(str.size()), sizeBytes / sizeof(TChar) - 1);
        for (uint32 i = 0; i < n; ++i) out[i] = TChar(str[i]);
        out[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* d, uint32 size) override {
        return set(id, std::string(static_cast<const char*>(d), size));
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& d, uint32& size) override {
        auto it = values_.find(id);
        if (it == values_.end() || !std::holds_alternative<std::string>(it->second))
            return kResultFalse;
        const auto& b = std::get<std::string>(it->second);
        d = b.data();
        size = uint32(b.size());
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IAttributeList::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_REFCOUNT

private:
    template <typename T>
    tresult set(AttrID id, T v) {
        if (!id) return kInvalidArgument;
        values_[id] = std::move(v);
        return kResultOk;
    }
    template <typename T>
    tresult get(AttrID id, T& v) {
        auto it = values_.find(id ? id : "");
        if (it == values_.end() || !std::holds_alternative<T>(it->second))
            return kResultFalse;
        v = std::get<T>(it->second);
        return kResultOk;
    }
    std::map<std::string, Value> values_;
};

class HostMessage final : public IMessage {
public:
    ~HostMessage() { attrs_->release(); }
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id ? id : ""; }
    IAttributeList* PLUGIN_API getAttributes() override { return attrs_; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IMessage::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_REFCOUNT

private:
    std::string id_;
    HostAttributes* attrs_ = new HostAttributes;
};

class HostApp final : public IHostApplication {
public:
    tresult PLUGIN_API getName(String128 name) override {
        const char* n = "ProdMesh Mix";
        int i = 0;
        for (; n[i] && i < 127; ++i) name[i] = char16(n[i]);
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(cid, IMessage::iid) &&
            FUnknownPrivate::iidEqual(iid, IMessage::iid)) {
            *obj = static_cast<IMessage*>(new HostMessage);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(cid, IAttributeList::iid) &&
            FUnknownPrivate::iidEqual(iid, IAttributeList::iid)) {
            *obj = static_cast<IAttributeList*>(new HostAttributes);
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(iid, IHostApplication::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_NO_REFCOUNT
};

// Fixed-capacity parameter-change container. Input: always empty for now.
// Output: plugins report meters (e.g. gain reduction) here and often do not
// check for a null queue — FabFilter's dynamics crash without one — so it
// accepts everything into preallocated queues, cleared every block. No
// allocation on the audio thread.
class ParamQueue final : public IParamValueQueue {
public:
    static constexpr int kPoints = 32;
    ParamID id = 0;
    int32 count = 0;
    int32 offsets[kPoints]{};
    ParamValue values[kPoints]{};

    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 i, int32& offset, ParamValue& v) override {
        if (i < 0 || i >= count) return kResultFalse;
        offset = offsets[i];
        v = values[i];
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue v, int32& index) override {
        if (count == kPoints) {  // keep the latest value in the last slot
            offsets[kPoints - 1] = offset;
            values[kPoints - 1] = v;
            index = kPoints - 1;
            return kResultOk;
        }
        offsets[count] = offset;
        values[count] = v;
        index = count++;
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IParamValueQueue::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_NO_REFCOUNT
};

class ParamChanges final : public IParameterChanges {
public:
    static constexpr int kQueues = 128;
    void clear() {
        for (int i = 0; i < used_; ++i) queues_[i].count = 0;
        used_ = 0;
    }
    int32 PLUGIN_API getParameterCount() override { return used_; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 i) override {
        return i >= 0 && i < used_ ? &queues_[i] : nullptr;
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id,
                                                  int32& index) override {
        for (int i = 0; i < used_; ++i)
            if (queues_[i].id == id) {
                index = i;
                return &queues_[i];
            }
        // Full: hand back a scratch queue so callers that skip the null
        // check still have somewhere to write.
        if (used_ == kQueues) {
            index = kQueues;
            scratch_.count = 0;
            return &scratch_;
        }
        queues_[used_].id = id;
        queues_[used_].count = 0;
        index = used_;
        return &queues_[used_++];
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IParameterChanges::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_NO_REFCOUNT

private:
    ParamQueue queues_[kQueues];
    ParamQueue scratch_;
    int used_ = 0;
};

class NoEvents final : public IEventList {
public:
    int32 PLUGIN_API getEventCount() override { return 0; }
    tresult PLUGIN_API getEvent(int32, Event&) override { return kResultFalse; }
    tresult PLUGIN_API addEvent(Event&) override { return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IEventList::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_NO_REFCOUNT
};

HostApp gHost;

// Growable in-memory IBStream for component state.
class MemStream final : public IBStream {
public:
    std::string data;
    std::size_t pos = 0;
    tresult PLUGIN_API read(void* buf, int32 n, int32* got) override {
        const auto k = std::min<std::size_t>(std::size_t(std::max(n, 0)), data.size() - pos);
        std::memcpy(buf, data.data() + pos, k);
        pos += k;
        if (got) *got = int32(k);
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buf, int32 n, int32* wrote) override {
        if (n < 0) return kInvalidArgument;
        if (pos + std::size_t(n) > data.size()) data.resize(pos + std::size_t(n));
        std::memcpy(&data[pos], buf, std::size_t(n));
        pos += std::size_t(n);
        if (wrote) *wrote = n;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 off, int32 mode, int64* result) override {
        int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? int64(pos) : int64(data.size());
        const int64 np = base + off;
        if (np < 0) return kInvalidArgument;
        pos = std::size_t(np);
        if (result) *result = np;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* p) override {
        if (p) *p = int64(pos);
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IBStream::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_NO_REFCOUNT
};

// --- module registry (control thread only) ------------------------------

using GetFactoryProc = IPluginFactory* (*)();
#if defined(__APPLE__)
using BundleEntryProc = bool (*)(CFBundleRef);
using BundleExitProc = bool (*)();
#endif

struct Module {
    IPluginFactory* factory = nullptr;
#if defined(__APPLE__)
    CFBundleRef bundle = nullptr;
    BundleExitProc exit = nullptr;
#endif
    ~Module() {
        if (factory) factory->release();
#if defined(__APPLE__)
        if (exit) exit();
        if (bundle) CFRelease(bundle);
#endif
    }
};

std::shared_ptr<Module> openModule(const std::string& path,
                                   std::string* error) {
    static std::map<std::string, std::weak_ptr<Module>> registry;
    if (auto m = registry[path].lock()) return m;
    auto m = std::make_shared<Module>();
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
    auto entry = reinterpret_cast<BundleEntryProc>(
        CFBundleGetFunctionPointerForName(m->bundle, CFSTR("bundleEntry")));
    m->exit = reinterpret_cast<BundleExitProc>(
        CFBundleGetFunctionPointerForName(m->bundle, CFSTR("bundleExit")));
    auto getFactory = reinterpret_cast<GetFactoryProc>(
        CFBundleGetFunctionPointerForName(m->bundle, CFSTR("GetPluginFactory")));
    if (!entry || !getFactory || !entry(m->bundle)) {
        m->exit = nullptr;
        if (error) *error = path + ": not a VST3 bundle";
        return nullptr;
    }
    m->factory = getFactory();
#endif
    if (!m->factory) {
        if (error) *error = path + ": no plugin factory";
        return nullptr;
    }
    registry[path] = m;
    return m;
}

std::string hexId(const TUID id) {
    char s[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(s + 2 * i, 3, "%02X", static_cast<unsigned char>(id[i]));
    return s;
}

bool parseId(const std::string& hex, TUID out) {
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        if (std::sscanf(hex.c_str() + 2 * i, "%2X", &v) != 1) return false;
        out[i] = static_cast<char>(v);
    }
    return true;
}

// Parameter edits from the plugin's editor (controller, main thread) to
// its processor (audio thread), through a wait-free queue.
struct ParamEdit {
    ParamID id = 0;
    ParamValue value = 0;
};

class ComponentHandler final : public IComponentHandler {
public:
    SpscQueue<ParamEdit>* edits = nullptr;
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
        if (edits) edits->push({id, v});
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_NO_REFCOUNT
};

class PlugFrame final : public IPlugFrame {
public:
    std::function<void(int, int)>* onResize = nullptr;
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
        if (!view || !r) return kInvalidArgument;
        if (onResize && *onResize) (*onResize)(r->getWidth(), r->getHeight());
        view->onSize(r);
        return kResultTrue;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    PM_NO_REFCOUNT
};

class Vst3Processor final : public Processor {
public:
    ~Vst3Processor() override {
        destroyEditor();
        if (controller_) {
            controller_->setComponentHandler(nullptr);
            if (componentCP_ && controllerCP_) {
                componentCP_->disconnect(controllerCP_);
                controllerCP_->disconnect(componentCP_);
            }
            if (componentCP_) componentCP_->release();
            if (controllerCP_) controllerCP_->release();
            if (separateController_) controller_->terminate();
            controller_->release();
        }
        if (component_) {
            if (active_) component_->setActive(false);
            component_->terminate();
        }
        if (processor_) processor_->release();
        if (component_) component_->release();
    }

    bool init(std::shared_ptr<Module> mod, const std::string& id, double fs,
              int maxBlock, int channels, std::string* error) {
        module_ = std::move(mod);
        auto fail = [&](const std::string& why) {
            if (error) *error = id + ": " + why;
            return false;
        };
        TUID cid;
        if (!parseId(id, cid)) return fail("bad class id");
        name_ = id;
        for (int32 i = 0; i < module_->factory->countClasses(); ++i) {
            PClassInfo ci;
            if (module_->factory->getClassInfo(i, &ci) == kResultOk &&
                std::memcmp(ci.cid, cid, sizeof(TUID)) == 0)
                name_ = ci.name;
        }
        if (module_->factory->createInstance(cid, IComponent::iid,
                                             reinterpret_cast<void**>(&component_)) !=
                kResultOk ||
            !component_)
            return fail("cannot create component");
        PM_TRACE("initialize");
        if (component_->initialize(&gHost) != kResultOk)
            return fail("initialize failed");
        if (component_->queryInterface(IAudioProcessor::iid,
                                       reinterpret_cast<void**>(&processor_)) !=
                kResultOk ||
            !processor_)
            return fail("not an audio processor");

        setUpController();

        const int32 nIn = component_->getBusCount(kAudio, kInput);
        const int32 nOut = component_->getBusCount(kAudio, kOutput);
        if (nIn < 1) return fail("has no audio input (instrument?)");
        if (nOut < 1) return fail("has no audio output");

        // Ask for the strip's width on the main buses; keep whatever the
        // plugin has on the others, then read back what it accepted.
        std::vector<SpeakerArrangement> ins(static_cast<std::size_t>(nIn));
        std::vector<SpeakerArrangement> outs(static_cast<std::size_t>(nOut));
        for (int32 i = 0; i < nIn; ++i) processor_->getBusArrangement(kInput, i, ins[std::size_t(i)]);
        for (int32 i = 0; i < nOut; ++i) processor_->getBusArrangement(kOutput, i, outs[std::size_t(i)]);
        const auto want = channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        ins[0] = outs[0] = want;
        PM_TRACE("setBusArrangements");
        if (processor_->setBusArrangements(ins.data(), nIn, outs.data(), nOut) != kResultOk) {
            // Common: stereo-only plugins. Use what they have.
            processor_->getBusArrangement(kInput, 0, ins[0]);
            processor_->getBusArrangement(kOutput, 0, outs[0]);
        }
        component_->activateBus(kAudio, kInput, 0, true);
        component_->activateBus(kAudio, kOutput, 0, true);

        auto setupBuses = [&](BusDirection dir, int32 n, std::vector<Bus>& buses) {
            for (int32 i = 0; i < n; ++i) {
                SpeakerArrangement arr = 0;
                processor_->getBusArrangement(dir, i, arr);
                const int nch = std::clamp(SpeakerArr::getChannelCount(arr), 1, 8);
                Bus b;
                b.data.assign(std::size_t(nch * maxBlock), 0.0f);
                for (int c = 0; c < nch; ++c)
                    b.ptr.push_back(&b.data[std::size_t(c * maxBlock)]);
                buses.push_back(std::move(b));
            }
        };
        setupBuses(kInput, nIn, inBuses_);
        setupBuses(kOutput, nOut, outBuses_);
        for (auto& b : inBuses_) {
            AudioBusBuffers a{};
            a.numChannels = int32(b.ptr.size());
            a.channelBuffers32 = b.ptr.data();
            inBufs_.push_back(a);
        }
        for (auto& b : outBuses_) {
            AudioBusBuffers a{};
            a.numChannels = int32(b.ptr.size());
            a.channelBuffers32 = b.ptr.data();
            outBufs_.push_back(a);
        }

        ProcessSetup setup{kRealtime, kSample32, int32(maxBlock), fs};
        PM_TRACE("setupProcessing");
        if (processor_->setupProcessing(setup) != kResultOk)
            return fail("setupProcessing failed");
        PM_TRACE("setActive(true)");
        if (component_->setActive(true) != kResultOk)
            return fail("setActive failed");
        active_ = true;
        latency_ = int(processor_->getLatencySamples());
        maxBlock_ = maxBlock;

        ctx_.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid |
                     ProcessContext::kContTimeValid;
        ctx_.sampleRate = fs;
        ctx_.tempo = 120;
        ctx_.timeSigNumerator = 4;
        ctx_.timeSigDenominator = 4;
        return true;
    }

    void process(float* const* ch, int nch, int n) override {
        rt::ScopedThirdParty attribute(&rtAllocs_);
        if (failed_ || n > maxBlock_) return;  // pass-through
        if (!processing_) {
            // kNotImplemented is a valid answer.
        PM_TRACE("setProcessing(true)");
            const auto r = processor_->setProcessing(true);
            if (r != kResultOk && r != kNotImplemented) {
                failed_ = true;
                return;
            }
            processing_ = true;
        }
        auto& in = inBuses_[0].ptr;
        for (std::size_t c = 0; c < in.size(); ++c)
            std::memcpy(in[c], ch[std::min(int(c), nch - 1)],
                        sizeof(float) * std::size_t(n));
        for (std::size_t b = 1; b < inBuses_.size(); ++b)
            for (float* x : inBuses_[b].ptr)
                std::memset(x, 0, sizeof(float) * std::size_t(n));

        ProcessData d;
        d.processMode = kRealtime;
        d.symbolicSampleSize = kSample32;
        d.numSamples = int32(n);
        d.numInputs = int32(inBufs_.size());
        d.numOutputs = int32(outBufs_.size());
        d.inputs = inBufs_.data();
        d.outputs = outBufs_.data();
        paramsOut_.clear();
        paramsIn_.clear();
        while (auto e = edits_.pop()) {
            int32 index = 0;
            if (auto* q = paramsIn_.addParameterData(e->id, index)) q->addPoint(0, e->value, index);
        }
        d.inputParameterChanges = &paramsIn_;
        d.outputParameterChanges = &paramsOut_;
        d.inputEvents = &eventsIn_;
        d.outputEvents = &eventsOut_;
        ctx_.projectTimeSamples = ctx_.continousTimeSamples = time_;
        d.processContext = &ctx_;
        if (time_ == 0) PM_TRACE("process (first, %d frames, %d in / %d out buses)", n, int(d.numInputs), int(d.numOutputs));
        time_ += n;
        if (processor_->process(d) != kResultOk) return;
        if (time_ == n) PM_TRACE("process returned");

        const auto& out = outBuses_[0].ptr;
        for (int c = 0; c < nch; ++c)
            std::memcpy(ch[c], out[std::size_t(std::min(c, int(out.size()) - 1))],
                        sizeof(float) * std::size_t(n));
    }

    // Blob: "PMV3", u32 component bytes, component state, u32 controller
    // bytes, controller state. A bare blob is component state alone.
    bool saveState(std::string& out) override {
        MemStream comp, ctrl;
        if (component_->getState(&comp) != kResultOk) return false;
        if (controller_) controller_->getState(&ctrl);
        auto u32 = [&out](std::uint32_t v) { out.append(reinterpret_cast<char*>(&v), 4); };
        out = "PMV3";
        u32(std::uint32_t(comp.data.size()));
        out += comp.data;
        u32(std::uint32_t(ctrl.data.size()));
        out += ctrl.data;
        return true;
    }

    bool loadState(const std::string& in) override {
        MemStream comp, ctrl;
        if (in.size() >= 8 && in.compare(0, 4, "PMV3") == 0) {
            std::uint32_t n = 0;
            std::memcpy(&n, in.data() + 4, 4);
            if (8 + std::size_t(n) > in.size()) return false;
            comp.data = in.substr(8, n);
            std::size_t at = 8 + n;
            if (at + 4 <= in.size()) {
                std::uint32_t m = 0;
                std::memcpy(&m, in.data() + at, 4);
                if (at + 4 + m <= in.size()) ctrl.data = in.substr(at + 4, m);
            }
        } else {
            comp.data = in;
        }
        if (component_->setState(&comp) != kResultOk) return false;
        if (controller_) {
            comp.pos = 0;
            controller_->setComponentState(&comp);
            if (!ctrl.data.empty()) controller_->setState(&ctrl);
        }
        return true;
    }

    // --- controller & editor (main thread of the hosting process) --------
    bool hasEditor() const override { return controller_ != nullptr; }

    bool createEditor(void* parent, int* w, int* h) override {
        if (!controller_ || view_) return false;
        view_ = controller_->createView(ViewType::kEditor);
        if (!view_) return false;
        if (view_->isPlatformTypeSupported(kPlatformTypeNSView) != kResultTrue) {
            view_->release();
            view_ = nullptr;
            return false;
        }
        frame_.onResize = &onEditorResize;
        view_->setFrame(&frame_);
        ViewRect r;
        view_->getSize(&r);
        if (view_->attached(parent, kPlatformTypeNSView) != kResultOk) {
            view_->release();
            view_ = nullptr;
            return false;
        }
        if (w) *w = r.getWidth();
        if (h) *h = r.getHeight();
        return true;
    }

    void destroyEditor() override {
        if (!view_) return;
        view_->setFrame(nullptr);
        view_->removed();
        view_->release();
        view_ = nullptr;
    }

    bool setParameterByTitle(const std::string& title, double v) override {
        if (!controller_) return false;
        for (int32 i = 0; i < controller_->getParameterCount(); ++i) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(i, info) != kResultOk) continue;
            std::string name;
            for (int k = 0; info.title[k] && k < 128; ++k) name += char(info.title[k]);
            if (strcasecmp(name.c_str(), title.c_str()) != 0) continue;
            // The same path a knob in the editor takes.
            controller_->setParamNormalized(info.id, v);
            handler_.performEdit(info.id, v);
            return true;
        }
        return false;
    }

    // Processor -> controller: meter values the plugin reported.
    void idle() override {
        if (!controller_) return;
        while (auto e = toController_.pop())
            controller_->setParamNormalized(e->id, e->value);
    }

    void stopProcessing() override {
        rt::ScopedThirdParty attribute(&rtAllocs_);
        if (processing_) processor_->setProcessing(false);
        processing_ = false;
    }

    int latencySamples() const override { return latency_; }
    std::string name() const override { return name_; }

private:
    struct Bus {
        std::vector<float> data;
        std::vector<float*> ptr;
    };
    // Single-component plugins implement IEditController on the component;
    // otherwise the controller is its own object, created and connected.
    void setUpController() {
        if (component_->queryInterface(IEditController::iid,
                                       reinterpret_cast<void**>(&controller_)) == kResultOk &&
            controller_) {
            separateController_ = false;
        } else {
            controller_ = nullptr;
            TUID ccid;
            if (component_->getControllerClassId(ccid) != kResultOk) return;
            if (module_->factory->createInstance(ccid, IEditController::iid,
                                                 reinterpret_cast<void**>(&controller_)) !=
                    kResultOk ||
                !controller_)
                return;
            if (controller_->initialize(&gHost) != kResultOk) {
                controller_->release();
                controller_ = nullptr;
                return;
            }
            separateController_ = true;
            component_->queryInterface(IConnectionPoint::iid,
                                       reinterpret_cast<void**>(&componentCP_));
            controller_->queryInterface(IConnectionPoint::iid,
                                        reinterpret_cast<void**>(&controllerCP_));
            if (componentCP_ && controllerCP_) {
                componentCP_->connect(controllerCP_);
                controllerCP_->connect(componentCP_);
            }
        }
        handler_.edits = &edits_;
        controller_->setComponentHandler(&handler_);
        MemStream st;
        if (component_->getState(&st) == kResultOk) {
            st.pos = 0;
            controller_->setComponentState(&st);
        }
    }

    std::shared_ptr<Module> module_;
    IComponent* component_ = nullptr;
    IAudioProcessor* processor_ = nullptr;
    IEditController* controller_ = nullptr;
    IConnectionPoint *componentCP_ = nullptr, *controllerCP_ = nullptr;
    bool separateController_ = false;
    IPlugView* view_ = nullptr;
    ComponentHandler handler_;
    PlugFrame frame_;
    SpscQueue<ParamEdit> edits_{1024}, toController_{4096};
    bool active_ = false, processing_ = false, failed_ = false;
    std::string name_;
    int latency_ = 0, maxBlock_ = 0;
    std::int64_t time_ = 0;
    std::vector<Bus> inBuses_, outBuses_;
    std::vector<AudioBusBuffers> inBufs_, outBufs_;
    ParamChanges paramsIn_, paramsOut_;
    NoEvents eventsIn_, eventsOut_;
    ProcessContext ctx_{};
};

}  // namespace

bool describeVst3(const std::string& path, std::vector<PluginInfo>& out,
                  std::string* error) {
    auto mod = openModule(path, error);
    if (!mod) return false;
    PFactoryInfo fi;
    mod->factory->getFactoryInfo(&fi);
    IPluginFactory2* f2 = nullptr;
    mod->factory->queryInterface(IPluginFactory2::iid,
                                 reinterpret_cast<void**>(&f2));
    for (int32 i = 0; i < mod->factory->countClasses(); ++i) {
        PClassInfo ci;
        if (mod->factory->getClassInfo(i, &ci) != kResultOk) continue;
        if (std::strcmp(ci.category, kVstAudioEffectClass) != 0) continue;
        PluginInfo p{"vst3", path, hexId(ci.cid), ci.name, fi.vendor, ""};
        PClassInfo2 c2;
        if (f2 && f2->getClassInfo2(i, &c2) == kResultOk) {
            p.version = c2.version;
            if (c2.vendor[0]) p.vendor = c2.vendor;
            p.instrument = std::strstr(c2.subCategories, "Instrument") != nullptr ||
                           std::strstr(c2.subCategories, "Generator") != nullptr;
        }
        out.push_back(std::move(p));
    }
    if (f2) f2->release();
    return true;
}

std::unique_ptr<Processor> loadVst3(const std::string& path,
                                    const std::string& id, double fs,
                                    int maxBlock, int channels,
                                    std::string* error) {
    auto mod = openModule(path, error);
    if (!mod) return nullptr;
    auto p = std::make_unique<Vst3Processor>();
    if (!p->init(std::move(mod), id, fs, maxBlock, channels, error))
        return nullptr;
    return p;
}

}  // namespace pm::plugin
