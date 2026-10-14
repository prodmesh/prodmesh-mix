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
          c.numOutputs = 2;
          return c;
      }()) {}
    void run(int blocks) {
        std::vector<float> a(kBlock), b(kBlock);
        float* ptr[2] = {a.data(), b.data()};
        for (int i = 0; i < blocks; ++i) {
            engine.pump();
            {
                rt::ScopedRealtime scope;
                engine.process(nullptr, 0, ptr, 2, kBlock);
            }
            out[0].insert(out[0].end(), a.begin(), a.end());
            out[1].insert(out[1].end(), b.begin(), b.end());
        }
    }
    Engine engine;
    std::vector<float> out[2];
};

}  // namespace

TEST(mix_minus_derives_sends) {
    GraphDesc g = show();
    resolveMixMinus(g);
    CHECK(derivedCount(g, 20, 31) == 1);
    CHECK(derivedCount(g, 21, 31) == 0);  // excluded
    for (const auto& e : g.edges)
        if (e.derived) CHECK(e.pan == -0.5f);  // follows the program image
    const auto n = g.edges.size();
    resolveMixMinus(g);  // idempotent
    CHECK(g.edges.size() == n);

    // A strip added later joins by itself; one routed elsewhere does not.
    NodeDesc s;
    s.id = 22;
    g.nodes.push_back(s);
    g.edges.push_back({22, 30, 0, 0});
    NodeDesc t;
    t.id = 23;
    g.nodes.push_back(t);
    resolveMixMinus(g);
    CHECK(derivedCount(g, 22, 31) == 1);
    CHECK(derivedCount(g, 23, 31) == 0);

    // An explicit send overrides the automatic one.
    g.edges.push_back({22, 31, -6, 0});
    resolveMixMinus(g);
    CHECK(derivedCount(g, 22, 31) == 0);

    // A strip that listens to the mix-minus is never fed by it.
    g.edges.push_back({31, 23, 0, 0});
    g.edges.push_back({23, 30, 0, 0});
    resolveMixMinus(g);
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
