#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioRoutingAdapter.h"
#include "channel/ChannelTypes.h"
#include "core/AllocationTracker.h"
#include "transport/TransportLinks.h"
#include "transport/VirtualCaptureTransport.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

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

using namespace audient::asio;
using namespace audient::channel;
using audient::transport::VirtualCaptureTransport;

namespace
{

ChannelPlan planFromCapabilities(const DriverCapabilities& caps)
{
    std::vector<ChannelInfo> inputs;
    std::vector<ChannelInfo> outputs;
    for (const ChannelInfo& c : caps.channels)
    {
        (c.isInput ? inputs : outputs).push_back(c);
    }
    return AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, outputs, false);
}

struct Harness
{
    SimulatedAsioProvider provider;
    AsioBackend backend;
    AsioRoutingAdapter adapter;
    int outputChannels = 2;
    explicit Harness(std::size_t maxBlock, int outs = 2)
        : backend(provider)
        , adapter(backend, maxBlock)
        , outputChannels(outs)
    {
    }
    bool configure(std::string& error)
    {
        provider.setOutputChannels(outputChannels);
        if (!backend.selectAudientDevice(error))
        {
            return false;
        }
        const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
        if (!plan.valid())
        {
            error = "plan invalid";
            return false;
        }
        return backend.configure(48000, 64, plan, error);
    }
};

ChannelRuntimeSnapshot makeRuntime(bool enabled, float levelDb, bool muted, std::uint64_t rev)
{
    ChannelRuntimeSnapshot s;
    s.virtualMicSend.enabled = enabled;
    s.virtualMicSend.levelDb = levelDb;
    s.virtualMicSend.muted = muted;
    s.virtualMicSend.revision = rev;
    s.localMonitorSend.enabled = false;
    s.localMonitorSend.muted = false;
    s.revision = rev;
    return s;
}

void settle(AsioRoutingAdapter& adapter, const float* in0, const float* in1, std::size_t frames,
            int inputChannels, int outputChannels, int blocks = 250)
{
    std::vector<std::vector<float>> outs(outputChannels, std::vector<float>(frames, 0.0f));
    std::vector<float*> outPtrs(outputChannels);
    for (int i = 0; i < outputChannels; ++i)
    {
        outPtrs[i] = outs[i].data();
    }
    const float* inPtrs[2] = {in0, in1};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(frames);
    info.inputChannels = inputChannels;
    info.inputs = inPtrs;
    info.outputChannels = outputChannels;
    info.outputs = outPtrs.data();
    adapter.requestFadeIn();
    for (int i = 0; i < blocks; ++i)
    {
        adapter.processAsio(info);
        if (adapter.fadeIsFull())
        {
        }
    }
}

} // namespace

TEST(CaptureMixerIntegration, Fallback_Slot0Mic1Unity_Slot1Mic2Absent)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.attach();

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.25f);
    std::vector<float> inMic2(kFrames, 0.90f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> out0(kFrames, 0.0f);
    std::vector<float> out1(kFrames, 0.0f);
    float* outPtrs[2] = {out0.data(), out1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;

    h.adapter.requestFadeIn();
    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    ASSERT_TRUE(h.adapter.fadeIsFull());
    const auto& uplink = h.adapter.micUplinkBuffer();
    EXPECT_NEAR(uplink[0], 0.25f, 2e-3f) << "fallback: internal slot0 (Mic1) unity, slot1 (Mic2) absent from VMic";
    EXPECT_NE(uplink[0], 0.90f);
}

TEST(CaptureMixerIntegration, Mic1Only_ViaExplicitSend)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeRuntime(true, 0.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeRuntime(false, 0.0f, false, 1));
    h.adapter.attach();

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.30f);
    std::vector<float> inMic2(kFrames, 0.80f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> out0(kFrames, 0.0f);
    std::vector<float> out1(kFrames, 0.0f);
    float* outPtrs[2] = {out0.data(), out1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    h.adapter.requestFadeIn();
    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.30f, 2e-3f) << "slot0 Mic1 only";
}

