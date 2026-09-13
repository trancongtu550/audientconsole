#include "TestPassthroughPlugin.h"
#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioRoutingAdapter.h"
#include "transport/TransportFormat.h"
#include "transport/TransportLinks.h"
#include "transport/VirtualCaptureTransport.h"
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

// Slice Q1 — virtual capture transport. Proves the processed mic uplink leaves
// the ASIO <-> RoutingCore adapter through transport::VirtualCaptureTransport
// (processed mic content, digital silence while the physical input is
// unavailable, no downlink leakage, and a fresh capture epoch after restart)
// on the simulated driver and via direct realtime calls. No Windows
// virtual-device code and no Discord code appear anywhere in the data path
// being tested.

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

// Direct realtime drive of one 64-frame block with the given mono input.
void driveBlock(audient::asio::AsioRoutingAdapter& adapter, const float* input, std::size_t frames,
                std::vector<float>& outL, std::vector<float>& outR)
{
    float* outputs[2] = {outL.data(), outR.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(frames);
    info.inputChannels = input != nullptr ? 1 : 0;
    info.inputs = input != nullptr ? &input : nullptr;
    info.outputChannels = 2;
    info.outputs = outputs;
    adapter.processAsio(info);
}

// Drained per-block mean over `frames`-sample blocks, skipping the first
// `skipBlocks` (the start fade). Returns the number of steady blocks summed.
std::size_t drainSteadyMean(audient::transport::VirtualCaptureTransport& capture, std::size_t frames,
                            std::size_t skipBlocks, double& meanOut)
{
    std::vector<float> block(frames);
    double sum = 0.0;
    std::size_t steady = 0;
    std::size_t index = 0;
    while (capture.readProcessedMic(block.data(), frames))
    {
        if (index++ >= skipBlocks)
        {
            for (const float sample : block)
            {
                sum += sample;
            }
            steady += frames;
        }
    }
    meanOut = steady > 0 ? sum / static_cast<double>(steady) : 0.0;
    return steady;
}

} // namespace

