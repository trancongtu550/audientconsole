#include "TestPassthroughPlugin.h"
#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioRoutingAdapter.h"
#include "core/AllocationTracker.h"
#include "engine/EngineGraph.h"
#include "engine/SyntheticDownlink.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"
#include "transport/TransportLinks.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && !defined(NDEBUG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Slice P — ASIO <-> RoutingCore adapter. Proves the realtime-safe mapping of
// physical ASIO buffers onto logical bus IDs (input[0] -> PhysicalInput1,
// output[0]/output[1] -> Physical Output 1/2), the processed-mic monitor
// render branch, chain processing, silence/drop on unavailable input, no stale
// replay, oversized-block safety, bypass, repeated start/stop, and a zero
// allocation callback body.

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

float peakMagnitude(const std::vector<float>& data)
{
    float peak = 0.0f;
    for (const float sample : data)
    {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

float dBToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

// Runs 250 direct 64-sample callbacks into a 4-channel (two stereo pair) output
// buffer with a constant mono physical mic input (micLevel) so the Monitor
// (pair 0 = out[0]/out[1]) and Headphone (pair 1 = out[2]/out[3]) render buses
// reach steady state (fade + gain ramps fully settled).
struct DirectRun
{
    std::vector<float> out[4];
    float peak[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // peak magnitude across the whole run

    void run(AsioRoutingAdapter& adapter, float micLevel)
    {
        constexpr std::size_t kFrames = 64;
        for (auto& channel : out)
        {
            channel.assign(kFrames, 0.0f);
        }
        peak[0] = peak[1] = peak[2] = peak[3] = 0.0f;
        std::vector<float> input(kFrames, micLevel);
        const float* inputPtrs[1] = {input.data()};
        float* outputPtrs[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
        AsioCallbackInfo info{};
        info.sampleCount = static_cast<long>(kFrames);
        info.inputChannels = 1;
        info.inputs = inputPtrs;
        info.outputChannels = 4;
        info.outputs = outputPtrs;
        adapter.attach();
        adapter.requestFadeIn();
        for (int i = 0; i < 250; ++i)
        {
            adapter.processAsio(info);
            for (int channel = 0; channel < 4; ++channel)
            {
                const float blockPeak = peakMagnitude(out[static_cast<std::size_t>(channel)]);
                peak[channel] = std::max(peak[channel], blockPeak);
            }
        }
    }
};

// Owning bundle for one prepared VST3 chain slot (factory + host + processor +
// chain). Members are destroyed in reverse order (chain, processor, host,
// factory), matching the existing host lifecycle tests.
struct ChainBundle
{
    audient::vst3_test::TestPassthroughFactory factory;
    audient::vst3::Vst3Host host;
    std::unique_ptr<audient::vst3::Vst3Processor> processor;
    audient::vst3::Vst3Chain chain;

    ChainBundle(float gain, audient::vst3::BusLayout layout, std::size_t maxBlock, bool wholeBypass = false)
        : factory(gain)
    {
        host.attachFactory(&factory);
        processor = host.createEffectProcessor(host.classes()[0], 48000.0, static_cast<long>(maxBlock), layout);
        chain.publish({{processor.get(), false}}, wholeBypass);
    }
};

// Booted backend + adapter on the simulated driver. The caller requests
// fade-in and start. maxBlockSamples may be smaller than the driver buffer to
// exercise the oversized-block path.
struct Harness
{
    SimulatedAsioProvider provider;
    AsioBackend backend;
    AsioRoutingAdapter adapter;

    explicit Harness(std::size_t maxBlockSamples, int outputChannels = 2)
        : backend(provider)
        , adapter(backend, maxBlockSamples)
        , m_outputChannels(outputChannels)
    {
    }

    bool configure(std::string& error)
    {
        provider.setOutputChannels(m_outputChannels);
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

private:
    int m_outputChannels = 2;
};

void identityMono(const float* in, float* out, std::size_t frames, void*)
{
    if (in != nullptr && out != nullptr)
    {
        std::memcpy(out, in, frames * sizeof(float));
    }
}

void identityStereo(const float* leftIn, const float* rightIn, float* leftOut, float* rightOut, std::size_t frames, void*)
{
    if (leftIn != nullptr && leftOut != nullptr)
    {
        std::memcpy(leftOut, leftIn, frames * sizeof(float));
    }
    if (rightIn != nullptr && rightOut != nullptr)
    {
        std::memcpy(rightOut, rightIn, frames * sizeof(float));
    }
}

#if defined(_MSC_VER) && !defined(NDEBUG)
std::uintptr_t currentThreadId()
{
    return static_cast<std::uintptr_t>(::GetCurrentThreadId());
}
#else
std::uintptr_t currentThreadId()
{
    return 0;
}
#endif

} // namespace

TEST(AsioRoutingAdapter, PhysicalInputMapsToMicRawBitIdentically)
{
    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true; // isolate the mic path from the render branch
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before the uplink staging is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ASSERT_TRUE(h.backend.stop(error));

    const std::vector<float>& raw = h.adapter.micRawBuffer();
    ASSERT_FALSE(raw.empty());
    EXPECT_EQ(raw[0], 0.25f) << "the simulated physical input (0.25f) must equal the micRaw tap bit-for-bit";
    EXPECT_EQ(h.adapter.micUplinkBuffer()[0], 0.25f) << "with no mic chain the wire passes input to the uplink";
}

TEST(AsioRoutingAdapter, MicProcessedPassesThroughInputChain)
{
    // Simulated input 0.25 -> mic VST3 chain gain 0.5 -> micProcessed 0.125.
    ChainBundle micChain(0.5f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before the mic staging is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ASSERT_TRUE(h.backend.stop(error));

    const std::vector<float>& raw = h.adapter.micRawBuffer();
    const std::vector<float>& processed = h.adapter.micProcessedBuffer();
    const std::vector<float>& uplink = h.adapter.micUplinkBuffer();
    const std::vector<float>& monitor = h.adapter.monitorScratchBuffer();
    ASSERT_FALSE(raw.empty());
    EXPECT_NEAR(raw[0], 0.25f, 1e-6f) << "micRaw stays the pre-chain tap";
    EXPECT_NEAR(processed[0], 0.25f * 0.5f, 1e-5f) << "micProcessed must run the input VST3 chain";
    EXPECT_NEAR(uplink[0], 0.25f * 0.5f, 1e-5f) << "micUplink carries the processed mic";
    EXPECT_NEAR(monitor[0], 0.25f * 0.5f, 1e-5f) << "processedMicMonitor taps the processed mic";
}

TEST(AsioRoutingAdapter, PlaybackLeftRightPreservedThroughOutputChain)
{
    // Drives the realtime entry DIRECTLY with a known downlink through the real
    // DownlinkTransport path (not the diagnostic-only synthetic signal), so the
    // left/right mapping contract is verified in every build configuration.
    constexpr std::size_t kFrames = 64;
    constexpr float kLeft = 0.3f;
    constexpr float kRight = -0.7f;

    std::string error;
    Harness h(256);

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true; // isolate: physical output = processed downlink only
    cfg.downlinkMute = false;
    cfg.downlinkGain = 1.0f;
    cfg.physicalOutputGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);

    audient::transport::DownlinkTransport downlink(
        audient::transport::Format{48000, 2, static_cast<int>(kFrames), audient::transport::SampleType::Float32}, 4096);
    std::vector<float> block(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        block[2 * i] = kLeft;
        block[2 * i + 1] = kRight;
    }
    h.adapter.setDownlinkTransport(&downlink);
    h.adapter.attach();
    h.adapter.requestFadeIn();

    std::vector<float> outL(kFrames, 0.0f);
    std::vector<float> outR(kFrames, 0.0f);
    float* outputPtrs[2] = {outL.data(), outR.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 0; // no physical input this block
    info.inputs = nullptr;
    info.outputChannels = 2;
    info.outputs = outputPtrs;

    // Keep the transport fed one block per callback; run well past the fade so
    // the steady-state block is unscaled.
    for (int i = 0; i < 200; ++i)
    {
        downlink.writeBlock(block.data(), kFrames);
        h.adapter.processAsio(info);
    }
    ASSERT_TRUE(h.adapter.fadeIsFull()) << "a direct steady-state run must complete the fade-in";

    EXPECT_NEAR(outL[0], kLeft, 1e-4f) << "PlaybackBus L must reach physical output 1 preserved";
    EXPECT_NEAR(outR[0], kRight, 1e-4f) << "PlaybackBus R must reach physical output 2 preserved";
    EXPECT_NEAR(h.adapter.outputProcessedLeftBuffer()[0], kLeft, 1e-4f) << "Output Processed L = downlink";
    EXPECT_NEAR(h.adapter.outputProcessedRightBuffer()[0], kRight, 1e-4f) << "Output Processed R = downlink";

    double differ = 0.0;
    for (std::size_t i = 0; i < outL.size(); ++i)
    {
        const double delta = static_cast<double>(outL[i]) - static_cast<double>(outR[i]);
        differ += delta * delta;
    }
    EXPECT_GT(differ, 1e-3) << "left/right downlink content must remain distinct (mapping preserved)";
}

TEST(AsioRoutingAdapter, OutputChainProcessedToPhysicalOutputs)
{
    // Downlink 0.35 L / -0.35 R through the output VST3 chain (gain 2.0) ->
    // 0.7 / -0.7 at the physical outputs. Direct realtime entry + transport so
    // it is valid in every build configuration.
    ChainBundle outChain(2.0f, audient::vst3::BusLayout::Stereo, 256);

    constexpr std::size_t kFrames = 64;
    constexpr float kLeft = 0.35f;
    constexpr float kRight = -0.35f;

    std::string error;
    Harness h(256);

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = false;
    cfg.physicalOutputGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setOutputChain(&audient::vst3::Vst3Chain::processStereo, &outChain.chain);

    audient::transport::DownlinkTransport downlink(
        audient::transport::Format{48000, 2, static_cast<int>(kFrames), audient::transport::SampleType::Float32}, 4096);
    std::vector<float> block(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        block[2 * i] = kLeft;
        block[2 * i + 1] = kRight;
    }
    h.adapter.setDownlinkTransport(&downlink);
    h.adapter.attach();
    h.adapter.requestFadeIn();

    std::vector<float> outL(kFrames, 0.0f);
    std::vector<float> outR(kFrames, 0.0f);
    float* outputPtrs[2] = {outL.data(), outR.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 0;
    info.inputs = nullptr;
    info.outputChannels = 2;
    info.outputs = outputPtrs;

    for (int i = 0; i < 200; ++i)
    {
        downlink.writeBlock(block.data(), kFrames);
        h.adapter.processAsio(info);
    }
    ASSERT_TRUE(h.adapter.fadeIsFull());

    EXPECT_NEAR(outL[0], kLeft * 2.0f, 1e-3f) << "the output VST3 chain (gain 2) must reach physical output 1";
    EXPECT_NEAR(outR[0], kRight * 2.0f, 1e-3f) << "the output VST3 chain (gain 2) must reach physical output 2";

    // Whole-chain bypass of the OUTPUT chain: the wire path (downlink as-is)
    // must be restored at the physical outputs (no stale processed audio).
    ASSERT_TRUE(outChain.chain.publish({{outChain.processor.get(), false}}, true));
    for (int i = 0; i < 200; ++i)
    {
        downlink.writeBlock(block.data(), kFrames);
        h.adapter.processAsio(info);
    }
    EXPECT_NEAR(outL[0], kLeft, 1e-3f) << "output chain bypass must restore the wire path";
    EXPECT_NEAR(outR[0], kRight, 1e-3f);
}

TEST(AsioRoutingAdapter, NullUnavailableInputYieldsSilenceAndRecovers)
{
    // Gain 2.0 mic chain would amplify stale input (0.5) if the adapter
    // replayed it when the input disappears mid-stream.
    ChainBundle micChain(2.0f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.micUplinkPeak() > 0.4f; }, std::chrono::milliseconds(3000)))
        << "with the input enabled the uplink must carry 0.25 * 2 = 0.5";

    SimulatedAsioDriver* driver = h.provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);

    // Input becomes unavailable mid-stream: silence, not stale 0.5.
    driver->setInputsEnabled(false);
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.micUplinkPeak() <= 1e-4f; }, std::chrono::milliseconds(3000)))
        << "unavailable input must drop to silence and never replay the previous block";

    const std::vector<float>& raw = h.adapter.micRawBuffer();
    const std::vector<float>& processed = h.adapter.micProcessedBuffer();
    const std::vector<float>& monitor = h.adapter.monitorScratchBuffer();
    EXPECT_LE(peakMagnitude(raw), 1e-6f) << "micRaw must not replay stale samples";
    EXPECT_LE(peakMagnitude(processed), 1e-6f) << "micProcessed must not replay stale samples";
    EXPECT_LE(peakMagnitude(monitor), 1e-6f) << "a stale processed-mic monitor must never leak into the render branch";

    // Input available again: the path recovers.
    driver->setInputsEnabled(true);
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.micUplinkPeak() > 0.4f; }, std::chrono::milliseconds(3000)))
        << "re-enabled input must route again";
    ASSERT_TRUE(h.backend.stop(error));
}

