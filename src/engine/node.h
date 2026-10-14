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
// and pans fade instead of stepping.
class EdgeInstance {
public:
    EdgeInstance(NodeId from, NodeId to, int srcCh, int dstCh);

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
};

}  // namespace pm