TEST(CaptureMixerIntegration, Mic2Only_Slot1Mic2)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeRuntime(false, 0.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeRuntime(true, 0.0f, false, 1));
    h.adapter.attach();

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.30f);
    std::vector<float> inMic2(kFrames, -0.40f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> out0(kFrames, 0.0f);
    std::vector<float> out1(kFrames, 0.0f);
    float* outPtrs[2] = {out0.data(), out1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    h.adapter.requestFadeIn();
    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], -0.40f, 2e-3f) << "slot1 Mic2 only";
}

TEST(CaptureMixerIntegration, BothMics_LinearSum_HeadroomPreserved)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeRuntime(true, 0.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeRuntime(true, 0.0f, false, 2));
    h.adapter.attach();

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.80f);
    std::vector<float> inMic2(kFrames, 0.90f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> out0(kFrames, 0.0f);
    std::vector<float> out1(kFrames, 0.0f);
    float* outPtrs[2] = {out0.data(), out1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    h.adapter.requestFadeIn();
    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 1.70f, 3e-3f)
        << "transparent linear sum: no clamp inside CaptureMixer, headroom preserved (downstream may clamp)";
}

TEST(CaptureMixerIntegration, PerChannelGain_Mic2AtMinus6dB)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeRuntime(true, 0.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeRuntime(true, -6.0f, false, 1));
    h.adapter.attach();

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.60f);
    std::vector<float> inMic2(kFrames, 1.00f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> out0(kFrames, 0.0f);
    std::vector<float> out1(kFrames, 0.0f);
    float* outPtrs[2] = {out0.data(), out1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    h.adapter.requestFadeIn();
    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    const float expected = 0.60f + 1.00f * std::pow(10.0f, -6.0f / 20.0f);
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], expected, 5e-3f);
}

TEST(CaptureMixerIntegration, MutedChannel_Silent)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeRuntime(true, 0.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeRuntime(true, 0.0f, true, 1));
    h.adapter.attach();

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.50f);
    std::vector<float> inMic2(kFrames, 0.50f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> out0(kFrames, 0.0f);
    std::vector<float> out1(kFrames, 0.0f);
    float* outPtrs[2] = {out0.data(), out1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    h.adapter.requestFadeIn();
    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.50f, 2e-3f) << "slot1 Mic2 muted -> silent";
}

