// Real-time safety guard. Any thread inside a ScopedRealtime is treated as
// an audio thread: heap allocation or deallocation through operator new /
// delete there is a Tier 0 bug. The guard counts violations (release) or
// aborts with a message (tests, debug), so CI catches them before a show
// does.
//
// Apple clang has no RealtimeSanitizer (-fsanitize=realtime) yet; when it
// does, run it in CI too. This guard only sees C++ allocation, not raw
// malloc or locks.
#pragma once

#include <atomic>
#include <cstdint>

// Marks a function whose whole dynamic extent must be real-time safe.
// Under RealtimeSanitizer (-DPM_RTSAN=ON, LLVM 20+) any allocation, lock or
// blocking syscall inside it aborts with a stack trace.
#if defined(PM_RTSAN_ENABLED)
#define PM_NONBLOCKING [[clang::nonblocking]]
#else
#define PM_NONBLOCKING
#endif

namespace pm::rt {

enum class Policy { Count, Abort };

void setPolicy(Policy p);
Policy policy();

// Total violations observed on any real-time thread since start / reset.
std::uint64_t violations();
void resetViolations();

bool isRealtimeThread();

class ScopedRealtime {
public:
    ScopedRealtime();
    ~ScopedRealtime();
    ScopedRealtime(const ScopedRealtime&) = delete;
    ScopedRealtime& operator=(const ScopedRealtime&) = delete;

private:
    bool previous_;
};

// Marks a call into third-party code (a plugin's process) on a real-time
// thread: allocations inside are added to `counter` — attributed to that
// plugin — instead of the engine's own violation count, and never abort.
class ScopedThirdParty {
public:
    explicit ScopedThirdParty(std::atomic<std::uint64_t>* counter);
    ~ScopedThirdParty();
    ScopedThirdParty(const ScopedThirdParty&) = delete;
    ScopedThirdParty& operator=(const ScopedThirdParty&) = delete;

private:
    std::atomic<std::uint64_t>* previous_;
};

// Temporarily lifts the guard, e.g. around a deliberately non-RT call in a
// test. Never use in engine code.
class ScopedNonRealtime {
public:
    ScopedNonRealtime();
    ~ScopedNonRealtime();

private:
    bool previous_;
};

}  // namespace pm::rt
