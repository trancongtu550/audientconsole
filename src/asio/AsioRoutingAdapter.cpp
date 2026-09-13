#include "asio/AsioRoutingAdapter.h"

#include "routing/CaptureMixer.h"
#include "transport/VirtualCaptureTransport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audient::asio
{

using routing::BusId;
using routing::BlockBinding;

namespace
{
constexpr std::uint32_t kToneCadenceSamples = 4800; // 100 ms on / 100 ms off at 48 kHz
// Physical-output de-pop priming: hold exact zero for ~2 ms at 48 kHz, then the
// existing stream fade performs the 0 -> 1 ramp (so fadeIsFull() always implies
// a fully ramped physical output).
constexpr std::size_t kDePopPrimeSamples = 96;
}

AsioRoutingAdapter::AsioRoutingAdapter(AsioBackend& backend, std::size_t maxBlockSamples)
    : m_backend(backend)
    , m_maxBlock(maxBlockSamples)
    , m_routing(maxBlockSamples)
    , m_micUplink(maxBlockSamples, 0.0f)
    , m_outProcessedL(maxBlockSamples, 0.0f)
    , m_outProcessedR(maxBlockSamples, 0.0f)
    , m_outScratchL(maxBlockSamples, 0.0f)
    , m_outScratchR(maxBlockSamples, 0.0f)
    , m_stageLeft(maxBlockSamples, 0.0f)
    , m_stageRight(maxBlockSamples, 0.0f)
    , m_downlinkScratch(maxBlockSamples * 2, 0.0f)
    , m_micProbeBuf(maxBlockSamples, 0.0f)
    , m_monitorSum(maxBlockSamples, 0.0f)
{
    for (std::vector<float>& buffer : m_inputRaw)
    {
        buffer.assign(maxBlockSamples, 0.0f);
    }
    for (std::vector<float>& buffer : m_inputProcessed)
    {
        buffer.assign(maxBlockSamples, 0.0f);
    }
    for (std::vector<float>& buffer : m_monitorFeed)
    {
        buffer.assign(maxBlockSamples, 0.0f);
    }
}

void AsioRoutingAdapter::attach()
{
    const std::size_t ramp = std::max<std::size_t>(m_maxBlock, 1);
    m_monitorRamp.configure(ramp);
    m_downlinkRamp.configure(ramp);
    m_outputRamp.configure(ramp);
    m_headphoneRamp.configure(ramp);
    m_micToHeadphonesRamp.configure(ramp);
    m_outputMonoRamp.configure(ramp);
    m_outputMonoRamp.jumpTo(0.0f);
    for (std::size_t slot = 0; slot < routing::kMaxInputChannels; ++slot)
    {
        m_virtualMicSendRamp[slot].configure(ramp);
        m_localMonitorSendRamp[slot].configure(ramp);
        m_virtualMicSendRamp[slot].jumpTo(slot == 0 ? 1.0f : 0.0f);
        m_localMonitorSendRamp[slot].jumpTo(0.0f);
    }
    m_streamFade.configure(std::max<std::size_t>(m_maxBlock * 3, 1));
    for (std::size_t i = 0; i < m_toneTable.size(); ++i)
    {
        m_toneTable[i] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) /
                     static_cast<double>(m_toneTable.size())));
    }

    // The live input-channel count and the ASIO-index -> runtime-slot mapping
    // are fixed by the configured ChannelPlan binding table (AsioChannelMap).
    // This is written on the control thread BEFORE the stream processor is
    // registered below and never mutates while the callback runs.
    m_configuredInputs = 1;
    const ChannelPlan& plan = m_backend.activePlan();
    if (plan.inputCount > 0)
    {
        m_configuredInputs = std::min<std::size_t>(static_cast<std::size_t>(plan.inputCount),
                                                   routing::kMaxInputChannels);
    }

    m_backend.setStreamProcessor(&AsioRoutingAdapter::onAsioCallback, this);
    m_attached = true;
}

void AsioRoutingAdapter::detach()
{
    m_backend.setStreamProcessor(nullptr, nullptr);
    m_attached = false;
    m_synthetic = nullptr;
    m_downlinkTransport = nullptr;
    m_captureTransport = nullptr;
}

void AsioRoutingAdapter::setChannelInputChain(std::size_t channel, routing::RoutingCore::MonoChainFn fn, void* context)
{
    m_routing.setInputChain(channel, fn, context);
}

