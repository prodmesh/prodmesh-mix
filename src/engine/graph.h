// Control-side description of a mix graph: plain values the UI/session
// edits freely. The engine diffs successive descriptions and compiles them
// into render plans; nothing here is touched by the audio thread.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/dynamics.h"
#include "dsp/generator.h"

namespace pm {

using NodeId = std::uint32_t;

enum class NodeKind : std::uint8_t {
    Input,      // device input channel(s)
    Generator,  // deterministic test source (benches, null tests)
    Strip,      // channel strip
    Bus,        // aux / group / program
    Output,     // device output channel(s)
};

struct EqBand {
    enum class Type : std::uint8_t { Peak, LowShelf, HighShelf };
    Type type = Type::Peak;
    float freqHz = 1000, q = 1, gainDb = 0;
    bool operator==(const EqBand&) const = default;
};

// An insert: a built-in test processor or a third-party plugin.
// Gain and CpuLoad exist for tests and benches (CpuLoad models a heavy
// plugin); Plugin names a CLAP/VST3 plugin by bundle path and id.
struct InsertDesc {
    enum class Type : std::uint8_t { Gain, CpuLoad, Plugin };
    Type type = Type::Gain;
    float value = 0;  // Gain: dB. CpuLoad: extra multiply-adds per sample.
    std::string format, path, pluginId;  // Plugin only
    // Plugin only: run in its own process (crash-isolated, +1 block).
    bool sandboxed = false;
    // Plugin only: the plugin's saved state, base64. Applied when the
    // instance is created; not part of the structure (a newer state never
    // rebuilds a running plugin).
    std::string state;

    bool operator==(const InsertDesc& o) const {
        return type == o.type && value == o.value && format == o.format &&
               path == o.path && pluginId == o.pluginId &&
               sandboxed == o.sandboxed;
    }

    static InsertDesc gain(float dB) { return {Type::Gain, dB, {}, {}, {}, false, {}}; }
    static InsertDesc cpuLoad(float ops) {
        return {Type::CpuLoad, ops, {}, {}, {}, false, {}};
    }
    static InsertDesc plugin(std::string format, std::string path,
                             std::string id, bool sandboxed = false) {
        return {Type::Plugin, 0, std::move(format), std::move(path),
                std::move(id), sandboxed, {}};
    }
};

// The processing chain. Any change here is structural: the engine builds a
// new node instance and crossfades to it. Fader and mute are not part of
// it; they ramp on the live instance.
struct ProcessingDesc {
    bool hpf = false;
    float hpfHz = 80;
    bool gate = false;
    dsp::GateParams gateParams;
    bool eq = false;
    std::array<EqBand, 4> bands{
        EqBand{EqBand::Type::LowShelf, 120, 0.7f, 0},
        EqBand{EqBand::Type::Peak, 500, 1.0f, 0},
        EqBand{EqBand::Type::Peak, 3000, 1.0f, 0},
        EqBand{EqBand::Type::HighShelf, 8000, 0.7f, 0}};
    bool comp = false;
    dsp::CompressorParams compParams;
    bool limiter = false;
    float limiterCeilingDb = -1;
    std::vector<InsertDesc> inserts;
};

bool sameStructure(const ProcessingDesc& a, const ProcessingDesc& b);

struct NodeDesc {
    NodeId id = 0;
    NodeKind kind = NodeKind::Strip;
    int channels = 1;  // 1 or 2
    std::string name;
    int ioChannel = 0;  // Input/Output: first device channel
    dsp::GeneratorParams generator;
    ProcessingDesc processing;
    float faderDb = 0;
    bool mute = false;
};

// A connection with its own level (a send, or a strip's bus assignment).
// Mono into stereo is panned with a -3 dB constant-power law.
struct EdgeDesc {
    NodeId from = 0, to = 0;
    float gainDb = 0;
    float pan = 0;  // -1 left .. +1 right
};

struct GraphDesc {
    std::vector<NodeDesc> nodes;
    std::vector<EdgeDesc> edges;

    const NodeDesc* find(NodeId id) const;
    NodeDesc* find(NodeId id);
};

}  // namespace pm
