#include "bench_graph.h"

#include <string>

namespace pm::bench {

RefLayout layoutFor(int paths) {
    // One bus per eight paths: program, aux, and the rest as groups.
    const int buses = paths / 8 < 3 ? 3 : paths / 8;
    return {paths - buses, buses - 2};
}

ProcessingDesc stripChain() {
    ProcessingDesc p;
    p.hpf = true;
    p.gate = true;
    p.eq = true;
    p.bands[1].gainDb = -3;
    p.bands[2].gainDb = 2;
    p.comp = true;
    return p;
}

ProcessingDesc busChain(bool program) {
    ProcessingDesc p;
    p.eq = true;
    p.comp = true;
    p.compParams.ratio = 2;
    p.compParams.thresholdDb = -12;
    p.limiter = program;
    return p;
}

GraphDesc makeReferenceGraph(const RefParams& rp) {
    const auto lay = layoutFor(rp.paths);
    GraphDesc g;

    auto bus = [&](NodeId id, std::string name, bool program) {
        NodeDesc n;
        n.id = id;
        n.kind = NodeKind::Bus;
        n.channels = 2;
        n.name = std::move(name);
        n.processing = busChain(program);
        g.nodes.push_back(n);
    };
    bus(kProgram, "Program", true);
    bus(kAux, "Aux", false);
    for (int k = 0; k < lay.groups; ++k)
        bus(kGroupBase + k, "Group " + std::to_string(k + 1), false);

    for (int i = 0; i < lay.strips; ++i) {
        NodeDesc src;
        src.id = kGenBase + i;
        src.channels = 1;
        if (rp.deviceInputs) {
            src.kind = NodeKind::Input;
            src.ioChannel = i;
        } else {
            src.kind = NodeKind::Generator;
            src.generator.seed = std::uint32_t(i + 1);
            src.generator.freqHz = 80.0f + 37.0f * float(i);
        }
        src.name = "Source " + std::to_string(i + 1);
        g.nodes.push_back(src);

        NodeDesc strip;
        strip.id = kStripBase + i;
        strip.kind = NodeKind::Strip;
        strip.channels = 1;
        strip.name = "Ch " + std::to_string(i + 1);
        strip.processing = stripChain();
        strip.faderDb = -6;
        if (i < rp.heavyInserts)
            strip.processing.inserts.push_back(
                InsertDesc::cpuLoad(rp.heavyOpsPerSample));
        g.nodes.push_back(strip);

        const float pan = lay.strips > 1
                              ? -0.8f + 1.6f * float(i) / float(lay.strips - 1)
                              : 0.0f;
        g.edges.push_back({src.id, strip.id, 0, 0});
        g.edges.push_back({strip.id, kProgram, -6, pan});
        if (lay.groups > 0)
            g.edges.push_back({strip.id, kGroupBase + NodeId(i % lay.groups),
                               -12, pan});
        if (i % 2 == 0) g.edges.push_back({strip.id, kAux, -10, pan});
    }
    for (int k = 0; k < lay.groups; ++k)
        g.edges.push_back({kGroupBase + NodeId(k), kProgram, -6, 0});

    auto output = [&](NodeId id, int ch, NodeId from) {
        NodeDesc n;
        n.id = id;
        n.kind = NodeKind::Output;
        n.channels = 2;
        n.ioChannel = ch;
        g.nodes.push_back(n);
        g.edges.push_back({from, id, 0, 0});
    };
    output(kOutProgram, 0, kProgram);
    output(kOutAux, 2, kAux);
    return g;
}

}  // namespace pm::bench
