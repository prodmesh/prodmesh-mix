#include <QJsonDocument>

#include "bench_graph.h"
#include "daemon/shows.h"
#include "engine/engine.h"
#include "proto/protocol.h"
#include "test.h"

using namespace pm;

namespace {

// Serialising a graph and reading it back must give an engine the exact
// same render.
bool sameRender(const GraphDesc& a, const GraphDesc& b) {
    auto render = [](const GraphDesc& g) {
        EngineConfig c;
        c.maxBlock = 128;
        c.numOutputs = 4;
        Engine e(c);
        if (!e.setGraph(g)) return std::vector<float>{};
        std::vector<float> all, buf(4 * 128);
        float* out[4] = {&buf[0], &buf[128], &buf[256], &buf[384]};
        for (int blk = 0; blk < 50; ++blk) {
            e.pump();
            e.process(nullptr, 0, out, 4, 128);
            all.insert(all.end(), buf.begin(), buf.end());
        }
        return all;
    };
    const auto x = render(a), y = render(b);
    return !x.empty() && x == y;
}

}  // namespace

TEST(graph_json_round_trip_renders_identically) {
    for (const auto& g : {shows::livestream(),
                          bench::makeReferenceGraph(bench::RefParams{})}) {
        const auto bytes = proto::frame(proto::graphToJson(g));
        CHECK(bytes.endsWith('\n'));
        CHECK(bytes.count('\n') == 1);
        GraphDesc back;
        QString err;
        CHECK(proto::graphFromJson(QJsonDocument::fromJson(bytes).object(),
                                   back, &err));
        CHECK(back.nodes.size() == g.nodes.size());
        CHECK(back.edges.size() == g.edges.size());
        for (std::size_t i = 0; i < g.nodes.size(); ++i)
            CHECK(back.nodes[i].mixMinus == g.nodes[i].mixMinus);
        CHECK(sameRender(g, back));
    }
}

TEST(graph_json_rejects_garbage) {
    GraphDesc g;
    QString err;
    CHECK(!proto::graphFromJson(
        QJsonDocument::fromJson(R"({"nodes":[{"kind":"strip"}]})").object(),
        g, &err));
    CHECK(!proto::graphFromJson(
        QJsonDocument::fromJson(R"({"nodes":[{"id":1,"kind":"x"}]})").object(),
        g, &err));
}

TEST(livestream_show_is_valid) {
    EngineConfig c;
    Engine e(c);
    std::string err;
    CHECK_MSG(e.setGraph(shows::livestream(), &err), err);
}

// What a client gets back (with derived sends) can be sent again as is.
TEST(resolved_graph_round_trips) {
    EngineConfig c;
    Engine e(c);
    CHECK(e.setGraph(shows::livestream()));
    const auto resolved = e.graph();
    int derived = 0;
    for (const auto& x : resolved.edges) derived += x.derived;
    CHECK(derived == 6);  // 8 strips, 2 remotes excluded
    GraphDesc back;
    CHECK(proto::graphFromJson(proto::graphToJson(resolved), back, nullptr));
    CHECK(e.setGraph(back));
    CHECK(e.graph().edges.size() == resolved.edges.size());
}

// Every device input exists (and is recorded) whether a channel uses it or
// not; old sessions' source-fader trims move onto the input edges.
TEST(normalize_adds_device_inputs_and_moves_trim) {
    GraphDesc g = shows::inputs(2);
    g.find(1000)->faderDb = 6;  // a 0.0.1 session: trim on the source
    shows::normalize(g, 4);
    int inputs = 0;
    for (const auto& n : g.nodes) inputs += n.kind == NodeKind::Input;
    CHECK(inputs == 4);
    CHECK(g.find(1000)->faderDb == 0);
    for (const auto& e : g.edges)
        if (e.from == 1000) CHECK(e.gainDb == 6);
    const auto n = g.nodes.size();
    shows::normalize(g, 4);  // idempotent
    CHECK(g.nodes.size() == n);
    EngineConfig c;
    c.numInputs = 4;
    Engine e(c);
    CHECK(e.setGraph(g));
}
