#include <algorithm>

#include "engine/graph.h"

namespace pm {
namespace {

const EdgeDesc* findEdge(const GraphDesc& g, NodeId from, NodeId to) {
    for (const auto& e : g.edges)
        if (e.from == from && e.to == to && !e.derived) return &e;
    return nullptr;
}

bool safe(const GraphDesc& g, NodeId id) {
    const auto* n = g.find(id);
    return n && n->recallSafe;
}

// Exclusions of a mix-minus as the scene has them, except for safe
// channels, which keep their current membership.
std::vector<NodeId> exclusions(const GraphDesc& now, const NodeDesc& bus,
                               const NodeDesc& stored) {
    std::vector<NodeId> out;
    for (NodeId x : stored.mixMinus.exclude)
        if (!safe(now, x)) out.push_back(x);
    for (NodeId x : bus.mixMinus.exclude)
        if (safe(now, x)) out.push_back(x);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

GraphDesc applyScene(const GraphDesc& now, const GraphDesc& scene) {
    GraphDesc g = now;
    for (auto& n : g.nodes) {
        const auto* s = scene.find(n.id);
        if (!s || s->kind != n.kind || n.recallSafe) continue;
        n.faderDb = s->faderDb;
        n.mute = s->mute;
        if (n.mixMinus.follows && s->mixMinus.follows)
            n.mixMinus.exclude = exclusions(now, n, *s);
    }
    for (auto& e : g.edges) {
        if (e.derived || safe(now, e.from) || safe(now, e.to)) continue;
        if (const auto* s = findEdge(scene, e.from, e.to)) {
            e.gainDb = s->gainDb;
            e.pan = s->pan;
            e.mute = s->mute;
            e.preFader = s->preFader;
        }
    }
    resolveDerived(g);
    return g;
}

bool sceneMatches(const GraphDesc& now, const GraphDesc& scene) {
    const GraphDesc r = applyScene(now, scene);
    for (std::size_t i = 0; i < now.nodes.size(); ++i) {
        const auto& a = now.nodes[i];
        const auto& b = r.nodes[i];
        if (a.faderDb != b.faderDb || a.mute != b.mute ||
            !(a.mixMinus == b.mixMinus))
            return false;
    }
    for (const auto& e : now.edges) {
        if (e.derived) continue;
        const auto* x = findEdge(r, e.from, e.to);
        if (!x || x->gainDb != e.gainDb || x->pan != e.pan ||
            x->mute != e.mute || x->preFader != e.preFader)
            return false;
    }
    return true;
}

}  // namespace pm
