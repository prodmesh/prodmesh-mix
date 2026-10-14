#include "plugin/sandbox_shm.h"

#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>

namespace pm::sandbox {

void ring(Header& h) {
    h.doorbell.fetch_add(1, std::memory_order_release);
    os_sync_wake_by_address_any(&h.doorbell, sizeof(std::uint32_t),
                                OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

void waitDoorbell(Header& h, std::uint32_t seen, std::uint64_t timeoutNs) {
    os_sync_wait_on_address_with_timeout(
        &h.doorbell, seen, sizeof(std::uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
        OS_CLOCK_MACH_ABSOLUTE_TIME, timeoutNs);
}

}  // namespace pm::sandbox
