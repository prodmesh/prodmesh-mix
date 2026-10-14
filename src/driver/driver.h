// Audio drivers: something that calls the engine once per buffer period on
// a real-time thread. Every driver reports per-callback timing into a
// wait-free queue so benches and the health strip see the same numbers.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "core/rt_thread.h"
#include "core/spsc_queue.h"

namespace pm {

using RenderFn = void (*)(void* ctx, const float* const* in, int numIn,
                          float* const* out, int numOut, int frames);

struct CallbackSample {
    std::uint32_t durationNs = 0;  // time inside the render function
    std::uint32_t frames = 0;
};

struct DriverStats {
    SpscQueue<CallbackSample> samples{1 << 16};
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> xruns{0};
    std::atomic<std::uint64_t> dropped{0};  // samples lost to a full queue
};

struct DriverConfig {
    double sampleRate = 48000;
    int blockSize = 128;
    int numInputs = 0;
    int numOutputs = 2;
};

class Driver {
public:
    virtual ~Driver() = default;
    virtual bool start(RenderFn fn, void* ctx, std::string* error) = 0;
    virtual void stop() = 0;
    virtual const DriverConfig& config() const = 0;
    // The OS audio workgroup render workers should join, if any.
    virtual rt::Workgroup workgroup() const { return nullptr; }
    DriverStats& stats() { return stats_; }

protected:
    DriverStats stats_;
};

// Software clock: a real-time thread waking every period, as a device
// would. `freeRun` skips the sleep to measure raw throughput.
std::unique_ptr<Driver> makeSimDriver(const DriverConfig& cfg, bool freeRun);

#if defined(__APPLE__)
// Default CoreAudio output device, opened and configured (the actual rate
// and block size land in config()). Inputs are not opened (no mic prompt).
std::unique_ptr<Driver> makeCoreAudioDriver(const DriverConfig& cfg,
                                            std::string* error);
#endif

}  // namespace pm