TEST(VirtualCaptureTransportIntegration, ProcessedMicCrossesTheCaptureTransport)
{
    // Simulated input 0.25 -> mic chain gain 0.5 -> micProcessed 0.125 ->
    // capture transport. Render branch muted so only the capture path is live.
    ChainBundle micChain(0.5f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    audient::transport::VirtualCaptureTransport capture(
        {48000, 1, 64, audient::transport::SampleType::Float32}, 65536);
    h.adapter.setCaptureTransport(&capture);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state capture is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ASSERT_TRUE(h.backend.stop(error));

    // The fade is maxBlock*3 = 768 samples = 12 blocks of 64; skip 14 blocks.
    double mean = 0.0;
    const std::size_t steady = drainSteadyMean(capture, 64, 14, mean);
    EXPECT_GT(steady, 0u);
    EXPECT_NEAR(mean, 0.125, 1e-2) << "the capture transport must carry the processed mic (0.25 * 0.5)";
    EXPECT_GT(capture.producerSequence(), 0u);
}

TEST(VirtualCaptureTransportIntegration, CaptureGoesSilentWhenInputUnavailableAndRecovers)
{
    // Deterministic direct-realtime drive (the same adapter callback the
    // streaming path uses). Gain 2.0 so the uplink is distinguishable: input
    // present -> 0.5, input absent -> exact digital silence, input restored ->
    // 0.5 again. Draining preserves the write order.
    ChainBundle micChain(2.0f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    audient::transport::VirtualCaptureTransport capture(
        {48000, 1, 64, audient::transport::SampleType::Float32}, 65536);
    h.adapter.setCaptureTransport(&capture);

    h.adapter.attach();
    h.adapter.requestFadeIn();

    const std::vector<float> input(64, 0.25f);
    std::vector<float> outL(64, 0.0f);
    std::vector<float> outR(64, 0.0f);

    constexpr int kActiveBlocks = 120;  // >= fade (12) + safety, all at 0.5
    constexpr int kSilentBlocks = 80;   // input unavailable: exact silence
    constexpr int kRecoverBlocks = 80;  // input restored: 0.5 again

    for (int i = 0; i < kActiveBlocks; ++i)
    {
        driveBlock(h.adapter, input.data(), 64, outL, outR);
    }
    for (int i = 0; i < kSilentBlocks; ++i)
    {
        driveBlock(h.adapter, nullptr, 64, outL, outR);
    }
    for (int i = 0; i < kRecoverBlocks; ++i)
    {
        driveBlock(h.adapter, input.data(), 64, outL, outR);
    }

    std::vector<float> block(64);
    std::vector<double> postFadeMeans;
    std::size_t index = 0;
    while (capture.readProcessedMic(block.data(), 64))
    {
        double m = 0.0;
        for (const float sample : block)
        {
            m += sample;
        }
        m /= 64.0;
        if (index++ >= 14) // skip the start fade (maxBlock*3 = 12 blocks)
        {
            postFadeMeans.push_back(m);
        }
    }

    bool sawActive = false;
    bool sawSilent = false;
    bool sawSilentAfterActive = false;
    for (const double m : postFadeMeans)
    {
        if (m >= 0.49)
        {
            sawActive = true;
        }
        else if (m <= 1e-4)
        {
            sawSilent = true;
            if (sawActive)
            {
                sawSilentAfterActive = true;
            }
        }
    }
    EXPECT_TRUE(sawActive) << "capture must carry the active mic while the input is present";
    EXPECT_TRUE(sawSilent) << "capture must emit silence while the input is unavailable (no stale replay)";
    EXPECT_TRUE(sawSilentAfterActive) << "silence must appear between active windows (the disabled phase in order)";
    EXPECT_GT(postFadeMeans.size(), 0u);
    EXPECT_GE(postFadeMeans.back(), 0.49) << "the final capture window must be the recovered active mic";
}

TEST(VirtualCaptureTransportIntegration, DownlinkNeverEntersCaptureTransport)
{
    // Loud stereo downlink (0.7 / -0.7) feeding Output 1/2 while the (silent-
    // downlink) capture path carries only the 0.25 physical input. The capture
    // transport must never see the render content.
    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = false;
    cfg.physicalOutputGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);

    audient::transport::DownlinkTransport downlink(
        {48000, 2, 64, audient::transport::SampleType::Float32}, 65536);
    audient::transport::VirtualCaptureTransport capture(
        {48000, 1, 64, audient::transport::SampleType::Float32}, 65536);
    h.adapter.setDownlinkTransport(&downlink);
    h.adapter.setCaptureTransport(&capture);

    h.adapter.attach();
    h.adapter.requestFadeIn();

    std::vector<float> block(128);
    for (std::size_t i = 0; i < 64; ++i)
    {
        block[2 * i] = 0.7f;
        block[2 * i + 1] = -0.7f;
    }
    std::atomic<bool> stopRender{false};
    std::thread renderProducer([&]() {
        while (!stopRender.load(std::memory_order_acquire))
        {
            if (!downlink.writeBlock(block.data(), 64))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ASSERT_TRUE(h.backend.stop(error));

    stopRender.store(true, std::memory_order_release);
    renderProducer.join();

    std::vector<float> captureBlock(64);
    std::size_t steady = 0;
    std::size_t index = 0;
    double sum = 0.0;
    double maxAbsDeviation = 0.0;
    while (capture.readProcessedMic(captureBlock.data(), 64))
    {
        if (index++ >= 14)
        {
            for (const float sample : captureBlock)
            {
                sum += sample;
                maxAbsDeviation = std::max(maxAbsDeviation, std::fabs(sample - 0.25));
            }
            steady += 64;
        }
    }
    EXPECT_GT(steady, 0u);
    const double mean = sum / static_cast<double>(steady);
    EXPECT_NEAR(mean, 0.25, 1e-2) << "capture carries only the physical mic, not the render mix";
    EXPECT_LE(maxAbsDeviation, 0.02) << "no 0.7 downlink energy may leak into the capture transport";
}

TEST(VirtualCaptureTransportIntegration, RestartYieldsFreshCaptureEpoch)
{
    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);

    audient::transport::VirtualCaptureTransport capture(
        {48000, 1, 64, audient::transport::SampleType::Float32}, 65536);
    h.adapter.setCaptureTransport(&capture);

    h.adapter.attach();

    const std::vector<float> input(64, 0.25f);
    std::vector<float> outL(64, 0.0f);
    std::vector<float> outR(64, 0.0f);

    // First "run": capture fills with 0.25 blocks (wire path).
    h.adapter.requestFadeIn();
    for (int i = 0; i < 200; ++i)
    {
        driveBlock(h.adapter, input.data(), 64, outL, outR);
    }
    EXPECT_GT(capture.availableFrames(), 0u) << "the first run must leave captured data buffered";

    // A new fade-in is a fresh capture epoch: all buffered data is cleared so
    // the second run can never serve pre-restart mic audio.
    h.adapter.requestFadeIn();
    EXPECT_EQ(capture.availableFrames(), 0u) << "restart must not replay stale pre-restart capture";
    EXPECT_EQ(capture.producerSequence(), 0u);

    // Second "run": fresh data flows again and is fully drained.
    for (int i = 0; i < 200; ++i)
    {
        driveBlock(h.adapter, input.data(), 64, outL, outR);
    }

    double mean = 0.0;
    const std::size_t steady = drainSteadyMean(capture, 64, 14, mean);
    EXPECT_GT(steady, 0u);
    EXPECT_NEAR(mean, 0.25, 1e-2) << "the second run must carry fresh processed mic audio";
    EXPECT_EQ(capture.availableFrames(), 0u);
}

TEST(VirtualCaptureTransportIntegration, FreshCaptureConsumerResynchronizesAfterStall)
{
    // Live-mic freshness: the consumer stalls (never reads) while the producer
    // keeps writing 1.0 uplink blocks -> the ring overflows and holds a stale
    // backlog. On recovery the freshness read must flush the stale backlog
    // (staleCatchupDrops), the producer's rejects must be counted
    // (overflowDrops), and only post-recovery 0.25 audio may be served.
    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);

    audient::transport::VirtualCaptureTransport capture(
        {48000, 1, 64, audient::transport::SampleType::Float32}, 65536); // 1024 blocks
    h.adapter.setCaptureTransport(&capture);

    h.adapter.attach();
    h.adapter.requestFadeIn();

    const std::vector<float> stallInput(64, 1.0f);
    const std::vector<float> freshInput(64, 0.25f);
    std::vector<float> outL(64, 0.0f);
    std::vector<float> outR(64, 0.0f);

    // Stall: 2000 blocks (128000 frames) attempted against 65536 capacity.
    for (int i = 0; i < 2000; ++i)
    {
        driveBlock(h.adapter, stallInput.data(), 64, outL, outR);
    }

    const auto afterStall = capture.stats();
    EXPECT_EQ(afterStall.overflowDrops, 128000u - 65536u) << "overflow rejects are counted, never silent";
    EXPECT_EQ(capture.availableFrames(), 65536u) << "the ring holds a full stale backlog while stalled";

    // Recovery: the first freshness read discards the entire stale backlog and
    // serves nothing.
    std::vector<float> read(64, 0.0f);
    EXPECT_FALSE(capture.readProcessedMicFresh(read.data(), 64, 256))
        << "the stale backlog must never be served after recovery";
    EXPECT_EQ(capture.stats().staleCatchupDrops, 65536u) << "consumer side catch-up discards are counted";
    EXPECT_EQ(capture.availableFrames(), 0u);

    // Post-recovery audio: written after the resync, read interleaved so the
    // backlog never exceeds the freshness threshold. Only 0.25 may be served.
    double maxServed = 0.0;
    int served = 0;
    for (int i = 0; i < 50; ++i)
    {
        driveBlock(h.adapter, freshInput.data(), 64, outL, outR);
        ASSERT_TRUE(capture.readProcessedMicFresh(read.data(), 64, 256));
        for (const float sample : read)
        {
            maxServed = std::max(maxServed, static_cast<double>(sample));
        }
        ++served;
    }
    EXPECT_EQ(served, 50);
    EXPECT_LE(maxServed, 0.251) << "the 1.0 stall-era audio must never reach the consumer";
    EXPECT_EQ(capture.availableFrames(), 0u);
    EXPECT_GT(capture.stats().overflowDrops, 0u);
    EXPECT_GT(capture.stats().staleCatchupDrops, 0u);
}