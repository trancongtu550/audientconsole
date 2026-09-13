#include "core/AllocationTracker.h"
#include "engine/EngineGraph.h"
#include "engine/EngineStreamData.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>

#if defined(_MSC_VER) && !defined(NDEBUG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

namespace core = audient::core;

namespace
{

constexpr std::size_t kFrames = 64;
constexpr std::size_t kBlocks = 4;

audient::engine::EngineConfig baseConfig()
{
    audient::engine::EngineConfig config;
    config.sampleRateHz = 48000;
    config.maxBlockSamples = 128;
    config.revision = 1;
    return config;
}

class GraphFixture
{
public:
    GraphFixture(audient::engine::EngineConfig config)
    {
        graph.publishConfig(config);
        d.physicalInputMono = physicalInput.data();
        d.physicalOutputLeft = outputLeft.data();
        d.physicalOutputRight = outputRight.data();
        d.micUplinkMono = micUplink.data();
        d.systemDownlinkLeft = downlinkLeft.data();
        d.systemDownlinkRight = downlinkRight.data();

        for (std::size_t i = 0; i < kFrames; ++i)
        {
            physicalInput[i] = 0.0f;
            downlinkLeft[i] = 0.0f;
            downlinkRight[i] = 0.0f;
            outputLeft[i] = 0.0f;
            outputRight[i] = 0.0f;
            micUplink[i] = 0.0f;
        }
        d.frames = static_cast<long>(kFrames);
    }

    void run()
    {
        for (std::size_t block = 0; block < kBlocks; ++block)
        {
            graph.process(d);
        }
    }

    void setPhysicalInput(float value)
    {
        physicalInput.fill(value);
    }

    void setDownlink(float left, float right)
    {
        downlinkLeft.fill(left);
        downlinkRight.fill(right);
    }

    audient::engine::EngineGraph graph{128};
    audient::engine::EngineStreamData d{};
    std::array<float, kFrames> physicalInput{};
    std::array<float, kFrames> downlinkLeft{};
    std::array<float, kFrames> downlinkRight{};
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    std::array<float, kFrames> micUplink{};
};

} // namespace

TEST(EngineGraphTest, DownlinkNeverLeaksIntoMicUplink)
{
    audient::engine::EngineConfig config = baseConfig();
    config.micMonitorMute = true;

    GraphFixture fixture(config);
    fixture.setPhysicalInput(0.0f);
    fixture.setDownlink(0.9f, -0.9f);
    fixture.run();

    for (const float sample : fixture.micUplink)
    {
        EXPECT_EQ(sample, 0.0f) << "downlink/desktop audio must never enter the mic uplink";
    }
    EXPECT_NEAR(fixture.outputLeft[0], 0.9f, 1e-4f);
    EXPECT_NEAR(fixture.outputRight[0], -0.9f, 1e-4f);
}

TEST(EngineGraphTest, MonitorBranchCarriesOnlyProcessedMic)
{
    audient::engine::EngineConfig config = baseConfig();
    config.micMonitorMute = false;
    config.micMonitorDb = 0.0f;
    config.micSendDb = 0.0f;

    GraphFixture fixture(config);
    fixture.setPhysicalInput(0.5f);
    fixture.setDownlink(0.0f, 0.0f);
    fixture.run();

    EXPECT_NEAR(fixture.micUplink[0], 0.5f, 1e-4f);
    EXPECT_NEAR(fixture.outputLeft[0], 0.5f, 1e-4f) << "monitor branch must route the processed mic to physical output";
    EXPECT_NEAR(fixture.outputRight[0], 0.5f, 1e-4f);
}

TEST(EngineGraphTest, PanicMuteZerosPhysicalPathOnly)
{
    audient::engine::EngineConfig config = baseConfig();
    config.micMonitorMute = true;
    config.panicMute = true;

    GraphFixture fixture(config);
    fixture.setPhysicalInput(0.75f);
    fixture.setDownlink(0.8f, 0.8f);
    fixture.run();

    for (const float sample : fixture.outputLeft)
    {
        EXPECT_EQ(sample, 0.0f);
    }
    for (const float sample : fixture.outputRight)
    {
        EXPECT_EQ(sample, 0.0f);
    }
    EXPECT_NEAR(fixture.micUplink[0], 0.75f, 1e-4f) << "panic must not silence the mic uplink";
}

