#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioStreamBridge.h"
#include "engine/GraphConfig.h"
#include "engine/EngineGraph.h"
#include "engine/SyntheticDownlink.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace audient::asio;

namespace
{

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

ChannelPlan planFromCapabilities(const DriverCapabilities& caps)
{
    std::vector<ChannelInfo> inputs;
    std::vector<ChannelInfo> outputs;
    for (const ChannelInfo& channel : caps.channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }
    return AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
}

float peakMagnitude(const std::vector<float>& data)
{
    float peak = 0.0f;
    for (const float sample : data)
    {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

} // namespace

TEST(AsioStream, LiveMicUplinkAndSyntheticStereoDownlink)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    audient::engine::EngineGraph graph(256);
    audient::engine::EngineConfig config;
    config.sampleRateHz = 48000;
    config.maxBlockSamples = 256;
    config.micMonitorMute = true;
    config.micSendDb = 0.0f;
    config.revision = 1;
    graph.publishConfig(config);

    AsioStreamBridge bridge(backend);
    bridge.attach(graph, 256);

    audient::engine::SyntheticDownlink synthetic;
    synthetic.configure(440.0, 880.0, -20.0, 48000);
    bridge.enableSyntheticDownlink(&synthetic);

    bridge.requestFadeIn();
    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state output is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> outLeft = driver->lastOutput(0);
    const std::vector<float> outRight = driver->lastOutput(1);
    ASSERT_FALSE(outLeft.empty());
    ASSERT_FALSE(outRight.empty());

    ASSERT_TRUE(backend.stop(error));

    const std::vector<float>& uplink = bridge.micUplinkBuffer();
    ASSERT_FALSE(uplink.empty());
    EXPECT_NEAR(uplink[0], 0.25f, 1e-3f) << "simulated physical input (0.25) must reach the mic uplink at unity send";

#if defined(NDEBUG)
    GTEST_SKIP() << "synthetic downlink is diagnostic-only; physical-content assertions apply in Debug/RelWithDebInfo";
#else
    EXPECT_NEAR(peakMagnitude(outLeft), 0.1f, 0.02f) << "left output must carry the -20 dBFS synthetic downlink";
    EXPECT_NEAR(peakMagnitude(outRight), 0.1f, 0.02f) << "right output must carry the -20 dBFS synthetic downlink";

    double differ = 0.0;
    for (std::size_t i = 0; i < outLeft.size(); ++i)
    {
        const double delta = static_cast<double>(outLeft[i]) - static_cast<double>(outRight[i]);
        differ += delta * delta;
    }
    EXPECT_GT(differ, 1e-6) << "left/right downlink content must remain distinct (mapping preserved)";

    EXPECT_LE(peakMagnitude(outLeft), 0.15f) << "with monitor muted the physical output must not carry the 0.25 mic signal";
    EXPECT_LE(peakMagnitude(outRight), 0.15f);
#endif
}

TEST(AsioStream, MonitorBranchReachedPhysicalOutput)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error));

    audient::engine::EngineGraph graph(256);
    audient::engine::EngineConfig config;
    config.sampleRateHz = 48000;
    config.maxBlockSamples = 256;
    config.micMonitorMute = false;
    config.micMonitorDb = 0.0f;
    config.downlinkMute = true;
    config.revision = 1;
    graph.publishConfig(config);

    AsioStreamBridge bridge(backend);
    bridge.attach(graph, 256);
    bridge.requestFadeIn();
    ASSERT_TRUE(backend.start(error));
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state output is read";

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> outLeft = driver->lastOutput(0);
    const std::vector<float> outRight = driver->lastOutput(1);
    ASSERT_FALSE(outLeft.empty());
    ASSERT_FALSE(outRight.empty());

    ASSERT_TRUE(backend.stop(error));

    EXPECT_NEAR(peakMagnitude(outLeft), 0.25f, 0.02f) << "processed-mic monitor (0.25) must reach the physical output";
    EXPECT_NEAR(peakMagnitude(outRight), 0.25f, 0.02f);
    EXPECT_NEAR(peakMagnitude(outRight), 0.25f, 0.02f);
}

TEST(AsioStream, FadeInReachesFullAndFadeOutStopsAtSilence)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    audient::engine::EngineGraph graph(256);
    audient::engine::EngineConfig config;
    config.sampleRateHz = 48000;
    config.maxBlockSamples = 256;
    config.micMonitorMute = false;
    config.micMonitorDb = 0.0f;
    config.revision = 1;
    graph.publishConfig(config);

    AsioStreamBridge bridge(backend);
    bridge.attach(graph, 256);
    bridge.configureStreamFade(256);

    bridge.requestFadeIn();
    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in to unity";

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> steadyLeft = driver->lastOutput(0);
    ASSERT_FALSE(steadyLeft.empty());
    EXPECT_NEAR(peakMagnitude(steadyLeft), 0.25f, 0.02f) << "fade-in must reach the full mic signal";

    bridge.requestFadeOut();
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsMuted(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade out to silence";
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const std::vector<float> mutedLeft = driver->lastOutput(0);
    const std::vector<float> mutedRight = driver->lastOutput(1);
    ASSERT_FALSE(mutedLeft.empty());
    ASSERT_FALSE(mutedRight.empty());
    EXPECT_LE(peakMagnitude(mutedLeft), 1e-4f) << "a graceful fade-out stop must end at digital silence on L";
    EXPECT_LE(peakMagnitude(mutedRight), 1e-4f) << "a graceful fade-out stop must end at digital silence on R";

    ASSERT_TRUE(backend.stop(error));
}