void AsioRoutingAdapter::setMicChain(routing::RoutingCore::MonoChainFn fn, void* context)
{
    m_routing.setInputChain(0, fn, context);
}

void AsioRoutingAdapter::setOutputChain(routing::RoutingCore::StereoChainFn fn, void* context)
{
    m_routing.setOutputChain(fn, context);
}

void AsioRoutingAdapter::setSourceLatency(BusId bus, std::uint32_t samples)
{
    m_routing.setSourceLatency(bus, samples);
}

void AsioRoutingAdapter::setChannelInputChainLatency(std::size_t channel, routing::LatencyQueryFn fn, void* context)
{
    m_routing.setInputChainLatency(channel, fn, context);
}

void AsioRoutingAdapter::setInputChainLatency(routing::LatencyQueryFn fn, void* context)
{
    m_routing.setInputChainLatency(0, fn, context);
}

void AsioRoutingAdapter::setOutputChainLatency(routing::LatencyQueryFn fn, void* context)
{
    m_routing.setOutputChainLatency(fn, context);
}

void AsioRoutingAdapter::publishRenderConfig(const RenderConfig& config)
{
    m_renderConfig.publish(config);
}

AsioRoutingAdapter::RenderConfig AsioRoutingAdapter::renderConfig() const
{
    return m_renderConfig.read();
}

void AsioRoutingAdapter::publishChannelRuntime(std::size_t channel,
                                                const channel::ChannelRuntimeSnapshot& state)
{
    if (channel < routing::kMaxInputChannels)
    {
        m_channelRuntime[channel].publish(state);
    }
}

channel::ChannelRuntimeSnapshot AsioRoutingAdapter::channelRuntime(std::size_t channel) const
{
    if (channel >= routing::kMaxInputChannels)
    {
        return {};
    }
    return m_channelRuntime[channel].read();
}

void AsioRoutingAdapter::enableSyntheticDownlink(engine::SyntheticDownlink* source)
{
    m_synthetic = source;
}

void AsioRoutingAdapter::setDownlinkTransport(transport::DownlinkTransport* downlink)
{
    m_downlinkTransport = downlink;
}

void AsioRoutingAdapter::setCaptureTransport(transport::VirtualCaptureTransport* capture)
{
    m_captureTransport = capture;
}

void AsioRoutingAdapter::setMicInputTap(SampleTapFn fn, void* context)
{
    m_micInputTap = fn;
    m_micInputTapContext = context;
}

void AsioRoutingAdapter::setMicUplinkTap(SampleTapFn fn, void* context)
{
    m_micUplinkTap = fn;
    m_micUplinkTapContext = context;
}

void AsioRoutingAdapter::configureFade(std::size_t fadeSamples)
{
    m_streamFade.configure(fadeSamples);
}

void AsioRoutingAdapter::armMicInputProbe(const float* burst, std::size_t frames)
{
    if (burst == nullptr || frames == 0 || frames > m_micProbeBuf.size())
    {
        return;
    }
    // Copy the burst into the adapter-owned buffer FIRST, then arm with a
    // release so an acquire-read callback always observes a fully written burst.
    std::memcpy(m_micProbeBuf.data(), burst, frames * sizeof(float));
    m_micProbeFrames = frames;
    std::atomic_signal_fence(std::memory_order_release);
    m_micProbeArmed.store(true, std::memory_order_release);
}

void AsioRoutingAdapter::disarmMicInputProbe()
{
    m_micProbeArmed.store(false, std::memory_order_relaxed);
    m_micProbeFrames = 0;
}

std::uint64_t AsioRoutingAdapter::micInputProbeQpc() const
{
    return m_micProbeQpc.load(std::memory_order_relaxed);
}

void AsioRoutingAdapter::armOutputPairTone(std::uint32_t pair, float amplitudeLinear)
{
    PairTone tone;
    tone.active = true;
    tone.pair = pair > 1 ? 1 : pair;
    tone.amplitude = amplitudeLinear;
    m_pairTone.publish(tone);
}

void AsioRoutingAdapter::disarmOutputPairTone()
{
    m_pairTone.publish(PairTone{});
}