TEST(EngineGraphTest, IndependentMutesAffectOnlyTheirPaths)
{
    audient::engine::EngineConfig config = baseConfig();
    config.micMonitorMute = true;
    config.micSendMute = true;

    GraphFixture fixture(config);
    fixture.setPhysicalInput(0.4f);
    fixture.setDownlink(0.6f, 0.6f);
    fixture.run();

    for (const float sample : fixture.micUplink)
    {
        EXPECT_EQ(sample, 0.0f) << "mic send mute must stop the uplink";
    }
    EXPECT_NEAR(fixture.outputLeft[0], 0.6f, 1e-4f) << "downlink must be unaffected by mic send mute";

    audient::engine::EngineConfig downlinkMuted = baseConfig();
    downlinkMuted.micMonitorMute = true;
    downlinkMuted.downlinkMute = true;

    GraphFixture other(downlinkMuted);
    other.setPhysicalInput(0.4f);
    other.setDownlink(0.8f, 0.8f);
    other.run();

    for (const float sample : other.outputLeft)
    {
        EXPECT_EQ(sample, 0.0f) << "downlink mute must remove desktop audio from the physical output";
    }
    EXPECT_NEAR(other.micUplink[0], 0.4f, 1e-4f);
}

TEST(EngineGraphTest, FullScaleSourcesStayBounded)
{
    audient::engine::EngineConfig config = baseConfig();
    config.micMonitorMute = false;
    config.micMonitorDb = 0.0f;
    config.micSendDb = 0.0f;

    GraphFixture fixture(config);
    fixture.setPhysicalInput(1.0f);
    fixture.setDownlink(1.0f, 1.0f);
    fixture.run();

    float maxMagnitude = 0.0f;
    for (const float sample : fixture.outputLeft)
    {
        maxMagnitude = std::max(maxMagnitude, std::fabs(sample));
        ASSERT_LE(sample, 1.0f);
        ASSERT_GE(sample, -1.0f);
    }
    for (const float sample : fixture.outputRight)
    {
        maxMagnitude = std::max(maxMagnitude, std::fabs(sample));
        ASSERT_LE(sample, 1.0f);
        ASSERT_GE(sample, -1.0f);
    }
    EXPECT_EQ(maxMagnitude, 1.0f) << "full-scale monitor + downlink engages the limiter at exactly full scale";
}

TEST(EngineGraphTest, TrimAppliesDecibels)
{
    audient::engine::EngineConfig config = baseConfig();
    config.micMonitorMute = true;
    config.micSendDb = 0.0f;
    config.inputTrimDb = -12.041199826559248f;

    GraphFixture fixture(config);
    fixture.setPhysicalInput(1.0f);
    fixture.setDownlink(0.0f, 0.0f);
    fixture.run();

    EXPECT_NEAR(fixture.micUplink[0], 0.25f, 1e-4f);
}

TEST(EngineGraphTest, GraphProcessingAllocatesNothing)
{
    bool supported = core::isAllocationTrackerEnabled() ||
                     core::installAllocationTracker() == core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker is active only when NDEBUG is unset";
    }

    audient::engine::EngineConfig config = baseConfig();
    config.micMonitorMute = true;

    GraphFixture fixture(config);
    fixture.setPhysicalInput(0.5f);
    fixture.setDownlink(0.5f, -0.5f);

    const std::uint64_t before = core::allocationCountOnRealtimeThread();
    core::markThreadRealtime(currentThreadId());
    for (std::size_t block = 0; block < 500; ++block)
    {
        fixture.graph.process(fixture.d);
    }
    core::clearRealtimeMark();

    EXPECT_EQ(core::allocationCountOnRealtimeThread(), before) << "graph processing must be allocation-free";
}