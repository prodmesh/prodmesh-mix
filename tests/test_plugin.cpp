// Plugin hosting against the in-tree pm-test.clap.
#include <chrono>
#include <cmath>
#include <thread>
#include <cstring>
#include <vector>

#include "core/rt_guard.h"
#include "engine/engine.h"
#include "core/base64.h"
#include "plugin/plugin.h"
#include "test.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace pm;

namespace {

constexpr int kBlock = 128;

struct Counters {
    int starts = 0, stops = 0, violations = 0, live = 0;
};

// Reads the test plugin's lifecycle counters from the loaded bundle.
Counters counters() {
    Counters c;
#if defined(__APPLE__)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(PM_TEST_CLAP),
        CFIndex(std::strlen(PM_TEST_CLAP)), true);
    CFBundleRef b = CFBundleCreate(nullptr, url);
    CFRelease(url);
    CFBundleLoadExecutable(b);
    using Fn = void (*)(int*, int*, int*, int*);
    if (auto fn = reinterpret_cast<Fn>(
            CFBundleGetFunctionPointerForName(b, CFSTR("pm_test_counters"))))
        fn(&c.starts, &c.stops, &c.violations, &c.live);
    CFRelease(b);
#endif
    return c;
}

GraphDesc sine() {
    GraphDesc g;
    NodeDesc gen;
    gen.id = 1;
    gen.kind = NodeKind::Generator;
    gen.generator = {7, 100, -12, -200, false};
    NodeDesc strip;
    strip.id = 2;
    NodeDesc out;
    out.id = 3;
    out.kind = NodeKind::Output;
    g.nodes = {gen, strip, out};
    g.edges = {{1, 2, 0, 0}, {2, 3, 0, 0}};
    return g;
}

GraphDesc withPlugin(GraphDesc g, const char* id, const char* path = PM_TEST_CLAP) {
    InsertDesc ins;
    ins.type = InsertDesc::Type::Plugin;
    ins.format = "clap";
    ins.path = path;
    ins.pluginId = id;
    g.find(2)->processing.inserts.push_back(ins);
    return g;
}

std::vector<float> run(Engine& e, int blocks, const std::function<void(int)>& before = {}) {
    std::vector<float> all, buf(kBlock);
    float* out[1] = {buf.data()};
    for (int b = 0; b < blocks; ++b) {
        if (before) before(b);
        e.pump();
        {
            rt::ScopedRealtime scope;
            e.process(nullptr, 0, out, 1, kBlock);
        }
        all.insert(all.end(), buf.begin(), buf.end());
    }
    return all;
}

EngineConfig cfg() {
    EngineConfig c;
    c.maxBlock = kBlock;
    c.numOutputs = 1;
    return c;
}

float peak(const std::vector<float>& x, std::size_t from, std::size_t to) {
    float m = 0;
    for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(x[i]));
    return m;
}

}  // namespace

TEST(clap_bundle_describes_its_plugins) {
    std::vector<plugin::PluginInfo> list;
    std::string err;
    CHECK_MSG(plugin::describeBundle(PM_TEST_CLAP, list, &err), err);
    CHECK(list.size() == 5);
    CHECK(list[0].id == "com.prodmesh.test.gain" && list[0].format == "clap");
}

TEST(clap_plugin_processes_in_a_live_insert) {
    const auto before = counters();
    {
        Engine e(cfg());
        CHECK(e.setGraph(sine()));
        const auto x = run(e, 120, [&](int b) {
            if (b == 20) CHECK(e.setGraph(withPlugin(sine(), "com.prodmesh.test.gain")));
            if (b == 70) CHECK(e.setGraph(sine()));  // and out again
        });
        CHECK(e.warnings().empty());
        const float full = peak(x, 0, 20 * kBlock);
        const float half = peak(x, 40 * kBlock, 70 * kBlock);
        CHECK(std::fabs(half / full - 0.5f) < 0.01f);  // -6 dB in the insert
        CHECK(std::fabs(peak(x, 100 * kBlock, 120 * kBlock) / full - 1) < 0.01f);
        CHECK(rt::violations() == 0);
        // Removed and cleaned up: stopped on the audio thread, destroyed.
        const auto mid = counters();
        CHECK(mid.starts - before.starts == 1);
        CHECK(mid.stops - before.stops == 1);
    }
    const auto after = counters();
    CHECK_MSG(after.violations == before.violations,
              "host broke the CLAP lifecycle rules");
    CHECK(after.live == before.live);
}

TEST(engine_teardown_with_a_running_plugin_is_clean) {
    const auto before = counters();
    {
        Engine e(cfg());
        CHECK(e.setGraph(withPlugin(sine(), "com.prodmesh.test.gain")));
        run(e, 10);
    }
    const auto after = counters();
    CHECK(after.live == before.live);
    // Processing was never stopped on the audio thread (the engine just
    // went away); the adapter's destructor is the backstop and must not
    // deactivate a plugin that is still processing... recorded as a
    // violation by the test plugin if it did.
    CHECK(after.violations == before.violations);
}