void AsioRoutingAdapter::requestFadeIn()
{
    m_streamFade.requestFadeIn();
    // De-pop startup gate (physical outputs only): hold exact zero for a short
    // priming window, then ramp 0 -> 1 over a fixed sample count. Runs on the
    // control thread BEFORE ASIOStart, so the callback sees a ready gate. The
    // virtual-mic/input/meter path keeps using the stream fade and is untouched.
    m_startupPrimeRemaining = kDePopPrimeSamples; // ~2 ms @ 48 kHz of silence
    m_depopMeasureRemaining = m_maxBlock * 3; // measure first ~3 blocks
    m_depopMeasuredFirst = false;
    m_depopFirstL = 0.0f;
    m_depopFirstR = 0.0f;
    m_depopMaxAbs = 0.0f;
    m_depopMaxJump = 0.0f;
    m_depopPrevL = 0.0f;
    m_depopPrevR = 0.0f;
    m_depopMutedSamples = 0;
    m_depopActive.store(true, std::memory_order_relaxed);
    m_depopPublished.store(false, std::memory_order_relaxed);
    std::printf("[audio-depop] priming %zu samples, then stream-fade ramp %zu samples (maxBlock=%zu)\n",
                kDePopPrimeSamples, m_maxBlock * 3, m_maxBlock);
    // Slice Q1: a fade-in begins a fresh capture epoch. Clear any buffered
    // capture data so the virtual mic never replays pre-restart samples
    // (AGENTS §11). Control plane; the callback is not running yet.
    if (m_captureTransport != nullptr)
    {
        m_captureTransport->reset();
    }
}

AsioRoutingAdapter::DepopSnapshot AsioRoutingAdapter::depopSnapshot() const
{
    DepopSnapshot s;
    s.active = m_depopActive.load(std::memory_order_relaxed);
    s.published = m_depopPublished.load(std::memory_order_acquire);
    s.firstLeft = m_pubFirstL.load(std::memory_order_relaxed);
    s.firstRight = m_pubFirstR.load(std::memory_order_relaxed);
    s.maxAbs = m_pubMaxAbs.load(std::memory_order_relaxed);
    s.maxJump = m_pubMaxJump.load(std::memory_order_relaxed);
    s.mutedSamples = static_cast<std::size_t>(m_pubMuted.load(std::memory_order_relaxed));
    s.rampSamples = m_maxBlock * 3; // stream-fade ramp length used by the adapter
    return s;
}

void AsioRoutingAdapter::requestFadeOut()
{
    m_streamFade.requestFadeOut();
}

bool AsioRoutingAdapter::fadeIsMuted() const
{
    return m_streamFade.isMuted();
}

bool AsioRoutingAdapter::fadeIsFull() const
{
    return m_streamFade.isFull();
}

routing::LatencySnapshot AsioRoutingAdapter::latencySnapshot() const
{
    return m_routing.latencySnapshot();
}

float AsioRoutingAdapter::micUplinkPeak() const
{
    return m_micUplinkPeak.load(std::memory_order_relaxed);
}

engine::MeterSnapshot AsioRoutingAdapter::inputRawMeter(std::size_t channel) const
{
    if (channel >= routing::kMaxInputChannels)
    {
        return {};
    }
    return m_inputRawMeters[channel].snapshot();
}

engine::MeterSnapshot AsioRoutingAdapter::inputPostMeter(std::size_t channel) const
{
    if (channel >= routing::kMaxInputChannels)
    {
        return {};
    }
    return m_inputPostMeters[channel].snapshot();
}

void AsioRoutingAdapter::onAsioCallback(const AsioCallbackInfo& info, void* context)
{
    auto* adapter = static_cast<AsioRoutingAdapter*>(context);
    adapter->processAsio(info);
}

void AsioRoutingAdapter::fillDownlink(std::size_t frames, const float*& left, const float*& right)
{
    if (m_downlinkTransport != nullptr)
    {
        if (m_downlinkTransport->readBlockInterleaved(m_downlinkScratch.data(), frames))
        {
            for (std::size_t i = 0; i < frames; ++i)
            {
                m_stageLeft[i] = m_downlinkScratch[2 * i];
                m_stageRight[i] = m_downlinkScratch[2 * i + 1];
            }
            left = m_stageLeft.data();
            right = m_stageRight.data();
        }
        return; // read failed => both stay null => RoutingCore drops to silence
    }

    if (m_synthetic != nullptr)
    {
        m_synthetic->fill(m_stageLeft.data(), m_stageRight.data(), frames);
        left = m_stageLeft.data();
        right = m_stageRight.data();
    }
}

