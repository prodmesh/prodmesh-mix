// pm-test.clap: plugins the host tests drive.
//   com.prodmesh.test.gain     x0.5, stereo
//   com.prodmesh.test.delay    64-sample delay, reports 64 samples latency
//   com.prodmesh.test.alloc    unity, but allocates in process() (a plugin
//                              that is not real-time safe)
//   com.prodmesh.test.crash    unity, segfaults on its 50th process() call
//                              (sandbox tests only!)
// Every instance also checks the host against CLAP's lifecycle rules and
// counts violations; tests read them through pm_test_counters(). Like many
// commercial plugins, they declare a mono sidechain input and expect the
// host to supply a buffer for it.
#include <clap/clap.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace {

std::atomic<int> gStarts{0}, gStops{0}, gViolations{0}, gLive{0};

struct Plugin {
    clap_plugin_t clap{};
    const clap_host_t* host = nullptr;
    bool delay = false, alloc = false, crash = false;
    int calls = 0;
    bool active = false, processing = false;
    std::vector<float> line[2];
    std::size_t pos = 0;
};

constexpr int kDelay = 64;

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

bool init(const clap_plugin_t*) { return true; }
void destroy(const clap_plugin_t* p) {
    auto* s = self(p);
    if (s->active) ++gViolations;  // must deactivate first
    --gLive;
    delete s;
}
bool activate(const clap_plugin_t* p, double, uint32_t, uint32_t) {
    auto* s = self(p);
    if (s->active) ++gViolations;
    s->active = true;
    for (auto& l : s->line) l.assign(kDelay, 0.0f);
    return true;
}
void deactivate(const clap_plugin_t* p) {
    auto* s = self(p);
    if (!s->active || s->processing) ++gViolations;
    s->active = false;
}
bool startProcessing(const clap_plugin_t* p) {
    auto* s = self(p);
    if (!s->active || s->processing) ++gViolations;
    s->processing = true;
    ++gStarts;
    return true;
}
void stopProcessing(const clap_plugin_t* p) {
    auto* s = self(p);
    if (!s->processing) ++gViolations;
    s->processing = false;
    ++gStops;
}
void reset(const clap_plugin_t*) {}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) {
    auto* s = self(p);
    if (!s->processing) ++gViolations;
    if (pr->audio_inputs_count < 2 || !pr->audio_inputs[1].data32 ||
        pr->audio_inputs[1].data32[0][0] != 0.0f)
        ++gViolations;  // sidechain missing (or not silent)
    if (s->crash && ++s->calls == 50) {
        volatile int* null = nullptr;
        *null = 1;  // the bug a sandbox exists for
    }
    if (s->alloc) {
        int* volatile leak = new int(1);  // what a careless plugin does
        delete leak;
    }
    const auto n = pr->frames_count;
    for (uint32_t c = 0; c < 2; ++c) {
        const float* in = pr->audio_inputs[0].data32[c];
        float* out = pr->audio_outputs[0].data32[c];
        for (uint32_t i = 0; i < n; ++i) {
            if (s->delay) {
                const float x = in[i];
                out[i] = s->line[c][(s->pos + i) % kDelay];
                s->line[c][(s->pos + i) % kDelay] = x;
            } else {
                out[i] = (s->alloc || s->crash) ? in[i] : in[i] * 0.5f;
            }
        }
    }
    s->pos = (s->pos + n) % kDelay;
    return CLAP_PROCESS_CONTINUE;
}

uint32_t portCount(const clap_plugin_t*, bool isInput) { return isInput ? 2 : 1; }
bool portGet(const clap_plugin_t*, uint32_t index, bool isInput,
             clap_audio_port_info_t* info) {
    if (index > (isInput ? 1u : 0u)) return false;
    const bool side = index == 1;
    info->id = isInput ? index : 10;
    std::strcpy(info->name, side ? "Sidechain" : "Main");
    info->flags = side ? 0 : CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = side ? 1 : 2;
    info->port_type = side ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts{portCount, portGet};

uint32_t latencyGet(const clap_plugin_t* p) { return self(p)->delay ? kDelay : 0; }
const clap_plugin_latency_t kLatency{latencyGet};

const void* getExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    return nullptr;
}
void onMainThread(const clap_plugin_t*) {}

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t kDescs[] = {
    {CLAP_VERSION, "com.prodmesh.test.gain", "PM Test Gain", "ProdMesh", "", "",
     "", "1.0", "", kFeatures},
    {CLAP_VERSION, "com.prodmesh.test.delay", "PM Test Delay", "ProdMesh", "",
     "", "", "1.0", "", kFeatures},
    {CLAP_VERSION, "com.prodmesh.test.alloc", "PM Test Alloc", "ProdMesh", "",
     "", "", "1.0", "", kFeatures},
    {CLAP_VERSION, "com.prodmesh.test.crash", "PM Test Crash", "ProdMesh", "",
     "", "", "1.0", "", kFeatures},
};
constexpr uint32_t kCount = 4;

uint32_t count(const clap_plugin_factory_t*) { return kCount; }
const clap_plugin_descriptor_t* desc(const clap_plugin_factory_t*, uint32_t i) {
    return i < kCount ? &kDescs[i] : nullptr;
}
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t* host,
                            const char* id) {
    int which = -1;
    for (uint32_t i = 0; i < kCount; ++i)
        if (!std::strcmp(id, kDescs[i].id)) which = int(i);
    if (which < 0) return nullptr;
    auto* s = new Plugin;
    s->host = host;
    s->delay = which == 1;
    s->alloc = which == 2;
    s->crash = which == 3;
    s->clap = {&kDescs[which], s,       init,           destroy,
               activate,       deactivate, startProcessing, stopProcessing,
               reset,          process, getExtension,  onMainThread};
    ++gLive;
    return &s->clap;
}
const clap_plugin_factory_t kFactory{count, desc, create};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* getFactory(const char* id) {
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
}

}  // namespace

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION, entryInit,
                                                 entryDeinit, getFactory};
CLAP_EXPORT void pm_test_counters(int* starts, int* stops, int* violations,
                                  int* live) {
    *starts = gStarts;
    *stops = gStops;
    *violations = gViolations;
    *live = gLive;
}
}
