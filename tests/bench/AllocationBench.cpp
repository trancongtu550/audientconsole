#include "bench/Bench.h"
#include "core/AllocationTracker.h"

#include <cstdint>
#include <cstdio>
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

void runAllocationBench()
{
    std::printf("AudientConsole benchmark: allocation detection\n");

    const bool enabled = core::isAllocationTrackerEnabled() ||
                         core::installAllocationTracker() == core::TrackerState::Installed;
    std::printf("allocation tracker enabled: %s\n", enabled ? "yes" : "no");

    const std::uint64_t before = core::allocationCountOnRealtimeThread();

    if (enabled)
    {
        core::markThreadRealtime(currentThreadId());
    }
    {
        std::vector<std::vector<int>*> blocks;
        blocks.reserve(10000);
        for (int i = 0; i < 10000; ++i)
        {
            blocks.push_back(new std::vector<int>(32, i));
        }
        for (std::vector<int>* block : blocks)
        {
            delete block;
        }
    }
    if (enabled)
    {
        core::clearRealtimeMark();
    }

    const std::uint64_t detected = core::allocationCountOnRealtimeThread() - before;
    std::printf("allocations observed on the realtime-marked thread: %llu\n",
                static_cast<unsigned long long>(detected));

    if (enabled)
    {
        std::printf("PASS: detector observed allocations in this build\n");
        return;
    }

    std::printf("WARN: detector is compiled out in this build configuration (MSVC Release); " 
                "run a Debug build to observe allocations\n");
}