void AsioRoutingAdapter::zeroSlotSinks(std::size_t channel, std::size_t frames)
{
    if (channel >= routing::kMaxInputChannels)
    {
        return;
    }
    std::fill(m_inputRaw[channel].begin(), m_inputRaw[channel].begin() + frames, 0.0f);
    std::fill(m_inputProcessed[channel].begin(), m_inputProcessed[channel].begin() + frames, 0.0f);
    std::fill(m_monitorFeed[channel].begin(), m_monitorFeed[channel].begin() + frames, 0.0f);
    m_inputRawMeters[channel].clear();
    m_inputPostMeters[channel].clear();
}

void AsioRoutingAdapter::zeroUplink(std::size_t frames)
{
    std::fill(m_micUplink.begin(), m_micUplink.begin() + frames, 0.0f);
}

const std::vector<float>& AsioRoutingAdapter::inputRawBuffer(std::size_t channel) const
{
    return m_inputRaw[std::min<std::size_t>(channel, routing::kMaxInputChannels - 1)];
}

const std::vector<float>& AsioRoutingAdapter::inputProcessedBuffer(std::size_t channel) const
{
    return m_inputProcessed[std::min<std::size_t>(channel, routing::kMaxInputChannels - 1)];
}

const std::vector<float>& AsioRoutingAdapter::inputMonitorFeedBuffer(std::size_t channel) const
{
    return m_monitorFeed[std::min<std::size_t>(channel, routing::kMaxInputChannels - 1)];
}

