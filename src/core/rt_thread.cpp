#include "core/rt_thread.h"

#include <chrono>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/workgroup.h>
#include <pthread.h>
#endif

namespace pm::rt {

#if defined(__APPLE__)
namespace {

const mach_timebase_info_data_t& timebase() {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    return tb;
}

}  // namespace

std::uint64_t nsToHostTicks(std::uint64_t ns) {
    const auto& tb = timebase();
    return ns * tb.denom / tb.numer;
}

std::uint64_t nowNs() {
    const auto& tb = timebase();
    return mach_absolute_time() * tb.numer / tb.denom;
}

bool promoteCurrentThread(std::uint64_t periodNs, std::uint64_t computeNs) {
    thread_time_constraint_policy_data_t p{};
    p.period = static_cast<uint32_t>(nsToHostTicks(periodNs));
    p.computation = static_cast<uint32_t>(nsToHostTicks(computeNs));
    p.constraint = p.period;
    p.preemptible = 1;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                             THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&p),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) ==
           KERN_SUCCESS;
}

void sleepUntilNs(std::uint64_t deadlineNs) {
    const auto& tb = timebase();
    mach_wait_until(deadlineNs * tb.denom / tb.numer);
}

static_assert(sizeof(os_workgroup_join_token_s) <= 64,
              "grow WorkgroupMembership::token_");

WorkgroupMembership::WorkgroupMembership(Workgroup wg) : wg_(wg) {
    if (!wg_) return;
    auto* token = reinterpret_cast<os_workgroup_join_token_t>(token_);
    joined_ = os_workgroup_join(static_cast<os_workgroup_t>(wg_), token) == 0;
}

WorkgroupMembership::~WorkgroupMembership() {
    if (!joined_) return;
    auto* token = reinterpret_cast<os_workgroup_join_token_t>(token_);
    os_workgroup_leave(static_cast<os_workgroup_t>(wg_), token);
}

#else

std::uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool promoteCurrentThread(std::uint64_t, std::uint64_t) { return false; }

void sleepUntilNs(std::uint64_t deadlineNs) {
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(deadlineNs)));
}

std::uint64_t nsToHostTicks(std::uint64_t ns) { return ns; }

WorkgroupMembership::WorkgroupMembership(Workgroup wg) : wg_(wg) {}
WorkgroupMembership::~WorkgroupMembership() = default;

#endif

}  // namespace pm::rt