TEST(AsioRoutingAdapter, MonitorBranchMixesProcessedMicIntoPhysicalOutput)
{
    // Mic chain gain 0.5, monitor unity, downlink muted: physical output must
    // carry the PROCESSED mic (0.25 * 0.5 = 0.125) — the manual hardware test
    // path (mic -> EQ/Comp -> monitor output).
    ChainBundle micChain(0.5f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorGain = dBToLinear(0.0f);
    cfg.monitorMute = false;
    cfg.downlinkMute = true;
    cfg.physicalOutputGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state output is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    SimulatedAsioDriver* driver = h.provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> outLeft = driver->lastOutput(0);
    const std::vector<float> outRight = driver->lastOutput(1);
    ASSERT_FALSE(outLeft.empty());
    ASSERT_FALSE(outRight.empty());

    ASSERT_TRUE(h.backend.stop(error));

    EXPECT_NEAR(peakMagnitude(outLeft), 0.125f, 0.02f) << "processed mic monitor must reach Output 1";
    EXPECT_NEAR(peakMagnitude(outRight), 0.125f, 0.02f) << "processed mic monitor must reach Output 2";
}

TEST(AsioRoutingAdapter, OversizedCallbackIsIgnoredAndOutputsSilence)
{
    // Driver buffer is 64, but the adapter graph is sized 32: an oversized
    // block must be ignored safely and the physical outputs zeroed.
    std::string error;
    Harness h(32);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = false;
    h.adapter.publishRenderConfig(cfg);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    SimulatedAsioDriver* driver = h.provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> outLeft = driver->lastOutput(0);
    const std::vector<float> outRight = driver->lastOutput(1);
    ASSERT_FALSE(outLeft.empty());
    ASSERT_FALSE(outRight.empty());

    ASSERT_TRUE(h.backend.stop(error));

    EXPECT_LE(peakMagnitude(outLeft), 1e-6f) << "oversized blocks must never reach the physical outputs";
    EXPECT_LE(peakMagnitude(outRight), 1e-6f);
}

TEST(AsioRoutingAdapter, WholeChainBypassRemovesProcessingFromSignal)
{
    ChainBundle micChain(2.0f, audient::vst3::BusLayout::Mono, 256);

    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain.chain);

    h.adapter.attach();
    h.adapter.requestFadeIn();
    ASSERT_TRUE(h.backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.micUplinkPeak() > 0.4f; }, std::chrono::milliseconds(3000)))
        << "with processing active the uplink must carry 0.25 * 2 = 0.5";

    // Whole-chain bypass (control thread; safe snapshot swap, AGENTS §9).
    ASSERT_TRUE(micChain.chain.publish({{micChain.processor.get(), false}}, true));
    ASSERT_TRUE(waitUntil([&]() {
        return h.adapter.micUplinkPeak() > 0.1f && h.adapter.micUplinkPeak() < 0.3f;
    }, std::chrono::milliseconds(3000)))
        << "whole-chain bypass must restore the wire path (0.25, not 0.5)";
    ASSERT_TRUE(micChain.chain.publish({{micChain.processor.get(), false}}, false));
    ASSERT_TRUE(waitUntil([&]() { return h.adapter.micUplinkPeak() > 0.4f; }, std::chrono::milliseconds(3000)))
        << "bypass must be reversible";

    ASSERT_TRUE(h.backend.stop(error));
}