TEST(missing_plugin_passes_through_with_a_warning) {
    Engine a(cfg()), b(cfg());
    CHECK(a.setGraph(sine()));
    CHECK(b.setGraph(withPlugin(sine(), "nope", "/nonexistent/Missing.clap")));
    const auto x = run(a, 40), y = run(b, 40);
    CHECK(x == y);
    CHECK(b.warnings().size() == 1);
}

TEST(clap_latency_is_reported) {
    std::string err;
    auto p = plugin::load("clap", PM_TEST_CLAP, "com.prodmesh.test.delay", 48000,
                          kBlock, 1, &err);
    CHECK_MSG(p != nullptr, err);
    CHECK(p->latencySamples() == 64);
}

namespace {

// gen(1) -> A(2) -> bus(4) -> out(5)
//        \-> B(3) -/
GraphDesc parallel(bool delayOnA) {
    GraphDesc g;
    NodeDesc gen;
    gen.id = 1;
    gen.kind = NodeKind::Generator;
    gen.generator = {11, 150, -12, -30, false};
    NodeDesc a, b, bus, out;
    a.id = 2;
    b.id = 3;
    bus.id = 4;
    bus.kind = NodeKind::Bus;
    out.id = 5;
    out.kind = NodeKind::Output;
    if (delayOnA)
        a.processing.inserts.push_back(InsertDesc::plugin(
            "clap", PM_TEST_CLAP, "com.prodmesh.test.delay"));
    g.nodes = {gen, a, b, bus, out};
    g.edges = {{1, 2, 0, 0}, {1, 3, 0, 0}, {2, 4, 0, 0}, {3, 4, 0, 0},
               {4, 5, 0, 0}};
    return g;
}

}  // namespace

TEST(pdc_aligns_parallel_paths_exactly) {
    Engine ref(cfg()), pdc(cfg());
    CHECK(ref.setGraph(parallel(false)));
    CHECK(pdc.setGraph(parallel(true)));
    const auto x = run(ref, 40), y = run(pdc, 40);
    CHECK(pdc.latencyOf(2) == 64 && pdc.latencyOf(3) == 0);
    CHECK(pdc.latencyOf(4) == 64 && pdc.latencyOf(5) == 64);
    // The delay view: A's chain adds 64 ("dly"); B is held back 64 to meet
    // it at the bus ("cmp"); A itself needs no compensation.
    const auto a = pdc.delayInfo(2), b = pdc.delayInfo(3);
    CHECK(a.chain == 64 && b.chain == 0);
    CHECK(b.compensation.size() == 1 && b.compensation[0] == std::make_pair(NodeId(4), 64));
    CHECK(a.compensation.size() == 1 && a.compensation[0].second == 0);
    // Both paths arrive together: the bus is the reference, 64 samples late.
    for (std::size_t n = 64; n < x.size(); ++n)
        if (y[n] != x[n - 64]) {
            CHECK_MSG(false, "misaligned at sample " + std::to_string(n));
            break;
        }
}

TEST(pdc_change_is_continuous) {
    Engine e(cfg());
    CHECK(e.setGraph(parallel(false)));
    const auto x = run(e, 80, [&](int b) {
        if (b == 20) CHECK(e.setGraph(parallel(true)));
    });
    float steady = 0;
    for (std::size_t i = 1; i < 19 * kBlock; ++i)
        steady = std::max(steady, std::fabs(x[i] - x[i - 1]));
    float worst = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
        worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
    CHECK_MSG(worst <= steady * 1.05f, "step when compensation changed: " +
                                           std::to_string(worst / steady));
    CHECK(e.settled());
}

TEST(plugin_allocations_are_attributed_not_counted_as_engine_bugs) {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return;  // TSan's runtime owns operator new for the plugin's image
#endif
#endif
    Engine e(cfg());
    CHECK(e.setGraph(withPlugin(sine(), "com.prodmesh.test.alloc")));
    run(e, 20);
    CHECK_MSG(rt::violations() == 0, "a plugin's allocation was blamed on the engine");
    const auto issues = e.pluginRtIssues();
    CHECK(issues.size() == 1);
    CHECK(issues[0].node == 2 && issues[0].plugin == "PM Test Alloc");
    CHECK(issues[0].allocations >= 20);
}

// --- sandbox -----------------------------------------------------------------

