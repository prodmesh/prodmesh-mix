// Routing tests: mix-minus (N-1) buses.
#include <cmath>
#include <vector>

#include "core/rt_guard.h"
#include "dsp/dynamics.h"
#include "engine/engine.h"
#include "test.h"

using namespace pm;

namespace {

constexpr int kBlock = 128;

// Generators 1x -> strips 2x -> Program (30) -> out 0; mix-minus bus 31
// follows Program, excludes strip 21 (the remote), -> out 1.
GraphDesc show() {
    GraphDesc g;
    auto node = [&](NodeId id, NodeKind k) -> NodeDesc& {
        NodeDesc n;
        n.id = id;
        n.kind = k;
        g.nodes.push_back(n);
        return g.nodes.back();
    };
    node(10, NodeKind::Generator).generator = {1, 200, -12, -200, false};
    node(11, NodeKind::Generator).generator = {2, 300, -12, -200, false};
    node(20, NodeKind::Strip);
    node(21, NodeKind::Strip);
    node(30, NodeKind::Bus);
    auto& mm = node(31, NodeKind::Bus);
    mm.mixMinus.follows = 30;
    mm.mixMinus.exclude = {21};
    node(40, NodeKind::Output).ioChannel = 0;
    node(41, NodeKind::Output).ioChannel = 1;
    g.edges = {{10, 20, 0, 0},  {11, 21, 0, 0}, {20, 30, 0, -0.5f},
               {21, 30, 0, 0},  {30, 40, 0, 0}, {31, 41, 0, 0}};
    return g;
}

int derivedCount(const GraphDesc& g, NodeId from, NodeId to) {
    int n = 0;
    for (const auto& e : g.edges) n += e.derived && e.from == from && e.to == to;
    return n;
}

float peak(const std::vector<float>& x, std::size_t from = 0) {
    float m = 0;
    for (std::size_t i = from; i < x.size(); ++i) m = std::max(m, std::fabs(x[i]));
    return m;
}

struct Offline {
    Offline() : engine([] {
          EngineConfig c;
          c.sampleRate = 48000;
          c.maxBlock = kBlock;
          c.numOutputs = 3;
          return c;
      }()) {}
    void run(int blocks) {
        std::vector<float> a(kBlock), b(kBlock), m(kBlock);
        float* ptr[3] = {a.data(), b.data(), m.data()};
        for (int i = 0; i < blocks; ++i) {
            engine.pump();
            {
                rt::ScopedRealtime scope;
                engine.process(nullptr, 0, ptr, 3, kBlock);
            }
            out[0].insert(out[0].end(), a.begin(), a.end());
            out[1].insert(out[1].end(), b.begin(), b.end());
            out[2].insert(out[2].end(), m.begin(), m.end());
        }
    }
    Engine engine;
    std::vector<float> out[3];
};

}  // namespace

TEST(mix_minus_derives_sends) {
    GraphDesc g = show();
    resolveDerived(g);
    CHECK(derivedCount(g, 20, 31) == 1);
    CHECK(derivedCount(g, 21, 31) == 0);  // excluded
    for (const auto& e : g.edges)
        if (e.derived) CHECK(e.pan == -0.5f);  // follows the program image
    const auto n = g.edges.size();
    resolveDerived(g);  // idempotent
    CHECK(g.edges.size() == n);

    // A strip added later joins by itself; one routed elsewhere does not.
    NodeDesc s;
    s.id = 22;
    g.nodes.push_back(s);
    g.edges.push_back({22, 30, 0, 0});
    NodeDesc t;
    t.id = 23;
    g.nodes.push_back(t);
    resolveDerived(g);
    CHECK(derivedCount(g, 22, 31) == 1);
    CHECK(derivedCount(g, 23, 31) == 0);

    // An explicit send overrides the automatic one.
    g.edges.push_back({22, 31, -6, 0});
    resolveDerived(g);
    CHECK(derivedCount(g, 22, 31) == 0);

    // A strip that listens to the mix-minus is never fed by it.
    g.edges.push_back({31, 23, 0, 0});
    g.edges.push_back({23, 30, 0, 0});
    resolveDerived(g);
    CHECK(derivedCount(g, 23, 31) == 0);
}

TEST(mix_minus_excludes_remote_audio) {
    Offline r;
    GraphDesc g = show();
    g.find(20)->mute = true;  // only the remote is live
    CHECK(r.engine.setGraph(g));
    r.run(40);
    CHECK(peak(r.out[0], 20 * kBlock) > 0.1f);   // program has the remote
    CHECK(peak(r.out[1]) == 0.0f);              // its own feed never does

    r.engine.setLevel(20, 0, false);
    r.run(40);
    CHECK(peak(r.out[1], 60 * kBlock) > 0.1f);  // the host reaches the remote

    // Excluding the host too empties the feed (with a fade, not a cut).
    g.find(20)->mute = false;
    g.find(31)->mixMinus.exclude.push_back(20);
    CHECK(r.engine.setGraph(g));
    r.run(80);
    CHECK(peak(r.out[1], 100 * kBlock) == 0.0f);
    CHECK(rt::violations() == 0);
}

