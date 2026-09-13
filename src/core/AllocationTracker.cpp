#include "core/AllocationTracker.h"

#include <atomic>

#if defined(_MSC_VER) && !defined(NDEBUG)
#include <windows.h>
#include <crtdbg.h>
#endif

namespace
{

std::atomic<std::uintptr_t> g_realtimeThreadId{0};
std::atomic<std::uint64_t> g_allocationCount{0};
std::atomic<bool> g_installed{false};

#if defined(_MSC_VER) && !defined(NDEBUG)
int audientCrtAllocHook(int allocType, void*, size_t, int, long, const unsigned char*, int)
{
    if (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)
    {
        const std::uintptr_t tid = static_cast<std::uintptr_t>(::GetCurrentThreadId());
        if (tid == g_realtimeThreadId.load(std::memory_order_relaxed))
        {
            g_allocationCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return TRUE;
}
#endif

} // namespace

namespace audient::core
{

TrackerState installAllocationTracker()
{
#if defined(_MSC_VER) && !defined(NDEBUG)
    bool expected = false;
    if (g_installed.compare_exchange_strong(expected, true))
    {
        ::_CrtSetAllocHook(&audientCrtAllocHook);
    }
    return TrackerState::Installed;
#else
    return TrackerState::NotInstalled;
#endif
}

void uninstallAllocationTracker()
{
#if defined(_MSC_VER) && !defined(NDEBUG)
    g_installed.store(false, std::memory_order_relaxed);
    ::_CrtSetAllocHook(nullptr);
#endif
}

void markThreadRealtime(std::uintptr_t osThreadId)
{
    g_realtimeThreadId.store(osThreadId, std::memory_order_relaxed);
}

void clearRealtimeMark()
{
    g_realtimeThreadId.store(0, std::memory_order_relaxed);
}

std::uint64_t allocationCountOnRealtimeThread()
{
    return g_allocationCount.load(std::memory_order_relaxed);
}

bool isAllocationTrackerEnabled()
{
#if defined(_MSC_VER) && !defined(NDEBUG)
    return g_installed.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

} // namespace audient::core