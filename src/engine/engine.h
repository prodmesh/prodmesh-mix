// The mix engine. The control thread describes the graph it wants; the
// engine compiles that into an immutable render plan off the audio thread
// and publishes it through a wait-free queue. The audio thread only ever
// executes plans:
//
//   - nodes whose processing chain is unchanged keep their instance, so
//     their state (and output) is untouched by an edit elsewhere;
//   - a node whose chain changed runs old and new instances side by side
//     and crossfades over the transition window;
//   - added / removed / re-levelled edges ramp their gains over the same
//     window, and removed nodes keep rendering until their edges are silent.
//
// One edit is in flight at a time: further setGraph() calls coalesce and
// publish once the previous transition has finished (~transitionMs). A node
// whose kind, channel count or device patch changes is replaced: the old one
// fades out as the new one fades in. An edit whose fading edges would form a
// cycle with its new ones (reversing a connection) publishes in two steps,
// removals first.
//
// Destroy the engine only after the driver has stopped calling process().
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "core/rt_guard.h"
#include "core/rt_thread.h"
#include "core/spsc_queue.h"
#include "engine/graph.h"

namespace pm {

class NodeInstance;
class EdgeInstance;
class LevelState;
class MeterState;
class WorkerPool;
struct RenderPlan;

inline constexpr int kMaxDeviceChannels = 256;

struct MeterReading {
    int channels = 0;
    float peak[2]{};  // linear, max since last read
    float rms[2]{};   // linear, over the samples since last read
};

struct EngineConfig {
    double sampleRate = 48000;
    int maxBlock = 512;
    int numInputs = 0;
    int numOutputs = 2;
    double transitionMs = 10;
    double levelRampMs = 5;  // fader / mute moves
    int workerThreads = 0;   // extra render threads beside the device's
    rt::Workgroup workgroup = nullptr;
};

class Engine {
public:
    explicit Engine(const EngineConfig& cfg);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    const EngineConfig& config() const { return cfg_; }

    // --- control thread -------------------------------------------------

    // Sets the desired graph. Returns false (and changes nothing) if the
    // graph is invalid, with the reason in `error`.
    bool setGraph(const GraphDesc& g, std::string* error = nullptr);

    // Fader / mute move: ramps on the live instance without a recompile.
    void setLevel(NodeId id, float faderDb, bool mute);

    // Publishes coalesced edits and frees retired plans. Call every few ms.
    void pump();

    // True when the audio thread runs the desired graph and no transition
    // or cleanup is outstanding.
    bool settled() const;

    const GraphDesc& graph() const { return desired_; }

    // Post-fader meters since the last read; false for an unknown node.
    // Reads reset the accumulators, so one reader (the daemon) owns them.
    bool takeMeter(NodeId id, MeterReading& out);

    // Why the last publish attempt failed; empty when it did not.
    const std::string& lastError() const { return lastError_; }

    // --- audio thread ---------------------------------------------------

    void process(const float* const* in, int numIn, float* const* out,
                 int numOut, int frames) PM_NONBLOCKING;

private:
    struct LevelCmd {
        NodeId id = 0;
        float faderDb = 0;
        bool mute = false;
        std::uint64_t seq = 0;
    };

    // What a plan leaves live for the next compile to reuse.
    struct Live {
        std::unordered_map<NodeId, std::shared_ptr<NodeInstance>> nodes;
        std::map<std::pair<NodeId, NodeId>, std::shared_ptr<EdgeInstance>>
            edges;
        std::unordered_map<NodeId, std::shared_ptr<LevelState>> levels;
        std::unordered_map<NodeId, std::shared_ptr<MeterState>> meters;
    };

    enum class Fail { None, Invalid, FadingCycle };

    std::unique_ptr<RenderPlan> compile(const GraphDesc& next,
                                        const GraphDesc* prev, Live* live,
                                        std::string* error, Fail* why);
    bool canPublish() const;
    void publish(std::unique_ptr<RenderPlan> plan, Live live,
                 const GraphDesc& desc);
    void activate(RenderPlan* plan);
    void renderStep(RenderPlan& p, int index, const float* const* in,
                    int numIn, int frames);

    EngineConfig cfg_;
    int transitionSamples_;
    int levelRampSamples_;

    // Control-thread state.
    GraphDesc desired_;
    GraphDesc published_;
    bool dirty_ = false;
    std::uint64_t nextSerial_ = 1;
    std::uint64_t publishedSerial_ = 0;
    bool publishedNeedsCleanup_ = false;
    bool resyncLevels_ = false;
    std::uint64_t levelSeq_ = 0;
    std::string lastError_;
    Live live_;

    // Cross-thread.
    SpscQueue<RenderPlan*> pending_{8};
    SpscQueue<RenderPlan*> retired_{64};
    SpscQueue<LevelCmd> levels_{1024};
    std::atomic<std::uint64_t> activeSerial_{0};
    std::atomic<std::uint64_t> transitionDoneSerial_{0};

    // Audio-thread state.
    RenderPlan* active_ = nullptr;
    std::unique_ptr<WorkerPool> pool_;

    friend class WorkerPool;
};

}  // namespace pm