TEST(AsioRoutingAdapter, RepeatedStartStopIsClean)
{
    // Run without setInputsEnabled default so input stays present; physical
    // output must be valid after each restart.
    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = false;
    h.adapter.publishRenderConfig(cfg);

    audient::engine::SyntheticDownlink synthetic;
    synthetic.configure(440.0, 880.0, -20.0, 48000);
    h.adapter.enableSyntheticDownlink(&synthetic);

    h.adapter.attach();

    for (int run = 0; run < 2; ++run)
    {
        h.adapter.requestFadeIn();
        ASSERT_TRUE(h.backend.start(error)) << error;
        ASSERT_TRUE(waitUntil([&]() { return h.backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
        ASSERT_TRUE(waitUntil([&]() { return h.adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)))
            << "run " << run << ": the stream must reach full fade";
        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        SimulatedAsioDriver* driver = h.provider.lastOpenedDriver();
        ASSERT_NE(driver, nullptr);
        const std::vector<float> outLeft = driver->lastOutput(0);
        ASSERT_FALSE(outLeft.empty());
#if !defined(NDEBUG)
        // SyntheticDownlink is diagnostic-only: it emits the -20 dBFS tone in
        // non-NDEBUG builds and digital silence under NDEBUG. Content is pinned
        // in debug configs; the start/stop cleanliness is verified in all.
        EXPECT_NEAR(peakMagnitude(outLeft), 0.1f, 0.02f) << "run " << run << ": output must be valid after restart";
#endif

        ASSERT_TRUE(h.backend.stop(error)) << error;
    }
    SimulatedAsioDriver* driver = h.provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    EXPECT_EQ(driver->startedCount(), 2u) << "the simulated driver must be started exactly twice, cleanly";
}

TEST(AsioRoutingAdapter, MeteredCallbackBodyAllocatesZeroBytes)
{
    const bool supported = audient::core::isAllocationTrackerEnabled() ||
                           audient::core::installAllocationTracker() == audient::core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker is active only when NDEBUG is unset";
    }

    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    std::vector<ChannelInfo> planInputs;
    std::vector<ChannelInfo> planOutputs;
    for (const ChannelInfo& channel : backend.driverCapabilities().channels)
    {
        (channel.isInput ? planInputs : planOutputs).push_back(channel);
    }
    const ChannelPlan plan =
        AsioChannelMap::planProductionInputsAndStereoDownlink(planInputs, planOutputs, false);
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;
    adapter.attach();
    adapter.requestFadeIn();

    constexpr std::size_t kFrames = 64;
    std::vector<float> ch0(kFrames, 0.1f);
    std::vector<float> ch1(kFrames, -0.2f);
    const float* meterInputs[2] = {ch0.data(), ch1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = meterInputs;
    const std::uint64_t baseline = audient::core::allocationCountOnRealtimeThread();
    audient::core::markThreadRealtime(currentThreadId());
    for (int i = 0; i < 20000; ++i)
    {
        adapter.processAsio(info);
    }
    audient::core::clearRealtimeMark();
    EXPECT_EQ(audient::core::allocationCountOnRealtimeThread(), baseline);
}

TEST(AsioRoutingAdapter, CallbackBodyAllocatesZeroBytes)
{
    const bool supported = audient::core::isAllocationTrackerEnabled() ||
                           audient::core::installAllocationTracker() == audient::core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker is active only when NDEBUG is unset (Debug/RelWithDebInfo)";
    }

    std::string error;
    Harness h(256);

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorGain = 1.0f;
    cfg.downlinkGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);
    // Plain identity chains focus the check on the ADAPTER path (the VST3 chain
    // allocation-freedom is separately pinned by the chain/VST tests).
    h.adapter.setMicChain(&identityMono, nullptr);
    h.adapter.setOutputChain(&identityStereo, nullptr);
    h.adapter.attach();
    h.adapter.requestFadeIn();

    constexpr std::size_t kFrames = 256;
    std::vector<float> input(kFrames, 0.1f);
    std::vector<float> outL(kFrames, 0.0f);
    std::vector<float> outR(kFrames, 0.0f);
    const float* inputPtrs[1] = {input.data()};
    float* outputPtrs[2] = {outL.data(), outR.data()};

    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 1;
    info.outputChannels = 2;
    info.inputs = inputPtrs;
    info.outputs = outputPtrs;

    audient::engine::SyntheticDownlink synthetic;
    synthetic.configure(440.0, 880.0, -20.0, 48000);
    h.adapter.enableSyntheticDownlink(&synthetic);

    const std::uint64_t baseline = audient::core::allocationCountOnRealtimeThread();
    audient::core::markThreadRealtime(currentThreadId());
    for (int i = 0; i < 20000; ++i)
    {
        h.adapter.processAsio(info);
    }
    audient::core::clearRealtimeMark();
    const std::uint64_t grown = audient::core::allocationCountOnRealtimeThread();

    EXPECT_EQ(grown, baseline) << "the ASIO->RoutingCore adapter callback must not allocate on the realtime thread";
}

TEST(AsioRoutingAdapter, MicInputProbeSeamReplacesOneBlockAndStampsQpc)
{
    std::string error;
    Harness h(256);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true; // isolate the mic path
    cfg.downlinkMute = true;
    h.adapter.publishRenderConfig(cfg);

    h.adapter.attach();
    h.adapter.requestFadeIn();

    constexpr std::size_t kFrames = 64;
    std::vector<float> input(kFrames, 0.25f);
    const float* inputPtrs[1] = {input.data()};

    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 1;
    info.inputs = inputPtrs;
    info.outputChannels = 0;
    info.outputs = nullptr;

    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    ASSERT_TRUE(h.adapter.fadeIsFull());

    EXPECT_EQ(h.adapter.micInputProbeQpc(), 0ull)
        << "no QPC stamp before any probe is armed and consumed";

    // Arm a one-shot burst: the next callback must route the burst instead of
    // the physical input and stamp the injection QPC.
    std::vector<float> burst(kFrames, 0.0f);
    burst[0] = 0.9f;
    h.adapter.armMicInputProbe(burst.data(), burst.size());
    h.adapter.processAsio(info);
    EXPECT_NE(h.adapter.micInputProbeQpc(), 0ull) << "the probe block must stamp the injection QPC";
    EXPECT_NEAR(h.adapter.micRawBuffer()[0], 0.9f, 1e-6f)
        << "the armed probe must replace the physical input block before routing";

    // One-shot only: the very next callback reverts to real input.
    h.adapter.processAsio(info);
    EXPECT_NEAR(h.adapter.micRawBuffer()[0], 0.25f, 1e-6f)
        << "the probe burst must not replay on a later block";

    // A size mismatch (not the current block size) is ignored, not applied.
    std::vector<float> wrongSize(256, 0.0f);
    wrongSize[0] = 0.5f;
    h.adapter.armMicInputProbe(wrongSize.data(), wrongSize.size());
    h.adapter.processAsio(info);
    EXPECT_NEAR(h.adapter.micRawBuffer()[0], 0.25f, 1e-6f)
        << "a probe armed with a mismatched block size must be ignored";
}

TEST(AsioRoutingAdapter, HeadphoneBusRoutesMicIndependentlyOfMonitorAndKeepsVirtualMicConstant)
{
    // 4 simulated outputs: pair 0 = Monitor (Output 1/2), pair 1 = Headphones
    // (Output 3/4). Constant mic 0.25 feeds both buses when routed.
    std::string error;
    Harness h(256, 4);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;          // no processed mic into the MONITOR bus
    cfg.downlinkMute = true;
    cfg.physicalOutputMute = true;   // Monitor bus muted (isolate Monitor)
    cfg.headphoneMute = false;       // Headphone bus unmuted, mic NOT routed yet
    h.adapter.publishRenderConfig(cfg);

    DirectRun run;
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[0][0], 0.0f, 1e-4f) << "Monitor bus stays muted";
    EXPECT_NEAR(run.out[2][0], 0.0f, 1e-4f)
        << "unmuted Headphones are silent while mic->headphones is not routed";
    EXPECT_NEAR(run.out[3][0], 0.0f, 1e-4f);
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.25f, 1e-4f)
        << "the virtual-mic uplink carries the processed mic regardless of output routing";

    // Route the mic into the Headphone bus only -> HP pair gets 0.25, Monitor stays silent.
    cfg.micToHeadphones = true;
    ++cfg.revision;
    h.adapter.publishRenderConfig(cfg);
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[2][0], 0.25f, 2e-3f) << "processed mic reaches the Headphone bus L";
    EXPECT_NEAR(run.out[3][0], 0.25f, 2e-3f) << "processed mic reaches the Headphone bus R";
    EXPECT_NEAR(run.out[0][0], 0.0f, 1e-4f) << "Monitor bus remains muted/silent";
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.25f, 1e-4f);

    // Bring the MONITOR bus up at a different level: must NOT change the HP bus
    // nor the virtual-mic path.
    cfg.monitorMute = false;
    cfg.physicalOutputMute = false;
    cfg.monitorGain = 1.0f;
    cfg.physicalOutputGain = 0.5f; // -6 dB Monitor level
    ++cfg.revision;
    h.adapter.publishRenderConfig(cfg);
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[0][0], 0.25f * 0.5f, 3e-3f) << "Monitor bus at -6 dB";
    EXPECT_NEAR(run.out[2][0], 0.25f, 3e-3f) << "Headphone bus level unchanged by Monitor gain";
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.25f, 1e-4f)
        << "Monitor gain must never change the virtual-mic uplink";

    // Muting the Headphone bus leaves the Monitor bus (and the uplink) untouched.
    cfg.headphoneMute = true;
    ++cfg.revision;
    h.adapter.publishRenderConfig(cfg);
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[2][0], 0.0f, 1e-4f) << "Headphone bus muted";
    EXPECT_NEAR(run.out[0][0], 0.25f * 0.5f, 3e-3f) << "Monitor bus unaffected by Headphone mute";
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.25f, 1e-4f);
}