TEST(CaptureMixerIntegration, PublishedViaTransport_DiagnosticTap)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeRuntime(true, 0.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeRuntime(true, 0.0f, false, 1));
    h.adapter.attach();
    VirtualCaptureTransport transport({48000, 1, 64, audient::transport::SampleType::Float32}, 65536);
    h.adapter.setCaptureTransport(&transport);

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.20f);
    std::vector<float> inMic2(kFrames, 0.30f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> out0(kFrames, 0.0f);
    std::vector<float> out1(kFrames, 0.0f);
    float* outPtrs[2] = {out0.data(), out1.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    h.adapter.requestFadeIn();
    for (int i = 0; i < 300; ++i)
    {
        h.adapter.processAsio(info);
    }
    ASSERT_TRUE(h.adapter.fadeIsFull());
    EXPECT_NEAR(h.adapter.micUplinkBuffer()[0], 0.50f, 3e-3f);
    int drains = 0;
    float best = 0.0f;
    for (int i = 0; i < 600; ++i)
    {
        std::vector<float> read(kFrames, 0.0f);
        if (transport.readProcessedMic(read.data(), kFrames))
        {
            ++drains;
            for (float s : read)
            {
                best = std::max(best, std::fabs(s));
            }
            if (best >= 0.49f)
            {
                break;
            }
        }
    }
    EXPECT_GT(drains, 0);
    EXPECT_NEAR(best, 0.50f, 3e-3f) << "mixed capture bus visible via internal transport diagnostic (fade skips early silence)";
}

static ChannelRuntimeSnapshot makeLocalMonitorRuntime(bool enabled, float levelDb, bool muted,
                                                    std::uint64_t rev)
{
    ChannelRuntimeSnapshot s;
    s.virtualMicSend.enabled = false;
    s.localMonitorSend.enabled = enabled;
    s.localMonitorSend.levelDb = levelDb;
    s.localMonitorSend.muted = muted;
    s.localMonitorSend.revision = 0;
    s.revision = rev;
    return s;
}

TEST(CaptureMixerIntegration, LocalMonitorFallback_CH0LegacyUnity_WhenRev0)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.attach();
    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorGain = 1.0f;
    cfg.monitorMute = false;
    cfg.downlinkMute = true;
    cfg.physicalOutputMute = false;
    cfg.downlinkGain = 0.0f;
    cfg.physicalOutputGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);

    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.40f);
    std::vector<float> inMic2(kFrames, 0.40f);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    std::vector<float> outL(kFrames, 0.0f);
    std::vector<float> outR(kFrames, 0.0f);
    float* outPtrs[2] = {outL.data(), outR.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    settle(h.adapter, inMic1.data(), inMic2.data(), kFrames, 2, 2, 300);
    h.adapter.processAsio(info);
    EXPECT_NEAR(outL[0], 0.40f, 5e-3f) << "rev0 fallback: CH0 via legacy monitor bus, CH1 silent";
    EXPECT_NEAR(outR[0], 0.40f, 5e-3f);
}

TEST(CaptureMixerIntegration, LocalMonitor_CH1SendsAlone_AndBothSummed)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeLocalMonitorRuntime(false, 0.0f, true, 1));
    h.adapter.publishChannelRuntime(1, makeLocalMonitorRuntime(true, 0.0f, false, 1));
    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorGain = 0.0f;
    cfg.downlinkMute = true;
    cfg.downlinkGain = 0.0f;
    cfg.physicalOutputGain = 1.0f;
    cfg.physicalOutputMute = false;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.attach();
    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 0.40f);
    std::vector<float> inMic2(kFrames, 0.30f);
    std::vector<float> outL(kFrames, 0.0f);
    std::vector<float> outR(kFrames, 0.0f);
    const float* inPtrsB[2] = {inMic1.data(), inMic2.data()};
    float* outPtrs[2] = {outL.data(), outR.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrsB;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    settle(h.adapter, inMic1.data(), inMic2.data(), kFrames, 2, 2, 300);
    h.adapter.processAsio(info);
    EXPECT_NEAR(outL[0], 0.30f, 6e-3f) << "CH1 alone via per-channel send";
    h.adapter.publishChannelRuntime(0, makeLocalMonitorRuntime(true, 0.0f, false, 2));
    h.adapter.publishChannelRuntime(1, makeLocalMonitorRuntime(true, 0.0f, false, 2));
    settle(h.adapter, inMic1.data(), inMic2.data(), kFrames, 2, 2, 300);
    h.adapter.processAsio(info);
    EXPECT_NEAR(outL[0], 0.70f, 6e-3f) << "both: mono sum to both L/R";
    EXPECT_NEAR(outR[0], 0.70f, 6e-3f);
}

TEST(CaptureMixerIntegration, LocalMonitor_Minus6dB_Attenuates)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeLocalMonitorRuntime(true, -6.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeLocalMonitorRuntime(false, 0.0f, true, 1));
    AsioRoutingAdapter::RenderConfig cfg;
    cfg.downlinkMute = true;
    cfg.downlinkGain = 0.0f;
    cfg.physicalOutputGain = 1.0f;
    h.adapter.publishRenderConfig(cfg);
    h.adapter.attach();
    constexpr std::size_t kFrames = 64;
    std::vector<float> inMic1(kFrames, 1.00f);
    std::vector<float> inMic2(kFrames, 0.0f);
    std::vector<float> outL(kFrames, 0.0f);
    std::vector<float> outR(kFrames, 0.0f);
    float* outPtrs[2] = {outL.data(), outR.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    const float* inPtrs[2] = {inMic1.data(), inMic2.data()};
    info.inputs = inPtrs;
    info.inputChannels = 2;
    info.outputChannels = 2;
    info.outputs = outPtrs;
    settle(h.adapter, inMic1.data(), inMic2.data(), kFrames, 2, 2, 400);
    h.adapter.processAsio(info);
    EXPECT_NEAR(outL[0], std::pow(10.0f, -6.0f / 20.0f), 2e-2f);
}

