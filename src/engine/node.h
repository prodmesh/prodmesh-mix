// Audio-thread objects. A NodeInstance owns a node's DSP state and lives
// across render plans for as long as its structure is unchanged, so its
// filters and envelopes never reset under a live edit. Instances and edges
// are created and destroyed on the control thread only.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/ramp.h"
#include "dsp/biquad.h"
#include "dsp/dynamics.h"
#include "dsp/generator.h"
#include "engine/graph.h"

namespace pm {

inline constexpr int kMaxChannels = 2;

// Built-in insert, and the shape third-party plugin adapters will take.
class Processor {
public:
    virtual ~Processor() = default;
    virtual void process(float* const* ch, int nch, int n) = 0;
    virtual int latencySamples() const { return 0; }
    // Audio thread, once, before the instance leaves the graph (CLAP's
    // stop_processing). Processing starts lazily on the first process().
    virtual void stopProcessing() {}

    // Third-party plugins: display name, and how often the plugin's own
    // code allocated on the audio thread (it is not real-time safe).
    virtual std::string name() const { return {}; }
    virtual std::uint64_t rtAllocations() const { return rtAllocs_.load(); }

    // Control thread, every pump(): housekeeping such as restarting a
    // crashed sandbox host. Returns a status line when something happened.
    virtual std::string maintain() { return {}; }
    // Sandboxed plugins: blocks the host did not deliver in time (each one
    // passed through dry at the same latency).
    virtual std::uint64_t missedBlocks() const { return 0; }
    virtual bool sandboxed() const { return false; }

    // Plugin editor, in whichever process hosts the plugin (its main
    // thread): embed into a native parent view (NSView* on macOS).
    virtual bool hasEditor() const { return false; }
    virtual bool createEditor(void* /*nativeParent*/, int* /*w*/, int* /*h*/) {
        return false;
    }
    virtual void destroyEditor() {}

    // Engine side, control thread: open the editor of plugin `index` of
    // this insert (a chain when isolated) in the process that hosts it.
    virtual bool requestEditor(int /*index*/, std::string* error) {
        if (error)
            *error = "this plugin runs inside the engine, which has no "
                     "screen: run it isolated to open its window";
        return false;
    }

protected:
    std::atomic<std::uint64_t> rtAllocs_{0};
};

class NodeInstance {
public:
    NodeInstance(const NodeDesc& desc, double sampleRate, int maxBlock);

    NodeId id() const { return id_; }
    NodeKind kind() const { return kind_; }
    int channels() const { return channels_; }
    const ProcessingDesc& processing() const { return processing_; }

    // Runs the processing chain in place. Fader and mute are applied by
    // the node's LevelState, after any crossfade.
    void process(float* const* ch, int n);

    // Audio thread: the instance is about to leave the graph.
    void stopProcessing();

    struct PluginRt {
        std::string plugin;
        std::uint64_t allocations = 0;
        std::uint64_t missed = 0;
    };
    // Plugins in this chain that allocated on the audio thread, or (when
    // sandboxed) missed blocks.
    std::vector<PluginRt> pluginRtReport() const;

    // Control thread: open the editor of the plugin at `insertIndex` (an
    // index into ProcessingDesc::inserts).
    bool requestEditor(int insertIndex, std::string* error);

    // Control thread: runs every insert's maintain(); returns event lines.
    std::vector<std::string> maintain();

    // Inserts that failed to load and are passed through, as messages.
    const std::vector<std::string>& warnings() const { return warnings_; }
    int latencySamples() const;

private:
    NodeId id_;
    NodeKind kind_;
    int channels_;
    ProcessingDesc processing_;

    dsp::Generator generator_;
    dsp::Biquad hpf_[kMaxChannels];
    dsp::Biquad eq_[4][kMaxChannels];
    dsp::Gate gate_[kMaxChannels];
    dsp::Compressor comp_;
    dsp::Limiter limiter_;
    std::vector<std::unique_ptr<Processor>> inserts_;
    // ProcessingDesc::inserts[i] -> (inserts_ index, position in its
    // chain), or -1 when it failed to load.
    std::vector<std::pair<int, int>> insertMap_;
    std::vector<std::string> warnings_;
};

// Fader + mute for one node id. It outlives instance swaps (a crossfade
// between chains shares one level), and every change carries a sequence
// number so a fader move and a plan carrying an older level can arrive in
// either order and the newest still wins.
class LevelState {
public:
    LevelState(float faderDb, bool mute);

    // Audio thread. Ignored if `seq` is not newer than the last applied.
    void set(float faderDb, bool mute, int rampSamples, std::uint64_t seq);
    void apply(float* const* ch, int nch, int n);

private:
    LinearRamp gain_;
    std::uint64_t seq_ = 0;
};

// Post-fader metering for one node, written by the audio thread once per
// block and drained by the control thread at display rate. Peaks are
// max-held until read, so a single-sample clip is never missed.
class MeterState {
public:
    explicit MeterState(int channels) : channels_(channels) {}

    struct Reading {
        int channels = 0;
        float peak[kMaxChannels]{};
        float rms[kMaxChannels]{};  // over the samples since the last read
    };

    // Audio thread.
    void accumulate(const float* const* ch, int n);
    // Control thread: returns and resets the accumulated values.
    Reading take();

private:
    int channels_;
    std::atomic<float> peak_[kMaxChannels]{};
    std::atomic<double> energy_[kMaxChannels]{};
    std::atomic<std::uint32_t> samples_{0};
};

// A live connection. Holds a ramped dst×src gain matrix so routing changes
// and pans fade instead of stepping, and the delay that aligns its source
// with the latest-arriving input of its destination (plugin delay
// compensation). The delay is fixed per instance: when compensation
// changes, a new edge fades in as the old one fades out.
class EdgeInstance {
public:
    EdgeInstance(NodeId from, NodeId to, int srcCh, int dstCh,
                 int delaySamples = 0, int maxBlock = 0);

    int delay() const { return delay_; }

    NodeId from() const { return from_; }
    NodeId to() const { return to_; }
    int srcChannels() const { return srcCh_; }
    int dstChannels() const { return dstCh_; }

    // Targets for a desc; a removed edge targets silence.
    static void targetsFor(const EdgeDesc* e, int srcCh, int dstCh,
                           float out[kMaxChannels][kMaxChannels]);

    void setTargets(const float t[kMaxChannels][kMaxChannels], int samples);
    bool silent() const;

    // dst[c] += sum_s m[c][s] * src[s]
    void mixInto(float* const* dst, const float* const* src, int n);

private:
    NodeId from_, to_;
    int srcCh_, dstCh_;
    LinearRamp m_[kMaxChannels][kMaxChannels];
    int delay_ = 0;
    std::size_t pos_ = 0;
    std::vector<float> line_[kMaxChannels];     // delay_ samples each
    std::vector<float> delayed_[kMaxChannels];  // maxBlock scratch
};

}  // namespace pm