namespace {

// Rendered at roughly real time: a sandbox host is a separate process, and
// the engine never waits for it.
std::vector<float> runPaced(Engine& e, int blocks) {
    std::vector<float> all, buf(kBlock);
    float* out[1] = {buf.data()};
    for (int b = 0; b < blocks; ++b) {
        e.pump();
        {
            rt::ScopedRealtime scope;
            e.process(nullptr, 0, out, 1, kBlock);
        }
        all.insert(all.end(), buf.begin(), buf.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return all;
}

GraphDesc withSandboxed(GraphDesc g, const char* id) {
    g.find(2)->processing.inserts.push_back(
        InsertDesc::plugin("clap", PM_TEST_CLAP, id, true));
    return g;
}

}  // namespace

TEST(sandboxed_plugin_is_exact_one_block_later) {
    Engine in(cfg()), out(cfg());
    CHECK(in.setGraph(withPlugin(sine(), "com.prodmesh.test.gain")));
    CHECK(out.setGraph(withSandboxed(sine(), "com.prodmesh.test.gain")));
    CHECK_MSG(out.warnings().empty(), out.warnings().empty() ? "" : out.warnings()[0]);
    const auto x = runPaced(in, 200), y = runPaced(out, 200);
    std::size_t bad = 0;
    for (std::size_t n = kBlock; n < x.size(); ++n) bad += y[n] != x[n - kBlock];
    CHECK_MSG(bad == 0, std::to_string(bad) + " samples differ");
    CHECK(out.latencyOf(2) == kBlock);
    CHECK(rt::violations() == 0);
}

TEST(sandboxed_latency_includes_the_exchange_block) {
    Engine e(cfg());
    CHECK(e.setGraph(withSandboxed(sine(), "com.prodmesh.test.delay")));
    CHECK(e.latencyOf(2) == 64 + kBlock);
}

TEST(sandboxed_plugin_crash_keeps_audio_running) {
    Engine e(cfg());
    CHECK(e.setGraph(withSandboxed(sine(), "com.prodmesh.test.crash")));
    // The plugin is unity until it dies on block 50; after that the insert
    // passes last block's input — so the output stays the signal, one block
    // late, with no gap or step, through the crash and the restart.
    Engine ref(cfg());
    CHECK(ref.setGraph(sine()));
    const auto x = runPaced(ref, 300), y = runPaced(e, 300);
    std::size_t bad = 0;
    for (std::size_t n = kBlock; n < x.size(); ++n) bad += y[n] != x[n - kBlock];
    CHECK_MSG(bad == 0, std::to_string(bad) + " samples differ across the crash");
    const auto events = e.drainEvents();
    CHECK(!events.empty());
    // Either way the host is replaced: it crashes, or (under a sanitizer
    // that takes its time writing a report) the watchdog kills it first.
    bool recovered = false;
    for (const auto& ev : events)
        recovered = recovered || ev.find("crashed") != std::string::npos ||
                    ev.find("stopped responding") != std::string::npos;
    CHECK_MSG(recovered, events.empty() ? "no events" : events[0]);
    if (std::getenv("PM_TEST_VERBOSE"))
        for (const auto& ev : events) std::printf("        event: %s\n", ev.c_str());
    CHECK(rt::violations() == 0);
}

TEST(isolated_chain_shares_one_host_and_one_block) {
    Engine in(cfg()), out(cfg());
    auto twoIn = withPlugin(withPlugin(sine(), "com.prodmesh.test.gain"),
                            "com.prodmesh.test.gain");
    auto twoOut = withSandboxed(withSandboxed(sine(), "com.prodmesh.test.gain"),
                                "com.prodmesh.test.gain");
    CHECK(in.setGraph(twoIn));
    CHECK(out.setGraph(twoOut));
    CHECK(out.latencyOf(2) == kBlock);  // one block for the pair
    const auto x = runPaced(in, 120), y = runPaced(out, 120);
    std::size_t bad = 0;
    for (std::size_t n = kBlock; n < x.size(); ++n) bad += y[n] != x[n - kBlock];
    CHECK_MSG(bad == 0, std::to_string(bad) + " samples differ");
}

// --- plugin state ------------------------------------------------------------

namespace {

std::string gainState(float g) {
    return pm::base64Encode(std::string(reinterpret_cast<const char*>(&g), sizeof g));
}

float stateGain(const std::string& b64) {
    std::string raw;
    float g = -1;
    if (pm::base64Decode(b64, raw) && raw.size() == sizeof g) std::memcpy(&g, raw.data(), sizeof g);
    return g;
}

GraphDesc gainWithState(bool isolated, float g, const char* id = "com.prodmesh.test.gain") {
    auto graph = isolated ? withSandboxed(sine(), id) : withPlugin(sine(), id);
    graph.find(2)->processing.inserts[0].state = gainState(g);
    return graph;
}

}  // namespace

TEST(plugin_state_is_applied_and_collected) {
    for (bool isolated : {false, true}) {
        Engine ref(cfg()), e(cfg());
        CHECK(ref.setGraph(sine()));
        CHECK(e.setGraph(gainWithState(isolated, 0.25f)));
        const auto x = runPaced(ref, 60), y = runPaced(e, 60);
        const std::size_t d = isolated ? kBlock : 0;
        CHECK_MSG(std::fabs(peak(y, 20 * kBlock + d, 60 * kBlock) /
                                peak(x, 20 * kBlock, 60 * kBlock - d) - 0.25f) < 0.01f,
                  isolated ? "isolated state not applied" : "state not applied");
        e.collectPluginStates(500);
        CHECK(stateGain(e.graph().find(2)->processing.inserts[0].state) == 0.25f);
    }
}

TEST(state_change_does_not_rebuild_the_plugin) {
    auto a = gainWithState(false, 0.25f), b = gainWithState(false, 0.75f);
    CHECK(sameStructure(a.find(2)->processing, b.find(2)->processing));
}

TEST(crashed_host_comes_back_with_its_settings) {
    Engine e(cfg()), ref(cfg());
    CHECK(e.setGraph(gainWithState(true, 0.5f, "com.prodmesh.test.crash")));
    CHECK(ref.setGraph(sine()));
    // Render until the host has crashed (block 50) and been restarted —
    // immediately when it dies, or via the 0.5 s watchdog when it is slow
    // to die (a sanitizer writing its report) — then 40 blocks more.
    std::vector<float> x, y, buf(kBlock);
    float* out[1] = {buf.data()};
    std::vector<std::string> events;
    int restartedAt = -1;
    for (int b = 0; b < 2000 && (restartedAt < 0 || b < restartedAt + 40); ++b) {
        for (auto* eng : {&ref, &e}) {
            eng->pump();
            {
                rt::ScopedRealtime scope;
                eng->process(nullptr, 0, out, 1, kBlock);
            }
            (eng == &ref ? x : y).insert((eng == &ref ? x : y).end(), buf.begin(), buf.end());
        }
        for (auto& ev : e.drainEvents()) {
            if (restartedAt < 0 && ev.find("last saved settings") != std::string::npos)
                restartedAt = b;
            events.push_back(ev);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (std::getenv("PM_TEST_VERBOSE"))
        for (const auto& ev : events) std::printf("        event: %s\n", ev.c_str());
    CHECK_MSG(restartedAt > 0, "never restarted with its settings");
    if (restartedAt <= 0) return;
    const std::size_t from = std::size_t(restartedAt + 10) * kBlock,
                      to = std::size_t(restartedAt + 38) * kBlock;
    const float ratio = peak(y, from, to) / peak(x, from - kBlock, to - kBlock);
    CHECK_MSG(std::fabs(ratio - 0.5f) < 0.01f,
              "after restart the gain ratio is " + std::to_string(ratio));
}

TEST(hung_host_is_killed_and_restarted) {
    Engine e(cfg());
    CHECK(e.setGraph(withSandboxed(sine(), "com.prodmesh.test.hang")));
    bool killed = false;
    std::vector<float> buf(kBlock);
    float* out[1] = {buf.data()};
    // Hangs at block 50; 0.5 s of audio (~188 blocks) later it is killed.
    for (int b = 0; b < 600 && !killed; ++b) {
        e.pump();
        {
            rt::ScopedRealtime scope;
            e.process(nullptr, 0, out, 1, kBlock);
        }
        for (const auto& ev : e.drainEvents())
            killed = killed || ev.find("stopped responding") != std::string::npos;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_MSG(killed, "hung host never killed");
    CHECK(rt::violations() == 0);
}

// Editing a chain (here: adding an insert in front) builds a new instance;
// the plugin must keep the settings it has now, isolated or not.
TEST(chain_edit_keeps_live_plugin_settings) {
    for (bool isolated : {false, true}) {
        Engine e(cfg()), ref(cfg());
        CHECK(ref.setGraph(sine()));
        CHECK(e.setGraph(gainWithState(isolated, 0.25f)));
        runPaced(ref, 40);
        runPaced(e, 40);
        auto g = gainWithState(isolated, 0.25f);
        auto& ins = g.find(2)->processing.inserts;
        ins[0].state.clear();  // the client never saw the state
        ins.insert(ins.begin(), InsertDesc::gain(0));
        CHECK(e.setGraph(g));
        const auto x = runPaced(ref, 80), y = runPaced(e, 80);
        const std::size_t d = isolated ? kBlock : 0;
        const float ratio = peak(y, 50 * kBlock + d, 80 * kBlock) /
                            peak(x, 50 * kBlock, 80 * kBlock - d);
        CHECK_MSG(std::fabs(ratio - 0.25f) < 0.01f,
                  std::string(isolated ? "isolated" : "in-engine") +
                      ": gain after the edit " + std::to_string(ratio));
    }
}
