// Engine tests: the Tier 0 live-edit guarantees (T0-3) as executable checks.
//
//   null tests     an edit leaves every path it does not feed bit-identical
//   continuity     edits on the edited path fade, never step
//   determinism    parallel and serial renders are bit-identical
//   rt safety      no allocation on the audio thread, edits included
#include <cmath>
#include <functional>
#include <vector>

#include "bench_graph.h"
#include "core/rt_guard.h"
#include "engine/engine.h"
#include "test.h"

using namespace pm;

namespace {

constexpr int kBlock = 128;
constexpr double kRate = 48000;

// Drives an engine block by block on the calling thread, which plays both
// control and audio roles (sequentially, as the real threads would).
struct Offline {
    explicit Offline(int workers = 0, int outputs = 4)
        : engine([&] {
              EngineConfig c;
              c.sampleRate = kRate;
              c.maxBlock = kBlock;
              c.numOutputs = outputs;
              c.workerThreads = workers;
              return c;
          }()),
          out(std::size_t(outputs)) {}

    void run(int blocks, const std::function<void(int)>& before = {}) {
        const int nout = int(out.size());
        std::vector<std::vector<float>> buf(nout, std::vector<float>(kBlock));
        std::vector<float*> ptr(nout);
        for (int c = 0; c < nout; ++c) ptr[c] = buf[c].data();
        for (int b = 0; b < blocks; ++b) {
            if (before) before(b);
            engine.pump();
            {
                rt::ScopedRealtime scope;
                engine.process(nullptr, 0, ptr.data(), nout, kBlock);
            }
            for (int c = 0; c < nout; ++c)
                out[c].insert(out[c].end(), buf[c].begin(), buf[c].end());
        }
    }

