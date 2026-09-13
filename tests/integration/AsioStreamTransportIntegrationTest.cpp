#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioStreamBridge.h"
#include "engine/GraphConfig.h"
#include "engine/EngineGraph.h"
#include "transport/TransportFormat.h"
#include "transport/TransportLinks.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace audient::asio;
namespace transport = audient::transport;
namespace engine = audient::engine;

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

} // namespace

TEST(AsioTransport, DownlinkAndUplinkRoundTripThroughEngine)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    transport::DownlinkTransport downlink({48000, 2, 64, transport::SampleType::Float32}, 4096);
    transport::UplinkTransport uplink({48000, 1, 64, transport::SampleType::Float32}, 4096);

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
    bridge.configureStreamFade(64);
    bridge.setDownlinkTransport(&downlink);
    bridge.setUplinkTransport(&uplink);
    bridge.requestFadeIn();

    std::atomic<bool> stopRender{false};
    std::thread renderProducer([&]() {
        std::vector<float> block(128);
        while (!stopRender.load(std::memory_order_acquire))
        {
            for (std::size_t i = 0; i < 64; ++i)
            {
                block[2 * i] = 0.3f;
                block[2 * i + 1] = -0.3f;
            }
            downlink.writeBlock(block.data(), 64);
        }
    });

    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state output is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> outLeft = driver->lastOutput(0);
    const std::vector<float> outRight = driver->lastOutput(1);
    ASSERT_FALSE(outLeft.empty());
    ASSERT_FALSE(outRight.empty());

    ASSERT_TRUE(backend.stop(error));
    stopRender.store(true, std::memory_order_release);
    renderProducer.join();

    EXPECT_NEAR(outLeft[0], 0.3f, 0.02f) << "render transport must feed the downlink and reach output L";
    EXPECT_NEAR(outRight[0], -0.3f, 0.02f) << "render transport must reach output R with correct stereo mapping";
    EXPECT_NE(outLeft[0], outRight[0]);

    std::vector<float> captured(64);
    std::uint64_t drainedFrames = 0;
    std::uint64_t steadyFrames = 0;
    double summed = 0.0;
    constexpr std::uint64_t kSkipFadeBlocks = 5;
    std::uint64_t blocks = 0;
    while (uplink.readBlockInterleaved(captured.data(), 64))
    {
        if (blocks >= kSkipFadeBlocks)
        {
            for (const float sample : captured)
            {
                summed += sample;
            }
            steadyFrames += 64;
        }
        ++blocks;
        drainedFrames += 64;
    }

    EXPECT_GT(drainedFrames, 0u) << "capture transport must carry at least one processed-mic block";
    EXPECT_GT(steadyFrames, 0u) << "capture must include steady-state blocks after the start fade-in";
    const double mean = summed / static_cast<double>(steadyFrames);
    EXPECT_NEAR(mean, 0.25, 1e-2) << "capture transport must carry the physical mic (0.25) at unity send";
}