TEST(muted_send_is_silent_and_keeps_its_level) {
    Offline r;
    GraphDesc g = show();
    g.find(31)->mixMinus = {};  // a plain aux with one explicit send
    g.edges.push_back({20, 31, -6, 0, false, true, true});
    CHECK(r.engine.setGraph(g));
    r.run(40);
    CHECK(peak(r.out[1]) == 0.0f);
    g.edges.back().mute = false;
    CHECK(r.engine.setGraph(g));
    r.run(40);
    const float p = peak(r.out[1], 60 * kBlock);
    CHECK_MSG(std::fabs(dsp::gainToDb(p) - (-12 - 6)) < 0.5f,
              "send level " + std::to_string(dsp::gainToDb(p)));
    CHECK(rt::violations() == 0);
}

// A pre-fader send ignores the fader but not the mute, and switching a
// send between pre and post fades instead of stepping.
TEST(pre_fader_send) {
    Offline r;
    GraphDesc g = show();
    g.find(31)->mixMinus = {};
    g.find(21)->mute = true;  // only the host (strip 20) is live
    g.edges.push_back({20, 31, 0, 0, false, true, false, true});
    g.find(20)->faderDb = -30;
    CHECK(r.engine.setGraph(g));
    r.run(40);
    const float pre = dsp::gainToDb(peak(r.out[1], 20 * kBlock));
    const float post = dsp::gainToDb(peak(r.out[0], 20 * kBlock));
    CHECK_MSG(std::fabs(pre - (-12)) < 0.5f, "pre " + std::to_string(pre));
    CHECK_MSG(std::fabs(post - (-12 - 30)) < 0.5f, "post " + std::to_string(post));

    // Pre -> post: the send drops by the fader, through a fade.
    const std::size_t from = r.out[1].size();
    g.edges.back().preFader = false;
    CHECK(r.engine.setGraph(g));
    r.run(40);
    CHECK(dsp::gainToDb(peak(r.out[1], from + 30 * kBlock)) < post + 0.5f);
    float step = 0;
    for (std::size_t i = from + 1; i < r.out[1].size(); ++i)
        step = std::max(step, std::fabs(r.out[1][i] - r.out[1][i - 1]));
    // A 200 Hz sine at -12 dBFS moves at most ~0.005 per sample.
    CHECK_MSG(step < 0.01f, "step " + std::to_string(step));

    // Mute kills a pre-fader send too.
    g.edges.back().preFader = true;
    g.find(20)->mute = true;
    CHECK(r.engine.setGraph(g));
    r.run(60);
    CHECK(peak(r.out[1], r.out[1].size() - 20 * kBlock) == 0.0f);
    CHECK(rt::violations() == 0);
}

// --- scenes ------------------------------------------------------------------

TEST(scene_recalls_the_mix_not_the_structure) {
    GraphDesc stored = show();
    stored.find(20)->faderDb = -10;
    stored.find(21)->mute = true;
    stored.edges[2].pan = 0.7f;           // 20 -> 30
    stored.edges[0].gainDb = 6;           // input gain of 20
    stored.find(31)->mixMinus.exclude = {};  // remote back in the feed

    GraphDesc now = show();
    NodeDesc extra;  // a channel added after the scene was stored
    extra.id = 22;
    now.nodes.push_back(extra);
    now.edges.push_back({22, 30, 0, 0});
    resolveDerived(now);
    CHECK(!sceneMatches(now, stored));

    const GraphDesc r = applyScene(now, stored);
    CHECK(r.find(20)->faderDb == -10);
    CHECK(r.find(21)->mute);
    CHECK(r.edges[2].pan == 0.7f);
    CHECK(r.edges[0].gainDb == 6);
    CHECK(r.find(31)->mixMinus.exclude.empty());
    CHECK(r.find(22) != nullptr);  // structure untouched
    CHECK(r.nodes.size() == now.nodes.size());
    CHECK(sceneMatches(r, stored));
}

TEST(recall_safe_channel_is_left_alone) {
    GraphDesc stored = show();
    stored.find(21)->faderDb = -20;
    stored.edges[1].gainDb = 9;              // 11 -> 21: input gain of 21
    stored.find(31)->mixMinus.exclude = {};  // would put 21 back in
    GraphDesc now = show();
    now.find(21)->recallSafe = true;
    const GraphDesc r = applyScene(now, stored);
    CHECK(r.find(21)->faderDb == 0);
    CHECK(r.edges[1].gainDb == 0);
    CHECK(r.find(31)->mixMinus.exclude == std::vector<NodeId>{21});
}