TEST(AsioRoutingAdapter, MonitorDimAndHeadphoneGainAreIndependent)
{
    std::string error;
    Harness h(256, 4);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.downlinkMute = true;
    cfg.monitorMute = false;
    cfg.micToHeadphones = true;
    cfg.headphoneMute = false;
    cfg.physicalOutputGain = 1.0f;
    cfg.headphoneGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);

    DirectRun run;
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[0][0], 0.25f, 2e-3f) << "Monitor at unity";
    EXPECT_NEAR(run.out[2][0], 0.25f, 2e-3f) << "Headphones at unity";

    // Dim applies ONLY to the Monitor bus (-20 dB by default).
    cfg.monitorDim = true;
    ++cfg.revision;
    h.adapter.publishRenderConfig(cfg);
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[0][0], 0.25f * cfg.monitorDimGain, 3e-3f) << "Monitor dimmed by monitorDimGain";
    EXPECT_NEAR(run.out[2][0], 0.25f, 3e-3f) << "Headphones unaffected by Monitor dim";
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.25f, 1e-4f) << "uplink unaffected by dim";

    // A Headphone gain change must not move the Monitor bus.
    cfg.headphoneGain = 0.5f;
    cfg.monitorDim = false;
    ++cfg.revision;
    h.adapter.publishRenderConfig(cfg);
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[0][0], 0.25f, 3e-3f) << "Monitor back to unity after dim off";
    EXPECT_NEAR(run.out[2][0], 0.25f * 0.5f, 3e-3f) << "Headphones at -6 dB";
}

