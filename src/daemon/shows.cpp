#include "daemon/shows.h"

#include <string>

namespace pm::shows {
namespace {

constexpr NodeId kTalent = 3002, kRemotes = 3003;
constexpr NodeId kOutProgram = 4000, kOutMixMinus = 4001;

ProcessingDesc voiceChain() {
    ProcessingDesc p;
    p.hpf = true;
    p.hpfHz = 90;
    p.gate = true;
    p.eq = true;
    p.bands[0].gainDb = -2;
    p.bands[2].gainDb = 2;
    p.comp = true;
    p.compParams.thresholdDb = -20;
    p.compParams.ratio = 3;
    return p;
}

ProcessingDesc busChain(bool program) {
    ProcessingDesc p;
    p.eq = true;
    p.comp = true;
    p.compParams.ratio = 2;
    p.compParams.thresholdDb = -14;
    p.limiter = program;
    return p;
}

}  // namespace

GraphDesc livestream() {
    struct Source {
        const char* name;
        NodeId group;  // 0 = straight to program
        bool remote;   // excluded from the mix-minus feed
        float freq;
    };
    const Source sources[] = {
        {"Host", kTalent, false, 140},     {"Co-host", kTalent, false, 190},
        {"Guest 1", kTalent, false, 230},  {"Guest 2", kTalent, false, 170},
        {"Remote 1", kRemotes, true, 210}, {"Remote 2", kRemotes, true, 160},
        {"Playback", 0, false, 330},       {"Video", 0, false, 260},
    };

    GraphDesc g;
    auto bus = [&](NodeId id, const char* name, bool program) {
        NodeDesc n;
        n.id = id;
        n.kind = NodeKind::Bus;
        n.channels = 2;
        n.name = name;
        n.processing = busChain(program);
        g.nodes.push_back(n);
    };
    bus(kProgram, "Program", true);
    bus(kMixMinus, "Mix-minus", false);
    bus(kTalent, "Talent", false);
    bus(kRemotes, "Remotes", false);

    const int count = int(std::size(sources));
    for (int i = 0; i < count; ++i) {
        const auto& s = sources[i];
        NodeDesc src;
        src.id = 1000 + NodeId(i);
        src.kind = NodeKind::Generator;
        src.name = std::string(s.name) + " source";
        src.generator.seed = std::uint32_t(i + 1);
        src.generator.freqHz = s.freq;
        src.generator.sineDb = -14;
        src.generator.noiseDb = -32;
        g.nodes.push_back(src);

        NodeDesc strip;
        strip.id = 2000 + NodeId(i);
        strip.kind = NodeKind::Strip;
        strip.name = s.name;
        strip.processing = voiceChain();
        g.nodes.push_back(strip);

        const float pan = -0.5f + float(i) / float(count - 1);
        g.edges.push_back({src.id, strip.id, 0, 0});
        g.edges.push_back({strip.id, s.group ? s.group : kProgram, 0, pan});
        // Remote guests hear everyone but themselves.
        if (!s.remote) g.edges.push_back({strip.id, kMixMinus, -3, 0});
    }
    g.edges.push_back({kTalent, kProgram, 0, 0});
    g.edges.push_back({kRemotes, kProgram, 0, 0});

    auto output = [&](NodeId id, const char* name, int ch, NodeId from) {
        NodeDesc n;
        n.id = id;
        n.kind = NodeKind::Output;
        n.channels = 2;
        n.ioChannel = ch;
        n.name = name;
        g.nodes.push_back(n);
        g.edges.push_back({from, id, 0, 0});
    };
    output(kOutProgram, "Program out", 0, kProgram);
    output(kOutMixMinus, "Mix-minus out", 2, kMixMinus);
    return g;
}

GraphDesc inputs(int count) {
    GraphDesc g;
    NodeDesc program;
    program.id = kProgram;
    program.kind = NodeKind::Bus;
    program.channels = 2;
    program.name = "Program";
    program.processing = busChain(true);
    g.nodes.push_back(program);
    for (int i = 0; i < count; ++i) {
        NodeDesc in;
        in.id = 1000 + NodeId(i);
        in.kind = NodeKind::Input;
        in.ioChannel = i;
        in.name = "In " + std::to_string(i + 1) + " source";
        g.nodes.push_back(in);
        NodeDesc strip;
        strip.id = 2000 + NodeId(i);
        strip.name = "In " + std::to_string(i + 1);
        strip.processing.hpf = true;
        g.nodes.push_back(strip);
        const float pan = count > 1 ? -0.6f + 1.2f * float(i) / float(count - 1) : 0.0f;
        g.edges.push_back({in.id, strip.id, 0, 0});
        g.edges.push_back({strip.id, kProgram, 0, pan});
    }
    NodeDesc out;
    out.id = 4000;
    out.kind = NodeKind::Output;
    out.channels = 2;
    out.name = "Program out";
    g.nodes.push_back(out);
    g.edges.push_back({kProgram, out.id, 0, 0});
    return g;
}

}  // namespace pm::shows
