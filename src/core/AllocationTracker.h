#pragma once

#include <cstdint>

namespace audient::core {

enum class TrackerState
{
    NotInstalled,
    Installed,
};

TrackerState installAllocationTracker();
void uninstallAllocationTracker();
void markThreadRealtime(std::uintptr_t osThreadId);
void clearRealtimeMark();
std::uint64_t allocationCountOnRealtimeThread();
bool isAllocationTrackerEnabled();

} // namespace audient::core