TEST(AsioRoutingAdapter, OutputPairToneIsLowLevelOnChosenPairOnly)
{
    std::string error;
    Harness h(256, 4);
    ASSERT_TRUE(h.configure(error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    cfg.physicalOutputMute = true;
    cfg.headphoneMute = true;
    h.adapter.publishRenderConfig(cfg);

    DirectRun run;
    run.run(h.adapter, 0.25f);
    EXPECT_NEAR(run.out[0][0], 0.0f, 1e-4f);
    EXPECT_NEAR(run.out[2][0], 0.0f, 1e-4f);

    // Arm a LOW-level (-30 dBFS) identification tone on the SECONDARY pair.
    constexpr float kAmp = 0.0316f;
    h.adapter.armOutputPairTone(1, kAmp);
    run.run(h.adapter, 0.25f);

    EXPECT_GT(run.peak[2], kAmp * 0.9f) << "the armed pair must carry the tone";
    EXPECT_LE(run.peak[2], kAmp * 1.01f) << "the tone must stay at the armed low level (never full scale)";
    EXPECT_GT(run.peak[3], kAmp * 0.9f) << "the tone must reach the pair's right channel";
    EXPECT_LE(run.peak[0], 1e-4f) << "the primary pair keeps its (silent) mix";
    EXPECT_LE(run.peak[1], 1e-4f);

    // Disarm -> both pairs silent again.
    h.adapter.disarmOutputPairTone();
    run.run(h.adapter, 0.25f);
    EXPECT_LE(run.peak[2], 1e-4f);
    EXPECT_LE(run.peak[3], 1e-4f);
}

namespace
{

ChannelPlan productionPlanFromCapabilities(const DriverCapabilities& caps)
{
    std::vector<ChannelInfo> inputs;
    std::vector<ChannelInfo> outputs;
    for (const ChannelInfo& channel : caps.channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }
    return AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, outputs, false);
}

ChannelInfo inputAt(long index, std::string name)
{
    ChannelInfo channel{};
    channel.index = index;
    channel.isInput = true;
    channel.isActive = true;
    channel.name = std::move(name);
    channel.preferredFormat = SampleFormat::Float32LE;
    return channel;
}

} // namespace

TEST(AsioRoutingAdapter, IndependentRawAndPostMetersFollowAuthoritativeTaps)
{
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    const ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    ChainBundle gain2(2.0f, audient::vst3::BusLayout::Mono, 256);
    adapter.setChannelInputChain(0, &audient::vst3::Vst3Chain::processMono, &gain2.chain);
    adapter.attach();
    adapter.requestFadeIn();

    constexpr std::size_t kFrames = 64;
    std::vector<float> ch0(kFrames, 0.25f);
    std::vector<float> ch1(kFrames, -0.5f);
    const float* bothInputs[2] = {ch0.data(), ch1.data()};
    const float* ch0Only[2] = {ch0.data(), nullptr};
    const float* ch1Only[2] = {nullptr, ch1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = bothInputs;
    info.outputChannels = 0;
    info.outputs = nullptr;

    adapter.processAsio(info);
    const audient::engine::MeterSnapshot raw0 = adapter.inputRawMeter(0);
    const audient::engine::MeterSnapshot raw1 = adapter.inputRawMeter(1);
    const audient::engine::MeterSnapshot post0 = adapter.inputPostMeter(0);
    const audient::engine::MeterSnapshot post1 = adapter.inputPostMeter(1);
    EXPECT_NEAR(raw0.peakAbs, 0.25f, 1e-6f);
    EXPECT_NEAR(raw1.peakAbs, 0.5f, 1e-6f);
    EXPECT_NEAR(post0.peakAbs, 0.5f, 1e-6f);
    EXPECT_NEAR(post1.peakAbs, 0.5f, 1e-6f);
    EXPECT_NEAR(raw0.rmsAbs, 0.25f, 1e-6f);
    EXPECT_NEAR(raw1.rmsAbs, 0.5f, 1e-6f);

    info.inputs = ch0Only;
    adapter.processAsio(info);
    EXPECT_NEAR(adapter.inputRawMeter(0).rmsAbs, 0.25f, 1e-6f);
    EXPECT_EQ(adapter.inputRawMeter(1).rmsAbs, 0.0f) << "missing channel 1 must publish zero, not stale audio";
    EXPECT_EQ(adapter.inputPostMeter(1).rmsAbs, 0.0f) << "missing channel 1 post meter must publish zero";

    info.inputs = ch1Only;
    adapter.processAsio(info);
    EXPECT_EQ(adapter.inputRawMeter(0).rmsAbs, 0.0f) << "missing channel 0 must publish zero";
    EXPECT_EQ(adapter.inputPostMeter(0).rmsAbs, 0.0f) << "missing channel 0 post meter must publish zero";
    EXPECT_NEAR(adapter.inputRawMeter(1).rmsAbs, 0.5f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(1).rmsAbs, 0.5f, 1e-6f);
}

TEST(AsioRoutingAdapter, WholeChainBypassChangesPostMeterOnly)
{
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    const ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    ChainBundle gain2(2.0f, audient::vst3::BusLayout::Mono, 256);
    adapter.setChannelInputChain(0, &audient::vst3::Vst3Chain::processMono, &gain2.chain);
    adapter.attach();
    adapter.requestFadeIn();

    constexpr std::size_t kFrames = 64;
    std::vector<float> input(kFrames, 0.25f);
    const float* inputs[2] = {input.data(), nullptr};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inputs;
    adapter.processAsio(info);
    EXPECT_NEAR(adapter.inputRawMeter(0).peakAbs, 0.25f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(0).peakAbs, 0.5f, 1e-6f);

    ASSERT_TRUE(gain2.chain.publish({{gain2.processor.get(), false}}, true));
    adapter.processAsio(info);
    EXPECT_NEAR(adapter.inputRawMeter(0).peakAbs, 0.25f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(0).rmsAbs, 0.25f, 1e-6f);
}

TEST(AsioRoutingAdapter, SimulatedDualChannelProductionPath)
{
    // B6: one complete simulated dual-input integration path. Ch0 -> VST gain 2
    // -> meters -> channel-0 Virtual Mic; ch1 -> independent VST gain 0.5 ->
    // meters -> independent (not summed) channel-1 state.
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    const ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    ChainBundle ch0Gain2(2.0f, audient::vst3::BusLayout::Mono, 256);
    ChainBundle ch1Half(0.5f, audient::vst3::BusLayout::Mono, 256);
    adapter.setChannelInputChain(0, &audient::vst3::Vst3Chain::processMono, &ch0Gain2.chain);
    adapter.setChannelInputChain(1, &audient::vst3::Vst3Chain::processMono, &ch1Half.chain);
    adapter.attach();
    for (int i = 0; i < 300; ++i) { adapter.processAsio(AsioCallbackInfo{}); }
    adapter.requestFadeIn();

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    cfg.downlinkGain = 0.0f;
    cfg.micToHeadphones = false;
    cfg.downlinkToHeadphones = false;
    adapter.publishRenderConfig(cfg);
    for (int i = 0; i < 300; ++i) { AsioCallbackInfo warm{}; warm.sampleCount = 64; adapter.processAsio(warm); }

    constexpr std::size_t kFrames = 64;
    std::vector<float> in0(kFrames, 0.2f);
    std::vector<float> in1(kFrames, 0.8f);
    const float* both[2] = {in0.data(), in1.data()};
    const float* only0[2] = {in0.data(), nullptr};
    const float* only1[2] = {nullptr, in1.data()};
    float outL[kFrames]{};
    float outR[kFrames]{};
    float* outputs[2] = {outL, outR};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = both;
    info.outputChannels = 2;
    info.outputs = outputs;

    for (int i = 0; i < 300; ++i) { adapter.processAsio(info); }
    EXPECT_NEAR(adapter.inputRawMeter(0).peakAbs, 0.2f, 1e-6f);
    EXPECT_NEAR(adapter.inputRawMeter(1).peakAbs, 0.8f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(0).peakAbs, 0.4f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(1).peakAbs, 0.4f, 1e-6f);
    EXPECT_NEAR(adapter.inputProcessedBuffer(0)[0], 0.4f, 1e-5f);
    EXPECT_NEAR(adapter.inputProcessedBuffer(1)[0], 0.4f, 1e-5f);
    EXPECT_NEAR(adapter.micUplinkBuffer()[0], 0.4f, 1e-5f) << "Virtual Mic is channel 0 only (no ch1 leak)";
    EXPECT_EQ(adapter.configuredInputChannels(), 2u);

    info.inputs = only0;
    for (int i = 0; i < 2; ++i) { adapter.processAsio(info); }
    EXPECT_NEAR(adapter.inputRawMeter(0).peakAbs, 0.2f, 1e-6f);
    EXPECT_EQ(adapter.inputRawMeter(1).rmsAbs, 0.0f) << "missing ch1 -> zero, no stale replay";
    EXPECT_EQ(adapter.inputPostMeter(1).rmsAbs, 0.0f);
    EXPECT_NEAR(adapter.inputPostMeter(0).peakAbs, 0.4f, 1e-6f);
    EXPECT_EQ(adapter.inputProcessedBuffer(1)[0], 0.0f);

    info.inputs = only1;
    for (int i = 0; i < 2; ++i) { adapter.processAsio(info); }
    EXPECT_EQ(adapter.inputRawMeter(0).rmsAbs, 0.0f) << "missing ch0 -> zero";
    EXPECT_EQ(adapter.inputPostMeter(0).rmsAbs, 0.0f);
    EXPECT_NEAR(adapter.inputRawMeter(1).peakAbs, 0.8f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(1).peakAbs, 0.4f, 1e-6f);
    EXPECT_EQ(adapter.inputProcessedBuffer(0)[0], 0.0f);

    info.inputs = both;
    for (int i = 0; i < 2; ++i) { adapter.processAsio(info); }
    ASSERT_TRUE(ch0Gain2.chain.publish({{ch0Gain2.processor.get(), false}}, true));
    for (int i = 0; i < 300; ++i) { adapter.processAsio(info); }
    EXPECT_NEAR(adapter.inputRawMeter(0).peakAbs, 0.2f, 1e-6f) << "raw unchanged under bypass";
    EXPECT_NEAR(adapter.inputPostMeter(0).rmsAbs, 0.2f, 1e-6f) << "post reflects bypass (wire) output";
    EXPECT_NEAR(adapter.inputPostMeter(1).rmsAbs, 0.4f, 1e-6f) << "ch1 bypass independent";
    ASSERT_TRUE(ch0Gain2.chain.publish({{ch0Gain2.processor.get(), false}}, false));
    for (int i = 0; i < 300; ++i) { adapter.processAsio(info); }
    EXPECT_NEAR(adapter.inputPostMeter(0).rmsAbs, 0.4f, 1e-6f);
}

TEST(AsioRoutingAdapter, PerChannelSendStateIsIndependentAndNeverShared)
{
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    const ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;
    adapter.attach();
    adapter.requestFadeIn();

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    cfg.downlinkGain = 0.0f;
    cfg.micToHeadphones = false;
    cfg.downlinkToHeadphones = false;
    adapter.publishRenderConfig(cfg);

    constexpr std::size_t kFrames = 64;
    std::vector<float> ch0(kFrames, 0.25f);
    std::vector<float> ch1(kFrames, 0.5f);
    const float* bothInputs[2] = {ch0.data(), ch1.data()};
    float outL2[kFrames]{};
    float outR2[kFrames]{};
    float* outputPtrs2[2] = {outL2, outR2};
    AsioCallbackInfo info2{};
    info2.sampleCount = static_cast<long>(kFrames);
    info2.inputChannels = 2;
    info2.inputs = bothInputs;
    info2.outputChannels = 2;
    info2.outputs = outputPtrs2;

    audient::channel::ChannelRuntimeSnapshot state0;
    state0.virtualMicSend = {true, 0.0f, false, 0};
    state0.localMonitorSend = {false, -18.0f, true, 0};
    state0.revision = 1;
    audient::channel::ChannelRuntimeSnapshot state1;
    state1.virtualMicSend = {true, -6.0f, false, 0};
    state1.localMonitorSend = {false, -18.0f, true, 0};
    state1.revision = 1;
    adapter.publishChannelRuntime(0, state0);
    adapter.publishChannelRuntime(1, state1);

    for (int i = 0; i < 300; ++i)
    {
        adapter.processAsio(info2);
    }
    EXPECT_NEAR(adapter.micUplinkBuffer()[0], 0.25f + 0.5f * std::pow(10.0f, -6.0f / 20.0f), 1e-3f)
        << "C1 Virtual Mic is transparent linear sum of slot0 Mic1 and slot1 Mic2 (no clamp in mixer)";
    EXPECT_NEAR(adapter.inputRawMeter(0).peakAbs, 0.25f, 1e-6f);
    EXPECT_NEAR(adapter.inputRawMeter(1).peakAbs, 0.5f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(0).peakAbs, 0.25f, 1e-6f);
    EXPECT_NEAR(adapter.inputPostMeter(1).peakAbs, 0.5f, 1e-6f);

    const auto before1 = adapter.channelRuntime(1);
    const auto before1Rev = before1.revision;
    audient::channel::ChannelRuntimeSnapshot state0b;
    state0b.virtualMicSend = {true, -18.0f, true, 0};
    state0b.localMonitorSend = {false, -18.0f, true, 0};
    state0b.revision = 2;
    adapter.publishChannelRuntime(0, state0b);
    for (int i = 0; i < 100; ++i)
    {
        adapter.processAsio(info2);
    }
    const auto after1 = adapter.channelRuntime(1);
    EXPECT_EQ(after1.revision, before1Rev);
    EXPECT_EQ(after1.virtualMicSend.levelDb, before1.virtualMicSend.levelDb);
    EXPECT_EQ(after1.virtualMicSend.muted, before1.virtualMicSend.muted);
    EXPECT_EQ(after1.localMonitorSend.levelDb, before1.localMonitorSend.levelDb);
    EXPECT_EQ(adapter.inputPostMeter(1).peakAbs, 0.5f) << "channel 1 state unchanged by channel 0 mutation";


}

TEST(AsioRoutingAdapter, DualPlanBindsTwoPhysicalInputsToTwoRuntimeSlots)
{
    // A/B/C/E integration: two distinct driver channels bound to two distinct
    // runtime slots; slot 1 is processed independently and never summed into
    // the Virtual Mic (uplink stays channel 0).
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);

    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    const ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    adapter.publishRenderConfig(cfg);
    adapter.attach();
    adapter.requestFadeIn();
    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before the staging is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ASSERT_TRUE(backend.stop(error));

    EXPECT_EQ(adapter.configuredInputChannels(), 2u);
    const std::vector<float>& p0 = adapter.inputProcessedBuffer(0);
    const std::vector<float>& p1 = adapter.inputProcessedBuffer(1);
    const std::vector<float>& uplink = adapter.micUplinkBuffer();
    ASSERT_FALSE(p0.empty());
    ASSERT_FALSE(p1.empty());
    EXPECT_NEAR(p0[0], 0.25f, 1e-6f) << "slot 0 processes Analog Input 1";
    EXPECT_NEAR(p1[0], 0.5f, 1e-6f) << "slot 1 processes Analog Input 2 independently";
    EXPECT_NEAR(uplink[0], 0.25f, 1e-6f) << "the Virtual Mic carries channel 0 only (no Phase-C summing)";
}

TEST(AsioRoutingAdapter, DriverIndexIndependenceIgnoresEnumerationOrder)
{
    // #1/#2: driver index does NOT need to equal the runtime slot. Enumeration
    // is reversed and non-contiguous ("Analogue 2" at 7 before "Analogue 1" at
    // 4); the binding table resolves Analog Input 1 -> slot 0 (index 4) and
    // Analog Input 2 -> slot 1 (index 7), and the callback binds by index.
    std::string error;
    SimulatedAsioProvider provider;
    provider.setInputChannelLayout({inputAt(7, "Analogue 2"), inputAt(4, "Analogue 1")});
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);

    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    const ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    EXPECT_EQ(plan.inputs[0].identity, audient::channel::makeAnalogInput(1));
    EXPECT_EQ(plan.inputs[0].asioChannelIndex, 4L);
    EXPECT_EQ(plan.inputs[1].identity, audient::channel::makeAnalogInput(2));
    EXPECT_EQ(plan.inputs[1].asioChannelIndex, 7L);
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    adapter.publishRenderConfig(cfg);
    adapter.attach();
    adapter.requestFadeIn();
    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ASSERT_TRUE(backend.stop(error));

    EXPECT_EQ(adapter.configuredInputChannels(), 2u);
    const std::vector<float>& p0 = adapter.inputProcessedBuffer(0);
    const std::vector<float>& p1 = adapter.inputProcessedBuffer(1);
    ASSERT_FALSE(p0.empty());
    ASSERT_FALSE(p1.empty());
    // The simulated driver feeds each requested slot a tone keyed to the ASIO
    // index bound to that slot: 0.25*(index+1). A swap (slot 0 = Analogue 2)
    // would read 2.0 here; the binding table guarantees the correct 1.25/2.0.
    EXPECT_NEAR(p0[0], 0.25f * 5.0f, 1e-6f) << "slot 0 must carry ASIO index 4 (Analogue 1)";
    EXPECT_NEAR(p1[0], 0.25f * 8.0f, 1e-6f) << "slot 1 must carry ASIO index 7 (Analogue 2)";
}

