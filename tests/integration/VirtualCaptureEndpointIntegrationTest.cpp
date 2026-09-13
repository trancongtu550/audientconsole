#include "TestPassthroughPlugin.h"
#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioRoutingAdapter.h"
#include "transport/TransportFormat.h"
#include "transport/VirtualCaptureTransport.h"
#include "virtual_audio/endpoint/SoftwareCaptureEndpoint.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

// Slice Q2 — virtual microphone endpoint contract end-to-end. On the simulated
// ASIO driver the full path is: physical mic -> RoutingCore -> Adapter ->
// VirtualCaptureTransport -> SoftwareCaptureEndpoint (the contract the future
// Phase-8 WaveRT capture endpoint and app bridge must honor). Proves processed
// mic delivery, digital silence while the physical input is unavailable, and
// recovery — with no Windows/driver-specific code in routing/vst3/transport.

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

} // namespace

TEST(VirtualCaptureEndpointIntegration, ProcessedMicUplinkReachesTheVirtualMicEndpoint)
{
    // Input 0.25 -> mic VST3 chain gain 0.5 -> processed 0.125 -> transport ->
    // endpoint. Render branch muted so only the capture contract is observed.
    ChainBundle micChain(0.5f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    audient::transport::VirtualCaptureTransport transport(
        {48000, 1, 64, audient::transport::SampleType::Float32}, 65536);
    audient::virtual_audio::SoftwareCaptureEndpoint endpoint(transport);
    h.adapter.setCaptureTransport(&transport);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));

    // Pull until steady-state processed mic (0.125) arrives. The endpoint
    // always fills the full block; content is the signal that the path is live.
    std::vector<float> out(64, 0.0f);
    ASSERT_TRUE(waitUntil([&]() {
        endpoint.captureMono(out.data(), 64, 256);
        return out[0] >= 0.124f;
    }, std::chrono::milliseconds(3000)))
        << "the endpoint must deliver the processed mic (0.25 * 0.5 = 0.125)";
    EXPECT_NEAR(out[0], 0.125f, 1e-2f);
    EXPECT_GE(endpoint.counters().freshServedFrames, 64u) << "fresh pulls accumulate (fade + steady state)";

    // Physical input disappears: the adapter publishes digital silence, which
    // the endpoint must deliver to capture clients (never stale audio).
    SimulatedAsioDriver* driver = h.provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    driver->setInputsEnabled(false);
    ASSERT_TRUE(waitUntil([&]() {
        endpoint.captureMono(out.data(), 64, 256);
        return out[0] == 0.0f && out[63] == 0.0f;
    }, std::chrono::milliseconds(3000)))
        << "unavailable input must become digital silence at the endpoint (never stale)";

    // Input restored: processed mic returns to the endpoint.
    driver->setInputsEnabled(true);
    ASSERT_TRUE(waitUntil([&]() {
        endpoint.captureMono(out.data(), 64, 256);
        return out[0] >= 0.124f;
    }, std::chrono::milliseconds(3000)))
        << "re-enabled input must recover through the endpoint contract";
    EXPECT_NEAR(out[0], 0.125f, 1e-2f);

    ASSERT_TRUE(h.backend.stop(error));
    EXPECT_GT(endpoint.counters().freshServedFrames, 0u) << "fresh processed mic was served before the reset";

    // True engine-absence path: after a control-plane reset the transport is
    // empty and the endpoint serves EXACT digital silence, counted.
    endpoint.reset();
    std::fill(out.begin(), out.end(), -1.0f);
    EXPECT_EQ(endpoint.captureMono(out.data(), 64, 256), 64u);
    float peak = 0.0f;
    for (const float sample : out)
    {
        peak = std::max(peak, std::fabs(sample));
    }
    EXPECT_EQ(peak, 0.0f) << "absent engine => the endpoint serves digital silence";
    EXPECT_GE(endpoint.counters().silenceServedFrames, 64u);

    EXPECT_STREQ(endpoint.friendlyName(), "Microphone (Audient Console)") << "AGENTS §4 canonical endpoint name";
    EXPECT_EQ(endpoint.format().channels, 1);
}