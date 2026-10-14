#include "core/rt_guard.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace pm::rt {
namespace {

thread_local bool tlsRealtime = false;
std::atomic<std::uint64_t> gViolations{0};
std::atomic<Policy> gPolicy{Policy::Count};

void onViolation(const char* what) {
    gViolations.fetch_add(1, std::memory_order_relaxed);
    if (gPolicy.load(std::memory_order_relaxed) == Policy::Abort) {
        tlsRealtime = false;  // let the report itself allocate if it must
        std::fprintf(stderr, "pm::rt violation: %s on a real-time thread\n",
                     what);
        std::abort();
    }
}

}  // namespace

void setPolicy(Policy p) { gPolicy.store(p); }
Policy policy() { return gPolicy.load(); }
std::uint64_t violations() { return gViolations.load(); }
void resetViolations() { gViolations.store(0); }
bool isRealtimeThread() { return tlsRealtime; }

ScopedRealtime::ScopedRealtime() : previous_(tlsRealtime) {
    tlsRealtime = true;
}
ScopedRealtime::~ScopedRealtime() { tlsRealtime = previous_; }

ScopedNonRealtime::ScopedNonRealtime() : previous_(tlsRealtime) {
    tlsRealtime = false;
}
ScopedNonRealtime::~ScopedNonRealtime() { tlsRealtime = previous_; }

// Hooks used by the global operator new/delete replacements below.
void noteAlloc() {
    if (tlsRealtime) onViolation("allocation");
}
void noteFree(void* p) {
    if (p && tlsRealtime) onViolation("deallocation");
}

}  // namespace pm::rt

// Global replacements. They live in this translation unit so any binary
// that uses ScopedRealtime links them in.
void* operator new(std::size_t n) {
    pm::rt::noteAlloc();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    pm::rt::noteAlloc();
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
void* operator new(std::size_t n, std::align_val_t a) {
    pm::rt::noteAlloc();
    const auto al = static_cast<std::size_t>(a);
    if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}

void operator delete(void* p) noexcept {
    pm::rt::noteFree(p);
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept {
    ::operator delete(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    pm::rt::noteFree(p);
    std::free(p);
}
void operator delete[](void* p, std::align_val_t a) noexcept {
    ::operator delete(p, a);
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept {
    ::operator delete(p, a);
}
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept {
    ::operator delete(p, a);
}
