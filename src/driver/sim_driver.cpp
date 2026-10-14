#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <AudioToolbox/AudioWorkInterval.h>
#endif

#include "core/rt_guard.h"
#include "driver/driver.h"

namespace pm {
namespace {

class SimDriver final : public Driver {
public:
    SimDriver(const DriverConfig& cfg, bool freeRun)
        : cfg_(cfg), freeRun_(freeRun) {
#if defined(__APPLE__)
        // Our own clock gets the same treatment a CoreAudio device's I/O
        // thread does: an audio work interval with real deadlines, which
        // keeps it (and joined render workers) on performance cores.
        if (!freeRun_)
            interval_ = AudioWorkIntervalCreate(
                "ProdMesh Mix sim clock", OS_CLOCK_MACH_ABSOLUTE_TIME,
                nullptr);
#endif
    }
    ~SimDriver() override {
        stop();
#if defined(__APPLE__)
        if (interval_) os_release(interval_);
#endif
    }

    const DriverConfig& config() const override { return cfg_; }
#if defined(__APPLE__)
    rt::Workgroup workgroup() const override { return interval_; }
#endif

    bool start(RenderFn fn, void* ctx, std::string*) override {
        stop();
        running_.store(true);
        thread_ = std::thread([this, fn, ctx] { run(fn, ctx); });
        return true;
    }

    void stop() override {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

private:
    void run(RenderFn fn, void* ctx) {
        const int n = cfg_.blockSize;
        std::vector<float> inData(std::size_t(n) * cfg_.numInputs);
        std::vector<float> outData(std::size_t(n) * cfg_.numOutputs);
        std::vector<const float*> in(cfg_.numInputs);
        std::vector<float*> out(cfg_.numOutputs);
        for (int c = 0; c < cfg_.numInputs; ++c) in[c] = &inData[c * n];
        for (int c = 0; c < cfg_.numOutputs; ++c) out[c] = &outData[c * n];

        const auto period = std::uint64_t(1e9 * n / cfg_.sampleRate);
        // A free-running thread never sleeps; asking for a time-constraint
        // budget it always exceeds just gets it throttled.
        if (!freeRun_) rt::promoteCurrentThread(period, period * 3 / 4);
        rt::WorkgroupMembership membership(workgroup());
        membershipOk_ = membership.joined();
        rt::ScopedRealtime realtime;

        std::uint64_t next = rt::nowNs();
        while (running_.load(std::memory_order_relaxed)) {
            const auto t0 = rt::nowNs();
            beginInterval(t0, next + period);
            fn(ctx, in.data(), cfg_.numInputs, out.data(), cfg_.numOutputs,
               n);
            endInterval();
            const auto t1 = rt::nowNs();
            stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
            if (!stats_.samples.push({std::uint32_t(t1 - t0),
                                      std::uint32_t(n)}))
                stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            if (freeRun_) continue;

            // A device would have needed this buffer by the end of the
            // period that started at `next`.
            next += period;
            if (t1 > next) {
                stats_.xruns.fetch_add(1, std::memory_order_relaxed);
                next = t1;  // resync rather than spiral
            }
            rt::sleepUntilNs(next);
        }
    }

    void beginInterval(std::uint64_t startNs, std::uint64_t deadlineNs) {
#if defined(__APPLE__)
        if (interval_ && membershipOk_ &&
            os_workgroup_interval_start(interval_, rt::nsToHostTicks(startNs),
                                        rt::nsToHostTicks(deadlineNs),
                                        nullptr) != 0)
            membershipOk_ = false;  // scheduling hint only; keep running
#endif
    }
    void endInterval() {
#if defined(__APPLE__)
        if (interval_ && membershipOk_ &&
            os_workgroup_interval_finish(interval_, nullptr) != 0)
            membershipOk_ = false;
#endif
    }

    DriverConfig cfg_;
    bool freeRun_;
#if defined(__APPLE__)
    os_workgroup_interval_t interval_ = nullptr;
#endif
    bool membershipOk_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace

std::unique_ptr<Driver> makeSimDriver(const DriverConfig& cfg, bool freeRun) {
    return std::make_unique<SimDriver>(cfg, freeRun);
}

}  // namespace pm
