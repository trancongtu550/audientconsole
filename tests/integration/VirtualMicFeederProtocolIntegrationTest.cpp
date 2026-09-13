#include "TestPassthroughPlugin.h"
#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioRoutingAdapter.h"
#include "transport/TransportFormat.h"
#include "transport/VirtualCaptureTransport.h"
#include "virtual_audio/driver-protocol/MockDriverCaptureSink.h"
#include "virtual_audio/driver-protocol/VirtualMicFeeder.h"
#include "virtual_audio/endpoint/SoftwareCaptureEndpoint.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

// Slice Q3 — driver transport protocol end-to-end on the simulated ASIO driver:
// physical mic -> RoutingCore -> adapter -> Q1 transport -> software endpoint ->
// VirtualMicFeeder (worker thread) -> MockDriverCaptureSink -> capture client.
// Proves normal flow, that a stalled/disconnected sink NEVER affects ASIO/VST
// (0 xruns), and that reconnect resumes with fresh audio (new generation).

namespace
{

using namespace audient::asio;

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

struct ChainBundle
{
    audient::vst3_test::TestPassthroughFactory factory;
    audient::vst3::Vst3Host host;
    std::unique_ptr<audient::vst3::Vst3Processor> processor;
    audient::vst3::Vst3Chain chain;

    ChainBundle(float gain, audient::vst3::BusLayout layout, std::size_t maxBlock)
        : factory(gain)
    {
        host.attachFactory(&factory);
        processor = host.createEffectProcessor(host.classes()[0], 48000.0, static_cast<long>(maxBlock), layout);
        chain.publish({{processor.get(), false}}, false);
    }
};

struct Harness
{
    SimulatedAsioProvider provider;
    AsioBackend backend;
    AsioRoutingAdapter adapter;

    explicit Harness(std::size_t maxBlockSamples)
        : backend(provider)
        , adapter(backend, maxBlockSamples)
    {
    }

    bool configure(std::string& error)
    {
        if (!backend.selectAudientDevice(error))
        {
            return false;
        }
        const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
        if (!plan.valid())
        {
            return false;
        }
        return backend.configure(48000, 64, plan, error);
    }
};

struct FeedHarness
{
    audient::transport::VirtualCaptureTransport transport;
    audient::virtual_audio::SoftwareCaptureEndpoint endpoint;
    audient::virtual_audio::MockDriverCaptureSink sink;
    audient::virtual_audio::VirtualMicFeeder feeder;

    FeedHarness(std::size_t sinkCapacity)
        : transport({48000, 1, 64, audient::transport::SampleType::Float32}, 65536)
        , endpoint(transport)
        , sink({48000, 1, 64, audient::transport::SampleType::Float32}, sinkCapacity)
        , feeder(endpoint, 64)
    {
    }
};

} // namespace

TEST(VirtualMicFeederProtocolIntegration, FeederStreamsProcessedMicToDriverSink)
{
    ChainBundle micChain(0.5f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    FeedHarness feed(8192);
    h.adapter.setCaptureTransport(&feed.transport);
    feed.feeder.setStaleThresholdFrames(256);
    h.adapter.attach();
    h.adapter.requestFadeIn();
    feed.feeder.attachSink(&feed.sink);
    feed.feeder.start(std::chrono::milliseconds(1));

    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));

    std::vector<float> out(64, 0.0f);
    ASSERT_TRUE(waitUntil([&]() {
        return feed.sink.readMono(out.data(), 64, 256) && out[0] >= 0.124f;
    }, std::chrono::milliseconds(3000)))
        << "the capture client must receive the processed mic (0.25 * 0.5 = 0.125)";
    EXPECT_NEAR(out[0], 0.125f, 1e-2f);
    EXPECT_GT(feed.feeder.snapshot().pushedBlocks, 0u);

    ASSERT_TRUE(h.backend.stop(error));
    feed.feeder.requestStop();
    feed.feeder.stop();

    EXPECT_EQ(h.backend.xrunCount(), 0u) << "the feeder/sink path must never disturb the ASIO callback";
    EXPECT_EQ(h.backend.overloadCount(), 0u);
    EXPECT_GT(feed.sink.snapshot().acceptedWrites, 0u);
}

