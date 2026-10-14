// Shared-memory contract between the engine and a plugin host process
// (mix-pluginhost). One region per sandboxed insert.
//
// Block k (seq = k + 1, slot = seq & 1):
//   engine: if done == seq - 1, output out[prev slot]; else output the
//           previous block's input (dry, same latency) and count a miss.
//           Then copy the input into in[slot], publish `request = seq` and
//           ring the doorbell. It never waits.
//   host:   sleeps on the doorbell; processes the newest request into
//           out[slot]; publishes `done = seq`.
// So the host has a full block period, and the insert adds exactly one
// block of latency, reported to PDC like any plugin latency.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace pm::sandbox {

inline constexpr std::uint32_t kMagic = 0x504d5342;  // "PMSB"
inline constexpr std::uint32_t kVersion = 1;
inline constexpr int kMaxChannels = 2;

enum class HostState : std::uint32_t {
    Starting = 0,
    Ready = 1,
    Failed = 2,
};

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::int32_t channels;
    std::int32_t maxBlock;
    std::atomic<std::uint32_t> state;     // HostState
    std::atomic<std::uint32_t> doorbell;  // bumped per request (futex word)
    std::atomic<std::uint64_t> request;   // seq posted by the engine
    std::atomic<std::uint64_t> done;      // seq finished by the host
    std::atomic<std::int32_t> latency;    // plugin's own latency, samples
    std::atomic<std::uint64_t> hostAllocs;  // host-side RT allocations
    std::uint32_t frames[2];              // frames in each slot
    char name[64];                        // plugin display name (host)
};

inline std::size_t bufferFloats(int channels, int maxBlock) {
    return std::size_t(channels) * std::size_t(maxBlock);
}

inline std::size_t regionSize(int channels, int maxBlock) {
    const std::size_t header = (sizeof(Header) + 63) & ~std::size_t(63);
    return header + 4 * bufferFloats(channels, maxBlock) * sizeof(float);
}

// Planar buffers after the header: in[slot][ch][frame], out[slot][ch][frame].
struct View {
    Header* header = nullptr;
    float* base = nullptr;
    int channels = 0, maxBlock = 0;

    static View of(void* region, int channels, int maxBlock) {
        View v;
        v.header = static_cast<Header*>(region);
        const std::size_t header = (sizeof(Header) + 63) & ~std::size_t(63);
        v.base = reinterpret_cast<float*>(static_cast<char*>(region) + header);
        v.channels = channels;
        v.maxBlock = maxBlock;
        return v;
    }
    float* in(int slot, int ch) const {
        return base + (std::size_t(slot) * channels + std::size_t(ch)) * maxBlock;
    }
    float* out(int slot, int ch) const {
        return base + (std::size_t(2 + slot) * channels + std::size_t(ch)) * maxBlock;
    }
};

// Cross-process futex on the doorbell word (macOS 14.4+).
void ring(Header& h);
// Blocks until the doorbell differs from `seen` or `timeoutNs` passes.
void waitDoorbell(Header& h, std::uint32_t seen, std::uint64_t timeoutNs);

}  // namespace pm::sandbox
