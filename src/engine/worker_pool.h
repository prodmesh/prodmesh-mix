// Parallel render helpers. Steps within one plan level are independent, so
// the device thread and N workers claim them with an atomic counter and
// meet at a spin barrier before the next level. Idle workers sleep on an
// atomic wait (a futex-style ulock on macOS), woken once per callback.
//
// Counters carry the callback generation in their high bits: a worker that
// wakes late, after its callback already finished, fails every claim and
// goes back to sleep instead of touching the next callback's data. They
// live here, not in the plan, so a late worker never reads a retired plan.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/rt_guard.h"
#include "core/rt_thread.h"

namespace pm {

class Engine;
struct RenderPlan;

class WorkerPool {
public:
    static constexpr int kMaxLevels = 64;

    WorkerPool(Engine& engine, int threads, double sampleRate, int maxBlock,
               rt::Workgroup wg);
    ~WorkerPool();

    int threads() const { return int(threads_.size()); }

    // Audio thread: renders every level of `plan` with the workers' help.
    void run(RenderPlan& plan, const float* const* in, int numIn, int frames);

private:
    void workerMain(int index);
    // Claims and renders steps of `level` for generation `gen`; returns
    // false if the generation moved on.
    bool work(std::uint64_t gen, int level) PM_NONBLOCKING;
    bool waitLevel(std::uint64_t gen, int level, int size);

    Engine& engine_;
    double sampleRate_;
    int maxBlock_;
    rt::Workgroup wg_;

    std::atomic<bool> quit_{false};
    std::vector<std::thread> threads_;

    std::atomic<std::uint64_t> gen_{0};
    std::atomic<std::uint64_t> next_[kMaxLevels];
    std::atomic<std::uint64_t> done_[kMaxLevels];

    // Job parameters, valid for the current generation only.
    std::atomic<RenderPlan*> plan_{nullptr};
    std::atomic<const float* const*> in_{nullptr};
    std::atomic<int> numIn_{0}, frames_{0}, levels_{0};
    std::atomic<int> start_[kMaxLevels], size_[kMaxLevels];
};

}  // namespace pm
