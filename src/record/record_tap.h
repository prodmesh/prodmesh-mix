// A recording tap: where the audio thread hands a node's output to the
// disk writer. Wait-free on the audio side; the writer drains it at leisure.
//
// If the ring is full (the disk stalled longer than the ring covers), the
// block is dropped and the gap is queued with its position, so the writer
// can fill it with silence and every file of a take stays sample-aligned.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

#include "core/spsc_queue.h"

namespace pm {

class RecordTap {
public:
    struct Gap {
        std::uint64_t atFrame = 0;  // in pushed-frame coordinates
        std::uint32_t frames = 0;
    };

    RecordTap(int channels, std::size_t capacityFrames)
        : channels_(channels),
          capacity_(capacityFrames),
          data_(std::make_unique<float[]>(capacityFrames * std::size_t(channels))) {}

    int channels() const { return channels_; }
    std::size_t capacityFrames() const { return capacity_; }

    // --- audio thread ---------------------------------------------------

    // Appends `n` frames (planar in, interleaved in the ring). `sampleTime`
    // is the engine's clock at the first frame, kept for the file header.
    void push(const float* const* ch, int n, std::uint64_t sampleTime) {
        if (!started_.load(std::memory_order_relaxed)) {
            startSample_.store(sampleTime, std::memory_order_relaxed);
            started_.store(true, std::memory_order_release);
        }
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        const bool fits = capacity_ - (w - r) >= std::size_t(n);
        // Room again after a stall: record the gap before this block.
        if (pendingGap_ && fits && gaps_.push({w, pendingGap_}))
            pendingGap_ = 0;
        if (!fits || pendingGap_) {
            pendingGap_ += std::uint32_t(n);
            dropped_.fetch_add(std::uint64_t(n), std::memory_order_relaxed);
            return;
        }
        for (int i = 0; i < n; ++i) {
            float* dst = &data_[((w + std::size_t(i)) % capacity_) *
                                std::size_t(channels_)];
            for (int c = 0; c < channels_; ++c) dst[c] = ch[c][i];
        }
        write_.store(w + std::size_t(n), std::memory_order_release);
    }

    // --- writer thread --------------------------------------------------

    // Copies up to `maxFrames` interleaved frames into `out`; returns how
    // many. `position` receives the pushed-frame index of the first one.
    std::size_t pop(float* out, std::size_t maxFrames, std::uint64_t* position) {
        const auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        const std::size_t n = std::min<std::size_t>(w - r, maxFrames);
        for (std::size_t i = 0; i < n; ++i)
            std::memcpy(&out[i * std::size_t(channels_)],
                        &data_[((r + i) % capacity_) * std::size_t(channels_)],
                        sizeof(float) * std::size_t(channels_));
        if (position) *position = r;
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    // Frames ready to pop. Acquires the audio thread's latest write, so a
    // gap queued before that data is visible to a following nextGap().
    std::size_t available() const {
        return write_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_relaxed);
    }

    // Oldest unconsumed gap, in pushed-frame coordinates.
    bool nextGap(Gap& g) {
        if (auto v = gaps_.pop()) {
            g = *v;
            return true;
        }
        return false;
    }

    bool started() const { return started_.load(std::memory_order_acquire); }
    std::uint64_t startSample() const {
        return startSample_.load(std::memory_order_relaxed);
    }
    std::uint64_t droppedFrames() const {
        return dropped_.load(std::memory_order_relaxed);
    }
    // Fraction of the ring in use: the disk writer's headroom indicator.
    float fill() const {
        return float(write_.load() - read_.load()) / float(capacity_);
    }

private:
    int channels_;
    std::size_t capacity_;
    std::unique_ptr<float[]> data_;
    alignas(kCacheLine) std::atomic<std::size_t> write_{0};
    std::uint32_t pendingGap_ = 0;  // audio thread only
    alignas(kCacheLine) std::atomic<std::size_t> read_{0};
    SpscQueue<Gap> gaps_{256};
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> startSample_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace pm