TEST(AsioRoutingAdapter, MissingSecondInputSlotZeroesSinksNeverAliasesOrReplays)
{
    // G: Input 2 disappears -> slot 1 becomes silence, slot 0 is unchanged, no
    // stale block replay and no aliasing of slot 0 into slot 1.
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);

    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    const ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    adapter.publishRenderConfig(cfg);
    adapter.attach();
    adapter.requestFadeIn();

    constexpr std::size_t kFrames = 64;
    std::vector<float> in0(kFrames, 0.11f);
    std::vector<float> in1(kFrames, 0.22f);
    float out0[kFrames]{};
    float out1[kFrames]{};
    const float* inputPtrs[2] = {in0.data(), in1.data()};
    const float* missingPtrs[2] = {in0.data(), nullptr};
    float* outputPtrs[4] = {out0, out1, out0, out1};

    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inputPtrs;
    info.outputChannels = 4;
    info.outputs = outputPtrs;

    for (int i = 0; i < 300; ++i)
    {
        adapter.processAsio(info); // fade completes; both slots present
    }
    EXPECT_NEAR(adapter.inputProcessedBuffer(0)[0], 0.11f, 1e-6f);
    EXPECT_NEAR(adapter.inputProcessedBuffer(1)[0], 0.22f, 1e-6f);

    // Input 2 disappears (null source): its sinks must go to silence, never
    // replaying the previous 0.22 block, and slot 0 must be untouched.
    info.inputs = missingPtrs;
    for (int i = 0; i < 100; ++i)
    {
        adapter.processAsio(info);
    }
    EXPECT_EQ(adapter.inputProcessedBuffer(1)[0], 0.0f) << "slot 1 must be silence, not stale replay";
    EXPECT_EQ(adapter.inputRawBuffer(1)[0], 0.0f);
    EXPECT_EQ(adapter.inputMonitorFeedBuffer(1)[0], 0.0f);
    EXPECT_NEAR(adapter.inputProcessedBuffer(0)[0], 0.11f, 1e-6f) << "slot 0 remains unchanged";
    EXPECT_NEAR(adapter.micUplinkBuffer()[0], 0.11f, 1e-6f) << "uplink continues from channel 0";
}

