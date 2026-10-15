// A compiled, immutable render plan. Built and destroyed on the control
// thread; between publish and retirement only the audio thread (and its
// workers) touch it, and the only fields that change are xfadePos and the
// DSP state inside the instances it points to.
#pragma once

#include <array>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "engine/graph.h"
#include "engine/node.h"
#include "record/playback.h"
#include "record/record_tap.h"

namespace pm {

struct Contribution {
    EdgeInstance* edge;
    int srcStep;
};

struct PlanStep {
    NodeId id = 0;
    NodeKind kind = NodeKind::Strip;
    NodeInstance* node = nullptr;
    NodeInstance* fadeOut = nullptr;  // old instance during a crossfade
    LevelState* level = nullptr;
    MeterState* meter = nullptr;
    // Recorder, loudness meter, ...: every tap on this node's output.
    std::array<RecordTap*, 4> taps{};
    PlaybackFeed* feed = nullptr;  // playback mode: this source's recording
    int nch = 1;
    int buf = 0;      // first channel in RenderPlan::chan
    int fadeBuf = 0;  // first scratch channel for fadeOut
    int preBuf = -1;  // pre-fader copy, when a pre-fader edge reads it
    int firstContrib = 0, numContrib = 0;
    int ioChannel = 0;
    bool dying = false;  // removed / replaced node, rendered while fading
    float faderDb = 0;
    bool mute = false;
};

struct EdgeTarget {
    EdgeInstance* edge;
    float m[kMaxChannels][kMaxChannels];
    int samples = 0;  // ramp length; 0 = the plan's crossfade
};

struct RenderPlan {
    std::uint64_t serial = 0;
    std::vector<PlanStep> steps;   // topological order, grouped by level
    std::vector<int> levelStart;   // steps of level l: [levelStart[l], levelStart[l+1])
    std::vector<Contribution> contribs;
    std::vector<EdgeTarget> edgeTargets;
    std::vector<std::pair<NodeId, int>> index;  // sorted live id -> step
    std::uint64_t levelSeq = 0;  // sequence of the levels baked in steps

    // Ownership; the audio thread only uses the raw pointers above.
    std::vector<std::shared_ptr<NodeInstance>> nodes;
    std::vector<std::shared_ptr<EdgeInstance>> edges;
    std::vector<std::shared_ptr<LevelState>> levelStates;
    std::vector<std::shared_ptr<MeterState>> meters;
    std::vector<std::shared_ptr<RecordTap>> taps;
    // Playback mode: sources play this set instead of live inputs / tones
    // (and are silent when they have no recording).
    PlaybackSet* playback = nullptr;
    std::shared_ptr<PlaybackSet> playbackHold;
    // Instances of the previous plan this one drops (owned by that plan).
    std::vector<NodeInstance*> stopping;

    std::unique_ptr<float[]> storage;
    std::vector<float*> chan;

    int xfadeLen = 0;
    int levelRamp = 0;  // fader ramp, samples; 0 = the engine's default
    int xfadePos = 0;
    bool needsCleanup = false;  // has dying steps or fadeOuts to drop

    int levels() const { return int(levelStart.size()) - 1; }
    bool fading() const { return xfadePos < xfadeLen; }

    const PlanStep* stepFor(NodeId id) const;
};

}  // namespace pm
