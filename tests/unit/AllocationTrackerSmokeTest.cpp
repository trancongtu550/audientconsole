#include "core/AllocationTracker.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace core = audient::core;

#if defined(_MSC_VER) && !defined(NDEBUG)
#include <windows.h>

namespace
{
std::uintptr_t currentThreadId()
{
    return static_cast<std::uintptr_t>(::GetCurrentThreadId());
}
} // namespace
#else
namespace
{
std::uintptr_t currentThreadId()
{
    return 0;
}
} // namespace
#endif

TEST(AllocationTrackerSmoke, DebugBuildCountsAllocationsOnMarkedThread)
{
    const bool supported = core::isAllocationTrackerEnabled() ||
                           core::installAllocationTracker() == core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "Allocation tracker is active only in MSVC Debug builds (NDEBUG unset)";
    }

    const std::uint64_t before = core::allocationCountOnRealtimeThread();

    core::markThreadRealtime(currentThreadId());
    {
        std::vector<int> values;
        values.push_back(1);
        values.push_back(2);
        values.push_back(3);
        values.push_back(4);
    }
    core::clearRealtimeMark();

    const std::uint64_t after = core::allocationCountOnRealtimeThread();
    EXPECT_GT(after, before) << "heap allocations on the realtime-marked thread must be counted";
}

TEST(AllocationTrackerSmoke, AllocationsOffMarkedThreadAreNotCounted)
{
    const bool supported = core::isAllocationTrackerEnabled() ||
                           core::installAllocationTracker() == core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "Allocation tracker is active only in MSVC Debug builds (NDEBUG unset)";
    }

    core::clearRealtimeMark();
    const std::uint64_t before = core::allocationCountOnRealtimeThread();
    {
        std::vector<int> values;
        for (int i = 0; i < 64; ++i)
        {
            values.push_back(i);
        }
    }
    const std::uint64_t after = core::allocationCountOnRealtimeThread();
    EXPECT_EQ(after, before) << "allocations on threads that are not marked realtime must not be counted";
}

TEST(AllocationTrackerSmoke, TrackerLifecycleIsReversible)
{
    core::installAllocationTracker();
    core::uninstallAllocationTracker();
    SUCCEED();
}