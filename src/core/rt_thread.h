// Real-time thread setup. On macOS a thread only gets audio-grade
// scheduling if it asks for a time-constraint policy sized to the buffer
// period, and on Apple Silicon parallel render threads must also join the
// device's audio workgroup or the scheduler may park them on E-cores.
#pragma once

#include <cstdint>

namespace pm::rt {

// Monotonic nanoseconds, cheap enough for per-callback timing.
std::uint64_t nowNs();

// Puts the calling thread in the time-constraint class. `periodNs` is the
// buffer period; `computeNs` the expected work per period.
bool promoteCurrentThread(std::uint64_t periodNs, std::uint64_t computeNs);

// Sleeps the calling thread until the absolute `nowNs()` deadline.
void sleepUntilNs(std::uint64_t deadlineNs);

// Host clock ticks (mach_absolute_time on macOS), the unit OS workgroup
// intervals are expressed in.
std::uint64_t nsToHostTicks(std::uint64_t ns);

// Opaque handle to an OS audio workgroup (os_workgroup_t on macOS); null
// when unavailable.
using Workgroup = void*;

// Joins `wg` for the lifetime of the object. A null workgroup is a no-op.
class WorkgroupMembership {
public:
    explicit WorkgroupMembership(Workgroup wg);
    ~WorkgroupMembership();
    WorkgroupMembership(const WorkgroupMembership&) = delete;
    WorkgroupMembership& operator=(const WorkgroupMembership&) = delete;
    bool joined() const { return joined_; }

private:
    Workgroup wg_;
    bool joined_ = false;
    alignas(8) unsigned char token_[64]{};  // os_workgroup_join_token_s
};

}  // namespace pm::rt