// A recall with a fade glides the fader; a mute in the same recall is fast.
TEST(scene_recall_glides_faders) {
    Offline r;
    GraphDesc g = show();
    g.find(21)->mute = true;
    CHECK(r.engine.setGraph(g));
    r.run(40);
    g.find(20)->faderDb = -20;
    CHECK(r.engine.setGraph(g, nullptr, 500));  // 500 ms glide
    const std::size_t from = r.out[0].size();
    r.run(int(0.25 * 48000 / kBlock));  // halfway through
    // Linear in gain: halfway between 1.0 and 0.1 is 0.55 (-5.2 dB).
    const float mid = peak(r.out[0], r.out[0].size() - 4 * kBlock);
    CHECK_MSG(std::fabs(mid / dsp::dbToGain(-12) - 0.55f) < 0.05f,
              "mid-glide gain " + std::to_string(mid / dsp::dbToGain(-12)));
    r.run(int(0.35 * 48000 / kBlock));
    const float end = peak(r.out[0], r.out[0].size() - 4 * kBlock);
    CHECK(std::fabs(dsp::gainToDb(end) - (-32)) < 0.5f);
    (void)from;

    // Mute in a gliding recall: silent within a few ms, not 500.
    g.find(20)->mute = true;
    g.find(20)->faderDb = 0;
    CHECK(r.engine.setGraph(g, nullptr, 500));
    r.run(int(0.02 * 48000 / kBlock) + 2);
    CHECK(peak(r.out[0], r.out[0].size() - kBlock) == 0.0f);
    CHECK(rt::violations() == 0);
}


// --- solo / monitor ------------------------------------------------------------

namespace {
// show() plus a monitor bus (33) following Program, on output 2.
GraphDesc withMonitor(bool pfl = true) {
    GraphDesc g = show();
    NodeDesc mon;
    mon.id = 33;
    mon.kind = NodeKind::Bus;
    mon.monitor = {true, 30, pfl};
    g.nodes.push_back(mon);
    NodeDesc o;
    o.id = 43;
    o.kind = NodeKind::Output;
    o.ioChannel = 2;
    g.nodes.push_back(o);
    g.edges.push_back({33, 43, 0, 0});
    return g;
}
float peakDb(const std::vector<float>& x, std::size_t from) {
    return dsp::gainToDb(peak(x, from));
}
}  // namespace

TEST(monitor_follows_program_until_something_is_soloed) {
    GraphDesc g = withMonitor();
    resolveDerived(g);
    CHECK(derivedCount(g, 30, 33) == 1);
    g.find(20)->solo = true;
    resolveDerived(g);
    CHECK(derivedCount(g, 30, 33) == 0);
    CHECK(derivedCount(g, 20, 33) == 1);
    CHECK(derivedCount(g, 21, 33) == 0);
}

// Tier 0: solo is for the operator's ears. Program must not change by a
// single bit, whatever is soloed.
TEST(solo_leaves_program_bit_identical) {
    Offline a, b;
    GraphDesc g = withMonitor();
    CHECK(a.engine.setGraph(g));
    CHECK(b.engine.setGraph(g));
    a.run(20);
    b.run(20);
    g.find(21)->solo = true;
    CHECK(b.engine.setGraph(g));  // b solos the remote mid-stream
    a.run(60);
    b.run(60);
    CHECK(a.out[0] == b.out[0]);
    CHECK(a.out[1] == b.out[1]);
    CHECK(a.out[2] != b.out[2]);  // the monitor did change
    CHECK(rt::violations() == 0);
}

// PFL ignores the channel's fader; AFL follows it. Mute applies to both.
TEST(pfl_and_afl) {
    for (bool pfl : {true, false}) {
        Offline r;
        GraphDesc g = withMonitor(pfl);
        g.find(21)->mute = true;     // only strip 20 is live
        g.find(20)->faderDb = -20;
        g.find(20)->solo = true;
        CHECK(r.engine.setGraph(g));
        r.run(40);
        const float mon = peakDb(r.out[2], 20 * kBlock);
        const float want = pfl ? -12.0f : -32.0f;
        CHECK_MSG(std::fabs(mon - want) < 0.5f,
                  std::string(pfl ? "PFL " : "AFL ") + std::to_string(mon));
    }
}

TEST(solo_is_not_part_of_a_scene) {
    GraphDesc stored = withMonitor();
    stored.find(20)->solo = true;
    GraphDesc now = withMonitor();
    CHECK(!applyScene(now, stored).find(20)->solo);
    CHECK(sceneMatches(now, stored));
}