TEST(AsioRoutingAdapter, ConfigureRejectsPlanReferencingMissingDriverIndex)
{
    // Never guess or alias: a binding that references a driver index the device
    // does not expose is refused at configure time.
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;

    ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    plan.inputs[1].asioChannelIndex = 99L; // not present on the simulated device
    ASSERT_TRUE(plan.valid()) << "the plan is structurally valid but references a missing index";

    EXPECT_FALSE(backend.configure(48000, 64, plan, error));
    EXPECT_FALSE(error.empty());
}

TEST(AsioRoutingAdapter, LegacyMicInputAliasCannotOverridePopulatedBindingTable)
{
    // Invariant 1: a populated binding table is authoritative. A conflicting
    // legacy micInput alias must never change the runtime binding; it is
    // normalized to mirror inputs[0].asioChannelIndex.
    std::string error;
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);
    AsioRoutingAdapter adapter(backend, 256);

    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    ChannelPlan plan = productionPlanFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    EXPECT_EQ(plan.inputs[0].asioChannelIndex, 0L);

    // Tamper the legacy alias so it conflicts with the populated table
    // (slot 0 = ASIO index 0; a stale/legacy alias claims index 1).
    plan.micInput = 1L;
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    // The table stays authoritative; the alias is normalized to mirror it.
    EXPECT_EQ(backend.activePlan().micInput, 0L);
    EXPECT_EQ(backend.activePlan().requestedInputCount(), 2u);

    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = true;
    adapter.publishRenderConfig(cfg);
    adapter.attach();
    adapter.requestFadeIn();
    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return adapter.fadeIsFull(); }, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ASSERT_TRUE(backend.stop(error));

    EXPECT_EQ(adapter.configuredInputChannels(), 2u);
    EXPECT_NEAR(adapter.inputProcessedBuffer(0)[0], 0.25f, 1e-6f)
        << "slot 0 must follow the TABLE (index 0), never the conflicting alias (index 1)";
    EXPECT_NEAR(adapter.inputProcessedBuffer(1)[0], 0.5f, 1e-6f);
}
