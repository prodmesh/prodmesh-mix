// Wait-free single-producer / single-consumer ring. The only primitive the
// audio thread uses to talk to anything else: no locks, no allocation after
// construction, and every operation is a bounded number of atomic ops.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>

namespace pm {

// Keeps producer and consumer indices on separate cache lines so the two
// threads don't false-share. Apple Silicon uses 128-byte lines.
inline constexpr std::size_t kCacheLine = 128;

template <typename T>
class SpscQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "audio-thread payloads must move without throwing");

public:
    // Capacity is rounded up to a power of two so wrap is a mask.
    explicit SpscQueue(std::size_t capacity)
        : mask_(roundUp(capacity) - 1),
          slots_(std::make_unique<Slot[]>(mask_ + 1)) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    std::size_t capacity() const { return mask_ + 1; }

    // Producer side. Returns false (and drops nothing) when full.
    bool push(T value) noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - readCache_ > mask_) {
            readCache_ = read_.load(std::memory_order_acquire);
            if (w - readCache_ > mask_) return false;
        }
        slots_[w & mask_].value = std::move(value);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.
    std::optional<T> pop() noexcept {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == writeCache_) {
            writeCache_ = write_.load(std::memory_order_acquire);
            if (r == writeCache_) return std::nullopt;
        }
        std::optional<T> out(std::move(slots_[r & mask_].value));
        read_.store(r + 1, std::memory_order_release);
        return out;
    }

    // Approximate; exact only when called from one of the two endpoints
    // while the other is idle.
    std::size_t sizeApprox() const noexcept {
        return write_.load(std::memory_order_acquire) -
               read_.load(std::memory_order_acquire);
    }

private:
    struct Slot { T value{}; };

    static std::size_t roundUp(std::size_t n) {
        std::size_t p = 2;
        while (p < n) p <<= 1;
        return p;
    }

    const std::size_t mask_;
    std::unique_ptr<Slot[]> slots_;

    alignas(kCacheLine) std::atomic<std::size_t> write_{0};
    std::size_t readCache_ = 0;  // producer's view of read_
    alignas(kCacheLine) std::atomic<std::size_t> read_{0};
    std::size_t writeCache_ = 0;  // consumer's view of write_
};

}  // namespace pm