    Engine engine;
    std::vector<std::vector<float>> out;
};

GraphDesc reference(int paths = 32) {
    bench::RefParams p;
    p.paths = paths;
    p.heavyInserts = 2;
    p.heavyOpsPerSample = 8;
    return bench::makeReferenceGraph(p);
}

// One sine generator -> strip -> mono output; no dynamics, so any step in
// the output comes from the engine.
GraphDesc sineGraph() {
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

float maxDelta(const std::vector<float>& x, std::size_t from = 1,
               std::size_t to = SIZE_MAX) {
    float m = 0;
    for (std::size_t i = std::max<std::size_t>(from, 1);
         i < std::min(to, x.size()); ++i)
        m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

bool identical(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

GraphDesc withInsert(GraphDesc g, NodeId id, float dB) {
    g.find(id)->processing.inserts.push_back({InsertDesc::Type::Gain, dB});
    return g;
}

}  // namespace

TEST(engine_renders_reference_graph) {
    Offline r;
    CHECK(r.engine.setGraph(reference()));
    r.run(100);
    float peak = 0;
    for (float v : r.out[0]) {
        CHECK(std::isfinite(v));
        peak = std::max(peak, std::fabs(v));
    }
    CHECK(peak > 0.01f);
    CHECK(peak <= 1.0f);
    CHECK(rt::violations() == 0);
}

TEST(engine_rejects_invalid_graphs) {
    Offline r;
    GraphDesc g = sineGraph();
    g.edges.push_back({3, 2, 0, 0});  // 2 -> 3 -> 2
    std::string err;
    CHECK(!r.engine.setGraph(g, &err));
    CHECK(err.find("cycle") != std::string::npos);
    g = sineGraph();
    g.edges.push_back({1, 99, 0, 0});
    CHECK(!r.engine.setGraph(g, &err));
}

TEST(null_test_insert_on_unrelated_strip) {
    const auto g = reference();
    const NodeId odd = bench::kStripBase + 3;
    Offline a, b;
    CHECK(a.engine.setGraph(g));
    CHECK(b.engine.setGraph(g));
    a.run(300);
    b.run(300, [&](int blk) {
        if (blk == 50) CHECK(b.engine.setGraph(withInsert(g, odd, -6)));
        if (blk == 150) CHECK(b.engine.setGraph(g));  // and back out
    });
    CHECK_MSG(identical(a.out[2], b.out[2]) && identical(a.out[3], b.out[3]),
              "aux output changed by an edit it does not depend on");
    CHECK_MSG(!identical(a.out[0], b.out[0]),
              "program output should reflect the edit");
    CHECK(b.engine.settled());
    CHECK(rt::violations() == 0);
}

TEST(null_test_route_change) {
    const auto g = reference();
    const NodeId odd = bench::kStripBase + 5;
    auto rerouted = g;
    std::erase_if(rerouted.edges, [&](const EdgeDesc& e) {
        return e.from == odd && e.to >= bench::kGroupBase;
    });
    rerouted.edges.push_back({odd, bench::kGroupBase + 1, -3, 0.5f});

    Offline a, b;
    CHECK(a.engine.setGraph(g));
    CHECK(b.engine.setGraph(g));
    a.run(200);
    b.run(200, [&](int blk) {
        if (blk == 40) CHECK(b.engine.setGraph(rerouted));
    });
    CHECK(identical(a.out[2], b.out[2]) && identical(a.out[3], b.out[3]));
    CHECK(!identical(a.out[0], b.out[0]));
    CHECK(rt::violations() == 0);
}

TEST(insert_change_crossfades) {
    Offline r(0, 1);
    CHECK(r.engine.setGraph(sineGraph()));
    r.run(60, [&](int blk) {
        if (blk == 20)
            CHECK(r.engine.setGraph(withInsert(sineGraph(), 2, 6)));
    });
    const auto& x = r.out[0];
    // Steady state after the edit: sine at twice the amplitude.
    const float after = maxDelta(x, 40 * kBlock);
    const float across = maxDelta(x, 19 * kBlock, 40 * kBlock);
    CHECK_MSG(across <= after * 1.05f,
              "step at the swap: " + std::to_string(across) + " vs " +
                  std::to_string(after));
    CHECK(maxDelta(x, 1, 19 * kBlock) < after * 0.55f);  // was half
}

TEST(edge_add_and_remove_ramp) {
    auto unrouted = sineGraph();
    unrouted.edges.pop_back();  // strip -> output removed
    Offline r(0, 1);
    CHECK(r.engine.setGraph(sineGraph()));
    r.run(80, [&](int blk) {
        if (blk == 20) CHECK(r.engine.setGraph(unrouted));
        if (blk == 50) CHECK(r.engine.setGraph(sineGraph()));
    });
    const auto& x = r.out[0];
    const float steady = maxDelta(x, 1, 19 * kBlock);
    CHECK(maxDelta(x) <= steady * 1.05f);
    // Fully silent once the removal has faded.
    for (int i = 30 * kBlock; i < 50 * kBlock; ++i) CHECK(x[i] == 0.0f);
    CHECK(rt::violations() == 0);
}

TEST(node_removal_fades_out) {
    auto removed = sineGraph();
    removed.nodes.erase(removed.nodes.begin() + 1);  // strip
    removed.edges.clear();
    Offline r(0, 1);
    CHECK(r.engine.setGraph(sineGraph()));
    r.run(60, [&](int blk) {
        if (blk == 20) CHECK(r.engine.setGraph(removed));
    });
    const auto& x = r.out[0];
    CHECK(maxDelta(x) <= maxDelta(x, 1, 19 * kBlock) * 1.05f);
    for (int i = 30 * kBlock; i < 60 * kBlock; ++i) CHECK(x[i] == 0.0f);
    CHECK(r.engine.settled());
}

TEST(mute_ramps) {
    Offline r(0, 1);
    CHECK(r.engine.setGraph(sineGraph()));
    r.run(40, [&](int blk) {
        if (blk == 20) r.engine.setLevel(2, 0, true);
    });
    const auto& x = r.out[0];
    CHECK(maxDelta(x) <= maxDelta(x, 1, 19 * kBlock) * 1.05f);
    for (int i = 30 * kBlock; i < 40 * kBlock; ++i) CHECK(x[i] == 0.0f);
}

TEST(edits_coalesce_while_fading) {
    Offline r(0, 1);
    CHECK(r.engine.setGraph(sineGraph()));
    r.run(40, [&](int blk) {
        // Three edits inside one transition window: last one wins.
        if (blk == 10) CHECK(r.engine.setGraph(withInsert(sineGraph(), 2, 3)));
        if (blk == 11) CHECK(r.engine.setGraph(withInsert(sineGraph(), 2, 9)));
        if (blk == 12) CHECK(r.engine.setGraph(withInsert(sineGraph(), 2, 6)));
    });
    CHECK(r.engine.settled());
    const auto& x = r.out[0];
    CHECK(maxDelta(x) <= maxDelta(x, 30 * kBlock) * 1.05f);
    CHECK(r.engine.graph().find(2)->processing.inserts[0].value == 6);
}

TEST(parallel_render_matches_serial) {
    const auto g = reference(64);
    const auto edited = withInsert(g, bench::kStripBase + 7, -4);
    auto script = [&](Offline& o) {
        return [&](int blk) {
            if (blk == 30) CHECK(o.engine.setGraph(edited));
            if (blk == 90) CHECK(o.engine.setGraph(g));
        };
    };
    Offline serial(0), parallel(3);
    CHECK(serial.engine.setGraph(g));
    CHECK(parallel.engine.setGraph(g));
    serial.run(150, script(serial));
    parallel.run(150, script(parallel));
    for (int c = 0; c < 4; ++c)
        CHECK_MSG(identical(serial.out[c], parallel.out[c]),
                  "channel " + std::to_string(c) + " differs");
    CHECK(rt::violations() == 0);
}

// --- regressions from the first concurrency / logic review ---------------

namespace {

// gen -> s1 -> s2 -> out (mono)
GraphDesc chainGraph(bool reversed) {
    GraphDesc g;
    NodeDesc gen;
    gen.id = 1;
    gen.kind = NodeKind::Generator;
    gen.generator = {7, 100, -12, -200, false};
    NodeDesc s1, s2;
    s1.id = 2;
    s2.id = 3;
    NodeDesc out;
    out.id = 4;
    out.kind = NodeKind::Output;
    g.nodes = {gen, s1, s2, out};
    if (!reversed)
        g.edges = {{1, 2, 0, 0}, {2, 3, 0, 0}, {3, 4, 0, 0}};
    else
        g.edges = {{1, 3, 0, 0}, {3, 2, 0, 0}, {2, 4, 0, 0}};
    return g;
}

// Two independent sine paths: node 2 -> output ch 0, node 12 -> ch 1.
GraphDesc twoPaths() {
    GraphDesc g;
    for (NodeId base : {NodeId(1), NodeId(11)}) {
        NodeDesc gen;
        gen.id = base;
        gen.kind = NodeKind::Generator;
        gen.generator = {base, 100.0f + float(base), -12, -200, false};
        NodeDesc strip;
        strip.id = base + 1;
        NodeDesc out;
        out.id = base + 2;
        out.kind = NodeKind::Output;
        out.ioChannel = base == 1 ? 0 : 1;
        g.nodes.insert(g.nodes.end(), {gen, strip, out});
        g.edges.push_back({base, base + 1, 0, 0});
        g.edges.push_back({base + 1, base + 2, 0, 0});
    }
    return g;
}

}  // namespace

TEST(negative_io_channel_rejected) {
    Offline r(0, 1);
    auto g = sineGraph();
    g.find(3)->ioChannel = -1;
    std::string err;
    CHECK(!r.engine.setGraph(g, &err));
}

TEST(reversing_an_edge_publishes) {
    Offline r(0, 1);
    CHECK(r.engine.setGraph(chainGraph(false)));
    r.run(80, [&](int blk) {
        if (blk == 10) CHECK(r.engine.setGraph(chainGraph(true)));
    });
    CHECK_MSG(r.engine.settled(), "edit never published");
    CHECK(r.engine.lastError().empty());
    const auto& x = r.out[0];
    CHECK(maxDelta(x) <= maxDelta(x, 1, 9 * kBlock) * 1.05f);
    float tail = 0;
    for (int i = 70 * kBlock; i < 80 * kBlock; ++i)
        tail = std::max(tail, std::fabs(x[i]));
    CHECK(tail > 0.2f);  // reversed chain is live
}

TEST(unrelated_edit_keeps_fader_ramp_identical) {
    Offline a(0, 2), b(0, 2);
    CHECK(a.engine.setGraph(twoPaths()));
    CHECK(b.engine.setGraph(twoPaths()));
    a.run(40, [&](int blk) {
        if (blk == 10) a.engine.setLevel(2, -6, false);
    });
    b.run(40, [&](int blk) {
        if (blk == 10) b.engine.setLevel(2, -6, false);
        // One block later the 5 ms fader ramp is still running. Edit the
        // current graph, as a UI would, so the fader move stays.
        if (blk == 11)
            CHECK(b.engine.setGraph(withInsert(b.engine.graph(), 12, -6)));
    });
    CHECK_MSG(identical(a.out[0], b.out[0]),
              "edit on path 2 changed path 1's fader ramp");
}

TEST(level_and_graph_ordering) {
    Offline r(0, 1);
    CHECK(r.engine.setGraph(sineGraph()));
    r.run(5);
    // setLevel after setGraph wins...
    auto g = sineGraph();
    g.find(2)->faderDb = -20;
    CHECK(r.engine.setGraph(g));
    r.engine.setLevel(2, 0, true);
    r.run(30);
    for (int i = 25 * kBlock; i < 35 * kBlock; ++i) CHECK(r.out[0][i] == 0.0f);
    // ...and setGraph after setLevel wins.
    r.engine.setLevel(2, 0, true);
    g.find(2)->faderDb = 0;
    g.find(2)->mute = false;
    CHECK(r.engine.setGraph(g));
    r.run(30);
    float tail = 0;
    for (int i = 55 * kBlock; i < 65 * kBlock; ++i)
        tail = std::max(tail, std::fabs(r.out[0][i]));
    CHECK(tail > 0.2f);
}

TEST(channel_count_change_fades) {
    // Mono strip into a stereo output; make the strip stereo mid-stream.
    auto g = sineGraph();
    g.find(3)->channels = 2;
    auto stereo = g;
    stereo.find(2)->channels = 2;
    Offline r(0, 2);
    CHECK(r.engine.setGraph(g));
    r.run(60, [&](int blk) {
        if (blk == 20) CHECK(r.engine.setGraph(stereo));
    });
    for (int c = 0; c < 2; ++c) {
        const auto& x = r.out[c];
        const float steady = std::max(maxDelta(x, 1, 19 * kBlock),
                                      maxDelta(x, 40 * kBlock));
        CHECK_MSG(maxDelta(x) <= steady * 1.05f,
                  "step on channel " + std::to_string(c));
    }
}

TEST(output_repatch_fades) {
    auto g = sineGraph();
    auto moved = g;
    moved.find(3)->ioChannel = 1;
    Offline r(0, 2);
    CHECK(r.engine.setGraph(g));
    r.run(60, [&](int blk) {
        if (blk == 20) CHECK(r.engine.setGraph(moved));
    });
    const float steady = maxDelta(r.out[0], 1, 19 * kBlock);
    CHECK(maxDelta(r.out[0]) <= steady * 1.05f);
    CHECK(maxDelta(r.out[1]) <= steady * 1.05f);
    for (int i = 40 * kBlock; i < 60 * kBlock; ++i) CHECK(r.out[0][i] == 0.0f);
}

TEST(kind_change_fades) {
    // Strip 2 becomes an Output on channel 1 while its old output stays.
    auto g = sineGraph();
    auto changed = g;
    changed.find(2)->kind = NodeKind::Output;
    changed.find(2)->ioChannel = 1;
    changed.edges.pop_back();  // 2 -> 3 gone
    Offline r(0, 2);
    CHECK(r.engine.setGraph(g));
    r.run(60, [&](int blk) {
        if (blk == 20) CHECK(r.engine.setGraph(changed));
    });
    const float steady = maxDelta(r.out[0], 1, 19 * kBlock);
    CHECK(maxDelta(r.out[0]) <= steady * 1.05f);
    CHECK(maxDelta(r.out[1]) <= steady * 1.05f);
}

TEST(degenerate_config_is_clamped) {
    EngineConfig c;
    c.maxBlock = 0;
    c.transitionMs = 0;
    Engine e(c);
    CHECK(e.config().maxBlock >= 1);
    CHECK(e.setGraph(sineGraph()));
    float buf[64];
    float* out[1] = {buf};
    e.process(nullptr, 0, out, 1, 64);  // must return
}

TEST(meters_report_post_fader_peak_and_rms) {
    Offline r(0, 1);
    CHECK(r.engine.setGraph(sineGraph()));  // -12 dBFS sine
    r.run(40);
    MeterReading m;
    CHECK(r.engine.takeMeter(2, m));
    CHECK(m.channels == 1);
    const float amp = dsp::dbToGain(-12);
    CHECK(std::fabs(m.peak[0] - amp) < 0.01f);
    CHECK(std::fabs(m.rms[0] - amp / std::sqrt(2.0f)) < 0.01f);
    CHECK(r.engine.takeMeter(2, m) && m.peak[0] == 0);  // reset on read
    r.engine.setLevel(2, -6, false);
    r.run(5);  // let the 5 ms ramp finish, and discard that window
    CHECK(r.engine.takeMeter(2, m));
    r.run(40);
    CHECK(r.engine.takeMeter(2, m));
    const float drop =
        dsp::gainToDb(amp / std::sqrt(2.0f)) - dsp::gainToDb(m.rms[0]);
    CHECK_MSG(std::fabs(drop - 6) < 0.3f, "fader drop " + std::to_string(drop));
    CHECK(!r.engine.takeMeter(999, m));
    CHECK(rt::violations() == 0);
}
