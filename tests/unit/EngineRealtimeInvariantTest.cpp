#include "core/AllocationTracker.h"
#include "engine/ChannelControls.h"
#include "engine/Counters.h"
#include "engine/GraphConfig.h"
#include "engine/GraphConfigSnapshot.h"
#include "engine/Meters.h"
#include "engine/RingBuffer.h"
#include "engine/SanitizeAudio.h"
#include "engine/TestSignal.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER) && !defined(NDEBUG)
#include <windows.h>

namespace core = audient::core;

namespace
{
std::uintptr_t currentThreadId()
{
    return static_cast<std::uintptr_t>(::GetCurrentThreadId());
}
} // namespace
#else
namespace core = audient::core;

namespace
{
std::uintptr_t currentThreadId()
{
    return 0;
}
} // namespace
#endif

TEST(EngineRealtimeInvariantTest, CallbackBodyAllocatesZeroBytes)
{
    const bool supported = core::isAllocationTrackerEnabled() ||
                           core::installAllocationTracker() == core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker is active only when NDEBUG is unset (Debug/RelWithDebInfo)";
    }

    constexpr std::size_t kFrames = 64;

    audient::engine::LockFreeRingBuffer ring(1024);
    audient::engine::ChannelControls controls;
    controls.setRampSamples(0);
    audient::engine::PeakRmsMeter meter;
    audient::engine::EngineCounters counters;
    audient::engine::EngineConfigSnapshot config;
    config.publish(audient::engine::EngineConfig{});
    audient::engine::TestSignalSource signal;
    signal.configure(1000.0, -18.0, 48000, 0.0);

    std::array<float, kFrames> mono{};
    std::array<float, kFrames> out{};
    audient::engine::SanitizeCounters sanitizeCounters;

    const std::uint64_t baseline = core::allocationCountOnRealtimeThread();
    core::markThreadRealtime(currentThreadId());

    for (int i = 0; i < 20000; ++i)
    {
        const audient::engine::EngineConfig cfg = config.read();
        (void)cfg;

        controls.processMono(mono.data(), kFrames);
        audient::engine::sanitizeBlock(mono.data(), kFrames, sanitizeCounters);
        counters.recordCallback();
        counters.recordSequenceStep();

        ring.writeBlock(mono.data(), kFrames);
        ring.readBlock(out.data(), kFrames);

        meter.feed(mono.data(), kFrames, 1);
        signal.fillMono(out.data(), kFrames);
    }

    core::clearRealtimeMark();
    const std::uint64_t grown = core::allocationCountOnRealtimeThread();

    EXPECT_EQ(grown, baseline) << "the engine callback body must not allocate on the realtime thread";
}