TEST(VirtualMicFeederProtocolIntegration, SinkStallNeverAffectsAsioOrVst)
{
    ChainBundle micChain(0.5f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    // Tiny sink: it fills almost immediately while no client is reading.
    FeedHarness feed(128);
    h.adapter.setCaptureTransport(&feed.transport);
    feed.feeder.setStaleThresholdFrames(256);
    h.adapter.attach();
    h.adapter.requestFadeIn();
    feed.feeder.attachSink(&feed.sink);
    feed.feeder.start(std::chrono::milliseconds(1));

    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(h.backend.xrunCount(), 0u) << "a stall-saturated sink must not affect the ASIO callback";
    EXPECT_EQ(h.backend.overloadCount(), 0u);
    EXPECT_EQ(feed.sink.availableFrames(), 128u) << "the stalled sink stays bounded at capacity";
    EXPECT_GT(feed.feeder.snapshot().rejectedBlocks, 0u) << "sink rejects are counted, never silent";

    // A client finally reads: it recovers through the freshness policy.
    std::vector<float> out(64, 0.0f);
    ASSERT_TRUE(waitUntil([&]() {
        return feed.sink.readMono(out.data(), 64, 256) && out[0] >= 0.124f;
    }, std::chrono::milliseconds(3000)))
        << "the stalled client recovers via the freshness policy";

    ASSERT_TRUE(h.backend.stop(error));
    feed.feeder.requestStop();
    feed.feeder.stop();
    EXPECT_EQ(h.backend.xrunCount(), 0u);
}

TEST(VirtualMicFeederProtocolIntegration, DisconnectReconnectResumesWithFreshAudioOnly)
{
    // Gain 4.0 -> content 1.0 in phase A; the wire (bypassed chain) -> 0.25 in
    // phase B, so stale-vs-fresh is numerically distinguishable.
    ChainBundle micChain(4.0f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    FeedHarness feed(8192);
    h.adapter.setCaptureTransport(&feed.transport);
    feed.feeder.setStaleThresholdFrames(256);
    h.adapter.attach();
    h.adapter.requestFadeIn();
    feed.feeder.attachSink(&feed.sink);
    feed.feeder.start(std::chrono::milliseconds(1));

    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));

    // Phase A: client hears the processed mic at 1.0 (0.25 * gain 4).
    std::vector<float> out(64, 0.0f);
    ASSERT_TRUE(waitUntil([&]() {
        return feed.sink.readMono(out.data(), 64, 256) && out[0] >= 0.99f;
    }, std::chrono::milliseconds(3000)))
        << "phase A must reach 1.0";

    // Disconnect while the engine keeps producing: feeder rejects are counted.
    feed.sink.setConnected(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_GT(feed.feeder.snapshot().rejectedBlocks, 0u);
    EXPECT_GT(feed.sink.snapshot().rejectedDisconnected, 0u);

    // Switch to wire content (0.25) then reconnect as a fresh epoch.
    ASSERT_TRUE(micChain.chain.publish({{micChain.processor.get(), false}}, true));
    feed.sink.setConnected(true);
    feed.feeder.attachSink(&feed.sink);
    EXPECT_GE(feed.feeder.generation(), 2u);

    // Phase B: only post-reconnect 0.25 audio may reach the client — never 1.0.
    ASSERT_TRUE(waitUntil([&]() {
        return feed.sink.readMono(out.data(), 64, 256) && out[0] >= 0.249f && out[0] <= 0.251f;
    }, std::chrono::milliseconds(3000)));
    EXPECT_GE(feed.sink.snapshot().generationFlushes, 1u)
        << "reconnect must flush the old-epoch backlog";

    for (int i = 0; i < 5; ++i)
    {
        ASSERT_TRUE(waitUntil([&]() { return feed.sink.readMono(out.data(), 64, 256); },
                              std::chrono::milliseconds(2000)));
        EXPECT_LE(std::fabs(out[0] - 0.25f), 1e-2f) << "no pre-reconnect (1.0) audio after reconnect";
    }

    ASSERT_TRUE(h.backend.stop(error));
    feed.feeder.requestStop();
    feed.feeder.stop();
    EXPECT_EQ(h.backend.xrunCount(), 0u);
    EXPECT_EQ(h.backend.overloadCount(), 0u);
}