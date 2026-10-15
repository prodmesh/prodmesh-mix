// Playback ("virtual soundcheck"): a recorded take fed back into the
// source nodes in place of the live inputs, through the whole mix.
//
// A PlaybackSet holds one read-ahead ring per recorded source, all
// starting at the same frame. A reader thread (Player) fills them from the
// WAVs; the audio thread drains them. Wait-free on the audio side, and the
// tracks never drift: a block is played only when every ring has it, and
// otherwise every source is silent for that block and none advances (an
// underrun is counted, never a stall). Start and stop fade over a few ms so
// starting mid-waveform never clicks.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "core/spsc_queue.h"
#include "engine/graph.h"

namespace pm {

class PlaybackFeed {
public:
    PlaybackFeed(NodeId node, int channels, std::size_t capacityFrames)
        : node_(node), ch_(std::max(1, channels)), cap_(capacityFrames),
          data_(std::make_unique<float[]>(capacityFrames * std::size_t(ch_))) {}

    NodeId node() const { return node_; }
    int channels() const { return ch_; }

    // --- reader thread ---------------------------------------------------
    std::size_t space() const {
        return cap_ - (write_.load(std::memory_order_relaxed) -
                       read_.load(std::memory_order_acquire));
    }
    // Interleaved frames; the caller has checked space().
    void write(const float* x, std::size_t frames) {
        const auto w = write_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < frames; ++i)
            std::memcpy(&data_[((w + i) % cap_) * std::size_t(ch_)],
                        &x[i * std::size_t(ch_)], sizeof(float) * std::size_t(ch_));
        write_.store(w + frames, std::memory_order_release);
    }

    // --- audio thread ----------------------------------------------------
    std::size_t available() const {
        return write_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_relaxed);
    }
    // n frames into planar `out` (nch channels: a mono file feeds every
    // channel, a stereo file into a mono node is summed), times a gain
    // ramping g0 -> g1 across the block.
    void read(float* const* out, int nch, int n, float g0, float g1) {
        const auto r = read_.load(std::memory_order_relaxed);
        const float step = n > 0 ? (g1 - g0) / float(n) : 0;
        for (int i = 0; i < n; ++i) {
            const float* f = &data_[((r + std::size_t(i)) % cap_) * std::size_t(ch_)];
            const float g = g0 + step * float(i + 1);
            for (int c = 0; c < nch; ++c) {
                float v;
                if (ch_ == 1) v = f[0];
                else if (nch == 1) v = 0.5f * (f[0] + f[1]);
                else v = f[std::min(c, ch_ - 1)];
                out[c][i] = v * g;
            }
        }
        read_.store(r + std::size_t(n), std::memory_order_release);
    }

private:
    NodeId node_;
    int ch_;
    std::size_t cap_;
    std::unique_ptr<float[]> data_;
    alignas(kCacheLine) std::atomic<std::size_t> write_{0};
    alignas(kCacheLine) std::atomic<std::size_t> read_{0};
};

struct PlaybackSet {
    std::vector<std::unique_ptr<PlaybackFeed>> feeds;  // fixed once published
    std::uint64_t startFrame = 0;  // take frame the rings begin at
    double sampleRate = 48000;

    // Control -> audio: play or stop (fades handle the edges).
    std::atomic<bool> wantRoll{false};
    // Audio -> control.
    std::atomic<std::uint64_t> position{0};  // take frame now playing
    std::atomic<std::uint64_t> underruns{0};
    std::atomic<bool> rolling{false};

    PlaybackFeed* feedFor(NodeId id) const {
        for (const auto& f : feeds)
            if (f->node() == id) return f.get();
        return nullptr;
    }
    bool allHave(std::size_t n) const {
        for (const auto& f : feeds)
            if (f->available() < n) return false;
        return true;
    }

    // --- audio thread, once per block before rendering ----------------------
    // Decides whether this block plays and the gain ramp it plays with.
    struct Block {
        bool play = false;
        float g0 = 0, g1 = 0;
    };
    Block begin(int n, int fadeFrames) {
        Block b;
        const bool want = wantRoll.load(std::memory_order_acquire);
        if (!want && gain_ <= 0.0f) {
            rolling.store(false, std::memory_order_release);
            return b;
        }
        if (!allHave(std::size_t(n))) {  // stay aligned: nobody advances
            if (want) underruns.fetch_add(1, std::memory_order_relaxed);
            return b;
        }
        const float step = float(n) / float(std::max(1, fadeFrames));
        b.play = true;
        b.g0 = gain_;
        gain_ = want ? std::min(1.0f, gain_ + step) : std::max(0.0f, gain_ - step);
        b.g1 = gain_;
        rolling.store(true, std::memory_order_release);
        return b;
    }
    // After rendering a block that played.
    void advance(int n) {
        position.store(position.load(std::memory_order_relaxed) + std::uint64_t(n),
                       std::memory_order_release);
    }

private:
    float gain_ = 0;  // audio thread only
};

}  // namespace pm