TEST(CaptureMixerIntegration, LocalMonitor_MonoFold_ON_CollapsesAsymmetricDownlink)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeLocalMonitorRuntime(false, 0.0f, true, 1));
    h.adapter.publishChannelRuntime(1, makeLocalMonitorRuntime(false, 0.0f, true, 1));
    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.monitorGain = 0.0f;
    cfg.downlinkMute = false;
    cfg.downlinkGain = 1.0f;
    cfg.physicalOutputGain = 1.0f;
    cfg.physicalOutputMute = false;
    cfg.outputMono = true;
    h.adapter.publishRenderConfig(cfg);
    EXPECT_TRUE(h.adapter.renderConfig().outputMono);
    constexpr std::size_t kFrames = 64;
    constexpr float kLeft = 0.70f;
    constexpr float kRight = -0.30f;
    constexpr float kMono = 0.5f * (kLeft + kRight);
    audient::transport::DownlinkTransport dl(
        audient::transport::Format{48000, 2, static_cast<int>(kFrames), audient::transport::SampleType::Float32}, 4096);
    std::vector<float> block(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) { block[2*i]=kLeft; block[2*i+1]=kRight; }
    h.adapter.setDownlinkTransport(&dl);
    h.adapter.attach();
    h.adapter.requestFadeIn();
    std::vector<float> silence(kFrames, 0.0f);
    for (int i=0;i<400;++i){ dl.writeBlock(block.data(), kFrames); AsioCallbackInfo si{}; si.sampleCount=(long)kFrames; const float* inP[2]={silence.data(), silence.data()}; si.inputs=inP; si.inputChannels=2; std::vector<float> oL(kFrames,0),oR(kFrames,0); float* oP[2]={oL.data(),oR.data()}; si.outputs=oP; si.outputChannels=2; h.adapter.processAsio(si); }
    std::vector<float> outL(kFrames, 9.9f), outR(kFrames, -9.9f);
    float* outPtrs[2]={outL.data(), outR.data()};
    AsioCallbackInfo info{}; info.sampleCount=(long)kFrames; const float* inPtrs[2]={silence.data(), silence.data()}; info.inputs=inPtrs; info.inputChannels=2; info.outputs=outPtrs; info.outputChannels=2;
    dl.writeBlock(block.data(), kFrames);
    h.adapter.processAsio(info);
    EXPECT_NEAR(outL[0], kMono, 1e-3f) << "mono ON L == 0.5*(L+R)";
    EXPECT_NEAR(outR[0], kMono, 1e-3f) << "mono ON R == 0.5*(L+R)";
    EXPECT_NEAR(outL[0], outR[0], 1e-5f) << "mono ON L==R phantom center";
    EXPECT_GT(std::abs(kLeft - kRight), 0.5f);
    EXPECT_LT(std::abs(outL[0] - kLeft), 1.0f);
    h.adapter.setDownlinkTransport(nullptr);
}