void AsioRoutingAdapter::processAsio(const AsioCallbackInfo& info)
{
    if (info.sampleCount <= 0)
    {
        return;
    }
    const std::size_t frames = static_cast<std::size_t>(info.sampleCount);

    if (!m_attached || frames > m_maxBlock)
    {
        // Not attached or an overlong driver block: never touch the staging or
        // the routing core (hard no-op for frames > max). Write exact digital
        // silence so an oversized block cannot leak stale driver buffers to the
        // physical outputs (AGENTS §7 "no stale replay").
        if (info.outputs != nullptr)
        {
            for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
            {
                if (info.outputs[channel] != nullptr)
                {
                    std::fill(info.outputs[channel], info.outputs[channel] + frames, 0.0f);
                }
            }
        }
        return;
    }

    // Block-boundary render config application (control thread publishes;
    // the callback reads one torn-free snapshot; gains ramp click-free).
    std::array<channel::ChannelRuntimeSnapshot, routing::kMaxInputChannels> channelStates;
    for (std::size_t slot = 0; slot < routing::kMaxInputChannels; ++slot)
    {
        channelStates[slot] = m_channelRuntime[slot].read();
        if (channelStates[slot].revision != m_appliedChannelRuntime[slot].revision)
        {
            m_virtualMicSendRamp[slot].setTarget(channel::linearGain(channelStates[slot].virtualMicSend));
            m_localMonitorSendRamp[slot].setTarget(channel::linearGain(channelStates[slot].localMonitorSend));
            m_appliedChannelRuntime[slot] = channelStates[slot];
        }
    }
    const RenderConfig config = m_renderConfig.read();
    if (!m_configApplied || config.revision != m_appliedRender.revision)
    {
        // MONITOR bus: mic-monitor feed (monitorGain/monitorMute) + downlink
        // feed (downlinkGain/downlinkMute) -> final MONITOR gain with dim.
        m_monitorRamp.setTarget(config.monitorGain * (config.monitorMute ? 0.0f : 1.0f));
        m_downlinkRamp.setTarget(config.downlinkGain * (config.downlinkMute ? 0.0f : 1.0f));
        const float monitorOutTarget = config.physicalOutputGain *
                                      (config.physicalOutputMute ? 0.0f : 1.0f) *
                                      (config.monitorDim ? config.monitorDimGain : 1.0f);
        m_outputRamp.setTarget(monitorOutTarget);
        // HEADPHONE bus: its own final gain/mute and an INDEPENDENT mic-monitor
        // feed (micToHeadphones), at the same mic-monitor feed level so both
        // buses hear the same processed mic when routed.
        m_headphoneRamp.setTarget(config.headphoneGain * (config.headphoneMute ? 0.0f : 1.0f));
        m_micToHeadphonesRamp.setTarget(config.micToHeadphones ? config.monitorGain : 0.0f);
        m_outputMonoRamp.setTarget(config.outputMono ? 1.0f : 0.0f);
        m_appliedRender = config;
        m_configApplied = true;
    }
    const bool panicMute = config.panicMute;

    const float* downlinkLeft = nullptr;
    const float* downlinkRight = nullptr;
    fillDownlink(frames, downlinkLeft, downlinkRight);

    // Per-slot raw source binding: request slot == runtime slot. A slot with no
    // source this block is silenced (its sinks are zeroed below), never aliased
    // to another slot and never stale-replayed.
    const std::size_t configured = std::min<std::size_t>(m_configuredInputs, routing::kMaxInputChannels);
    const auto sourceForSlot = [&](std::size_t slot) -> const float* {
        if (info.inputs == nullptr || slot >= info.inputChannels)
        {
            return nullptr;
        }
        return info.inputs[slot];
    };

    // Diagnostic calibration: one-shot probe burst replaces the runtime slot 0
    // input for this callback (before the mic tap + RoutingCore). The ASIO
    // input buffers are const, so the effective input becomes the adapter-owned
    // probe buffer that armMicInputProbe copied the burst into. Consumed
    // exactly once (CAS) so it cannot replay on a later block.
    const float* slot0Source = sourceForSlot(0);
    if (m_micProbeArmed.exchange(false) && slot0Source != nullptr && frames == m_micProbeFrames)
    {
        slot0Source = m_micProbeBuf.data();
        LARGE_INTEGER probeQpc;
        QueryPerformanceCounter(&probeQpc);
        m_micProbeQpc.store(static_cast<std::uint64_t>(probeQpc.QuadPart),
                            std::memory_order_relaxed);
    }

    // Diagnostic tap A (realtime-clean; null by default): the (effective)
    // decoded mono ASIO input for runtime slot 0 exactly as it enters the core.
    if (m_micInputTap != nullptr && slot0Source != nullptr)
    {
        m_micInputTap(slot0Source, frames, m_micInputTapContext);
    }

    // Silence any configured slot whose source is absent this block (fresh
    // sinks: no stale replay). The uplink (slot 0 in Phase B) is zeroed too when
    // its source is missing.
    for (std::size_t slot = 0; slot < configured; ++slot)
    {
        if (sourceForSlot(slot) == nullptr)
        {
            zeroSlotSinks(slot, frames);
            if (slot == 0)
            {
                zeroUplink(frames);
            }
        }
    }

    BlockBinding binding;
    binding.frames = frames;
    binding.inputChannels = configured;
    binding.micUplinkChannel = std::min<std::size_t>(0, routing::kMaxInputChannels - 1);
    binding.micUplink = nullptr;
    for (std::size_t slot = 0; slot < configured; ++slot)
    {
        binding.inputSource[slot] = slot == 0 ? slot0Source : sourceForSlot(slot);
        binding.inputRawTap[slot] = m_inputRaw[slot].data();
        binding.inputProcessed[slot] = m_inputProcessed[slot].data();
        binding.inputMonitorFeed[slot] = m_monitorFeed[slot].data();
    }
    binding.playbackLeft = downlinkLeft;
    binding.playbackRight = downlinkRight;
    binding.outputProcessedLeft = m_outProcessedL.data();
    binding.outputProcessedRight = m_outProcessedR.data();
    binding.physicalOutputLeft = m_outScratchL.data();
    binding.physicalOutputRight = m_outScratchR.data();
    m_routing.process(binding);
    {
        const float* srcs[routing::kMaxInputChannels] = {};
        for (std::size_t slot = 0; slot < configured; ++slot)
        {
            srcs[slot] = m_inputProcessed[slot].data();
        }
        routing::CaptureMixer::mixMonoRamped(frames, srcs, m_virtualMicSendRamp.data(), configured,
                                             m_micUplink.data());
    }
    for (std::size_t slot = 0; slot < configured; ++slot)
    {
        if (binding.inputSource[slot] != nullptr)
        {
            m_inputRawMeters[slot].feed(m_inputRaw[slot].data(), frames, 1);
            m_inputPostMeters[slot].feed(m_inputProcessed[slot].data(), frames, 1);
        }
    }

    const bool haveOut0 = info.outputs != nullptr && info.outputChannels > 0 && info.outputs[0] != nullptr;
    const bool haveOut1 = info.outputs != nullptr && info.outputChannels > 1 && info.outputs[1] != nullptr;
    const bool haveOut2 = info.outputs != nullptr && info.outputChannels > 2 && info.outputs[2] != nullptr;
    const bool haveOut3 = info.outputs != nullptr && info.outputChannels > 3 && info.outputs[3] != nullptr;

    const float outputScale = panicMute ? 0.0f : 1.0f;
    const bool dlToHeadphones = config.downlinkToHeadphones;
    float micPeak = 0.0f;
    for (std::size_t i = 0; i < frames; ++i)
    {
        const float micFeedLegacy = m_monitorRamp.next();
        const float dlGain = m_downlinkRamp.next();
        const float monitorOut = m_outputRamp.next();
        const float headphoneOut = m_headphoneRamp.next();
        const float micToHp = m_micToHeadphonesRamp.next();
        const float monoAmt = m_outputMonoRamp.next();
        // De-pop priming: hold the stream fade at exact zero for the first
        // kDePopPrimeSamples, then let it ramp 0 -> 1 as usual. Applied to the
        // whole render envelope (the mic uplink loses only this ~2 ms window).
        float envelope = 0.0f;
        if (m_startupPrimeRemaining > 0)
        {
            --m_startupPrimeRemaining;
            ++m_depopMutedSamples;
            if (m_startupPrimeRemaining == 0)
            {
                m_depopActive.store(false, std::memory_order_relaxed);
            }
        }
        else
        {
            envelope = m_streamFade.next();
        }

        float monitorSum = 0.0f;
        float localNext[routing::kMaxInputChannels] = {};
        for (std::size_t slot = 0; slot < routing::kMaxInputChannels; ++slot)
        {
            localNext[slot] = m_localMonitorSendRamp[slot].next();
        }
        for (std::size_t slot = 0; slot < configured; ++slot)
        {
            if (channelStates[slot].revision == 0)
            {
                if (slot != 0)
                {
                    continue;
                }
                monitorSum += m_monitorFeed[slot][i] * micFeedLegacy;
            }
            else if (localNext[slot] != 0.0f)
            {
                monitorSum += m_monitorFeed[slot][i] * localNext[slot];
            }
        }
        if (config.monitorMute)
        {
            bool anyPublishedActive = false;
            for (std::size_t s = 0; s < configured; ++s)
            {
                if (channelStates[s].revision != 0 && localNext[s] != 0.0f)
                {
                    anyPublishedActive = true;
                    break;
                }
            }
            if (!anyPublishedActive)
            {
                monitorSum = 0.0f;
            }
        }

        const float dlLeft = m_outScratchL[i];
        const float dlRight = m_outScratchR[i];

        float monitorL = monitorSum + dlLeft * dlGain;
        float monitorR = monitorSum + dlRight * dlGain;
        if (monoAmt > 0.001f)
        {
            const float mono = 0.5f * (monitorL + monitorR);
            if (monoAmt >= 0.999f)
            {
                monitorL = mono;
                monitorR = mono;
            }
            else
            {
                monitorL += monoAmt * (mono - monitorL);
                monitorR += monoAmt * (mono - monitorR);
            }
        }
        monitorL = engine::clampToUnit(monitorL);
        monitorR = engine::clampToUnit(monitorR);
        float headphoneL = m_monitorFeed[0][i] * micToHp + (dlToHeadphones ? dlLeft * dlGain : 0.0f);
        float headphoneR = m_monitorFeed[0][i] * micToHp + (dlToHeadphones ? dlRight * dlGain : 0.0f);
        if (monoAmt != 0.0f)
        {
            const float monoHp = 0.5f * (headphoneL + headphoneR);
            headphoneL += monoAmt * (monoHp - headphoneL);
            headphoneR += monoAmt * (monoHp - headphoneR);
        }
        headphoneL = engine::clampToUnit(headphoneL);
        headphoneR = engine::clampToUnit(headphoneR);

        m_micUplink[i] *= envelope;
        const float magnitude = m_micUplink[i] >= 0.0f ? m_micUplink[i] : -m_micUplink[i];
        micPeak = magnitude > micPeak ? magnitude : micPeak;

        const float out0 = monitorL * monitorOut * envelope * outputScale;
        const float out1 = monitorR * monitorOut * envelope * outputScale;
        const float out2 = headphoneL * headphoneOut * envelope * outputScale;
        const float out3 = headphoneR * headphoneOut * envelope * outputScale;
        if (haveOut0)
        {
            info.outputs[0][i] = out0;
        }
        if (haveOut1)
        {
            info.outputs[1][i] = out1;
        }
        if (haveOut2)
        {
            info.outputs[2][i] = out2;
        }
        if (haveOut3)
        {
            info.outputs[3][i] = out3;
        }

        // Bounded startup measurement (diagnostics only).
        if (m_depopMeasureRemaining > 0)
        {
            const float l = haveOut0 ? out0 : 0.0f;
            const float r = haveOut1 ? out1 : 0.0f;
            if (!m_depopMeasuredFirst)
            {
                m_depopFirstL = l;
                m_depopFirstR = r;
                m_depopPrevL = l;
                m_depopPrevR = r;
                m_depopMeasuredFirst = true;
            }
            const float aL = l >= 0.0f ? l : -l;
            const float aR = r >= 0.0f ? r : -r;
            if (aL > m_depopMaxAbs) m_depopMaxAbs = aL;
            if (aR > m_depopMaxAbs) m_depopMaxAbs = aR;
            const float jL = l - m_depopPrevL; const float jLa = jL >= 0.0f ? jL : -jL;
            const float jR = r - m_depopPrevR; const float jRa = jR >= 0.0f ? jR : -jR;
            if (jLa > m_depopMaxJump) m_depopMaxJump = jLa;
            if (jRa > m_depopMaxJump) m_depopMaxJump = jRa;
            m_depopPrevL = l;
            m_depopPrevR = r;
            if (--m_depopMeasureRemaining == 0)
            {
                // Publish once so the control thread can log without racing.
                m_pubFirstL.store(m_depopFirstL, std::memory_order_relaxed);
                m_pubFirstR.store(m_depopFirstR, std::memory_order_relaxed);
                m_pubMaxAbs.store(m_depopMaxAbs, std::memory_order_relaxed);
                m_pubMaxJump.store(m_depopMaxJump, std::memory_order_relaxed);
                m_pubMuted.store(static_cast<unsigned>(m_depopMutedSamples), std::memory_order_relaxed);
                m_depopPublished.store(true, std::memory_order_release);
            }
        }
    }
    m_micUplinkPeak.store(micPeak, std::memory_order_relaxed);

    // Diagnostic output-pair tone (safe channel identification). Overwrites the
    // chosen pair AFTER the normal mix with a LOW-LEVEL tone at a 100 ms on /
    // 100 ms off cadence so its physical destination can be named safely. The
    // other pair keeps its normal (default muted) mix. Never armed by production.
    const PairTone tone = m_pairTone.read();
    const std::uint64_t toneStart = m_toneElapsedSamples;
    m_toneElapsedSamples += frames;
    if (tone.active && tone.amplitude > 0.0f)
    {
        float* const toneLeft = tone.pair == 0 ? (haveOut0 ? info.outputs[0] : nullptr) : (haveOut2 ? info.outputs[2] : nullptr);
        float* const toneRight = tone.pair == 0 ? (haveOut1 ? info.outputs[1] : nullptr) : (haveOut3 ? info.outputs[3] : nullptr);
        for (std::size_t i = 0; i < frames; ++i)
        {
            const std::uint64_t t = toneStart + i;
            const bool toneOn = (t % kToneCadenceSamples) < (kToneCadenceSamples / 2);
            const float sample = toneOn ? tone.amplitude * m_toneTable[t % m_toneTable.size()] : 0.0f;
            if (toneLeft != nullptr)
            {
                toneLeft[i] = sample;
            }
            if (toneRight != nullptr)
            {
                toneRight[i] = sample;
            }
        }
    }

    // Slice Q1: publish the processed mic uplink to the virtual capture
    // transport (SPSC ring write; realtime-clean). The buffer already carries
    // digital silence whenever the uplink source was unavailable, and the
    // stream-fade envelope is applied, so capture clients observe the same
    // start/stop crossfade as the render branch.
    // Diagnostic tap B (realtime-clean; null by default): the exact processed
    // mic uplink being published (post chain, post fade).
    if (m_micUplinkTap != nullptr)
    {
        m_micUplinkTap(m_micUplink.data(), frames, m_micUplinkTapContext);
    }
    if (m_captureTransport != nullptr)
    {
        m_captureTransport->writeProcessedMic(m_micUplink.data(), frames);
    }
}

} // namespace audient::asio
