#include "engine/worker_pool.h"

#include "core/rt_guard.h"
#include "engine/engine.h"
#include "engine/plan.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#define PM_CPU_RELAX() __asm__ __volatile__("yield")
#elif defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define PM_CPU_RELAX() _mm_pause()
#else
#define PM_CPU_RELAX() ((void)0)
#endif

namespace pm {
namespace {

constexpr std::uint64_t kIndexMask = 0xffffffffull;

std::uint64_t tag(std::uint64_t gen, std::uint64_t v) {
    return (gen << 32) | v;
}

}  // namespace

WorkerPool::WorkerPool(Engine& engine, int threads, double sampleRate,
                       int maxBlock, rt::Workgroup wg)
    : engine_(engine), sampleRate_(sampleRate), maxBlock_(maxBlock), wg_(wg) {
    for (int l = 0; l < kMaxLevels; ++l) {
        next_[l].store(0);
        done_[l].store(0);
        start_[l].store(0);
        size_[l].store(0);
    }
    for (int i = 0; i < threads; ++i)
        threads_.emplace_back([this, i] { workerMain(i); });
}

WorkerPool::~WorkerPool() {
    quit_.store(true);
    gen_.fetch_add(1);
    gen_.notify_all();
    for (auto& t : threads_) t.join();
}

void WorkerPool::workerMain(int) {
    const auto periodNs =
        std::uint64_t(1e9 * maxBlock_ / sampleRate_);
    rt::promoteCurrentThread(periodNs, periodNs / 2);
    rt::WorkgroupMembership membership(wg_);
    rt::ScopedRealtime realtime;

    std::uint64_t seen = gen_.load();
    while (true) {
        gen_.wait(seen);
        if (quit_.load()) return;
        const std::uint64_t g = gen_.load();
        seen = g;
        const int levels = levels_.load();
        for (int l = 0; l < levels; ++l) {
            if (!work(g, l)) break;
            if (!waitLevel(g, l, size_[l].load())) break;
        }
    }
}

bool WorkerPool::work(std::uint64_t gen, int level) PM_NONBLOCKING {
    auto& next = next_[level];
    const int start = start_[level].load();
    const int size = size_[level].load();
    RenderPlan* plan = plan_.load();
    const float* const* in = in_.load();
    const int numIn = numIn_.load();
    const int frames = frames_.load();

    std::uint64_t v = next.load();
    while (true) {
        if ((v >> 32) != gen) return false;
        const auto idx = int(v & kIndexMask);
        if (idx >= size) return true;
        // A successful claim proves the generation (and so the parameters
        // read above) is current: they are written after the counters are
        // re-tagged and before gen_ is bumped.
        if (!next.compare_exchange_weak(v, v + 1)) continue;
        engine_.renderStep(*plan, start + idx, in, numIn, frames);
        done_[level].fetch_add(1);
        v = next.load();
    }
}

bool WorkerPool::waitLevel(std::uint64_t gen, int level, int size) {
    const auto want = tag(gen, std::uint64_t(size));
    while (done_[level].load() != want) {
        if (gen_.load() != gen) return false;
        PM_CPU_RELAX();
    }
    return true;
}

void WorkerPool::run(RenderPlan& plan, const float* const* in, int numIn,
                     int frames) {
    const std::uint64_t g = (gen_.load() + 1) & kIndexMask;
    const int levels = plan.levels() < kMaxLevels ? plan.levels() : kMaxLevels;

    for (int l = 0; l < levels; ++l) {
        next_[l].store(tag(g, 0));
        done_[l].store(tag(g, 0));
    }
    for (int l = 0; l < levels; ++l) {
        start_[l].store(plan.levelStart[l]);
        size_[l].store(plan.levelStart[l + 1] - plan.levelStart[l]);
    }
    plan_.store(&plan);
    in_.store(in);
    numIn_.store(numIn);
    frames_.store(frames);
    levels_.store(levels);
    gen_.store(g);
    gen_.notify_all();

    for (int l = 0; l < levels; ++l) {
        work(g, l);
        waitLevel(g, l, size_[l].load());
    }
    // Plans deeper than kMaxLevels finish serially.
    for (int s = levels < plan.levels() ? plan.levelStart[levels]
                                        : int(plan.steps.size());
         s < int(plan.steps.size()); ++s)
        engine_.renderStep(plan, s, in, numIn, frames);
}

}  // namespace pm