TEST(CaptureMixerIntegration, LocalMonitor_MonoFold_OFF_PreservesAsymmetricStereo)
{
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeLocalMonitorRuntime(false, 0.0f, true, 1));
    AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorMute = true;
    cfg.downlinkMute = false;
    cfg.downlinkGain = 1.0f;
    cfg.physicalOutputGain = 1.0f;
    cfg.outputMono = false;
    EXPECT_FALSE(h.adapter.renderConfig().outputMono);
    h.adapter.attach();
    constexpr std::size_t kFrames = 64;
    constexpr float kLeft = 0.70f;
    constexpr float kRight = -0.30f;
    audient::transport::DownlinkTransport dl(
        audient::transport::Format{48000, 2, static_cast<int>(kFrames), audient::transport::SampleType::Float32}, 4096);
    std::vector<float> block(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) { block[2*i]=kLeft; block[2*i+1]=kRight; }
    h.adapter.setDownlinkTransport(&dl);
    h.adapter.requestFadeIn();
    std::vector<float> silence(kFrames, 0.0f);
    for (int i=0;i<400;++i){ dl.writeBlock(block.data(), kFrames); AsioCallbackInfo si{}; si.sampleCount=(long)kFrames; const float* inP[2]={silence.data(), silence.data()}; si.inputs=inP; si.inputChannels=2; std::vector<float> oL(kFrames,0),oR(kFrames,0); float* oP[2]={oL.data(),oR.data()}; si.outputs=oP; si.outputChannels=2; h.adapter.processAsio(si); }
    std::vector<float> outL(kFrames, 9.9f), outR(kFrames, -9.9f);
    float* outPtrs[2]={outL.data(), outR.data()};
    AsioCallbackInfo info{}; info.sampleCount=(long)kFrames; const float* inPtrs[2]={silence.data(), silence.data()}; info.inputs=inPtrs; info.inputChannels=2; info.outputs=outPtrs; info.outputChannels=2;
    dl.writeBlock(block.data(), kFrames);
    h.adapter.processAsio(info);
    EXPECT_NEAR(outL[0], kLeft, 1e-3f) << "mono OFF L preserves original";
    EXPECT_NEAR(outR[0], kRight, 1e-3f) << "mono OFF R preserves original";
    EXPECT_GT(std::abs(outL[0]-outR[0]), 0.5f) << "mono OFF L!=R";
    EXPECT_FALSE(std::abs(outL[0]-outR[0]) < 1e-4f);
    h.adapter.setDownlinkTransport(nullptr);
}
TEST(CaptureMixerIntegration, CallbackStillZeroAlloc_WithTwoSlotMix)
{
    const bool supported = audient::core::isAllocationTrackerEnabled() ||
                           audient::core::installAllocationTracker() == audient::core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker only in Debug";
    }
    std::string error;
    Harness h(256, 2);
    ASSERT_TRUE(h.configure(error)) << error;
    h.adapter.publishChannelRuntime(0, makeRuntime(true, 0.0f, false, 1));
    h.adapter.publishChannelRuntime(1, makeRuntime(true, 0.0f, false, 1));
    h.adapter.attach();
    h.adapter.requestFadeIn();

    constexpr std::size_t kFrames = 64;
    std::vector<float> in0(kFrames, 0.10f);
    std::vector<float> in1(kFrames, -0.10f);
    std::vector<float> outL(kFrames, 0.0f);
    std::vector<float> outR(kFrames, 0.0f);
    const float* inPtrs[2] = {in0.data(), in1.data()};
    float* outPtrs[2] = {outL.data(), outR.data()};
    AsioCallbackInfo info{};
    info.sampleCount = static_cast<long>(kFrames);
    info.inputChannels = 2;
    info.inputs = inPtrs;
    info.outputChannels = 2;
    info.outputs = outPtrs;

    const std::uint64_t baseline = audient::core::allocationCountOnRealtimeThread();
    audient::core::markThreadRealtime(currentThreadId());
    for (int i = 0; i < 20000; ++i)
    {
        h.adapter.processAsio(info);
    }
    audient::core::clearRealtimeMark();
    EXPECT_EQ(audient::core::allocationCountOnRealtimeThread(), baseline)
        << "two-slot VMic mix + monitor CH0-only must remain zero-alloc";
}
