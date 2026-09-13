#include "routing/RoutingCore.h"

#include <algorithm>
#include <cstring>

namespace audient::routing
{

std::size_t channelCount(BusId bus)
{
    switch (bus)
    {
    case BusId::PlaybackBus:
    case BusId::OutputProcessed:
        return 2;
    default:
        return 1;
    }
}

const char* busName(BusId bus)
{
    switch (bus)
    {
    case BusId::PhysicalInput1:
        return "Physical Input 1";
    case BusId::MicUplink:
        return "Mic Uplink";
    case BusId::MicRaw:
        return "Mic Raw";
    case BusId::MicProcessed:
        return "Mic Processed";
    case BusId::PlaybackBus:
        return "Playback Bus";
    case BusId::OutputProcessed:
        return "Output Processed";
    case BusId::PhysicalOutputLeft:
        return "Physical Output 1";
    case BusId::PhysicalOutputRight:
        return "Physical Output 2";
    }
    return "Unknown Bus";
}

RoutingCore::RoutingCore(std::size_t maxBlockSamples)
    : m_maxBlock(maxBlockSamples)
    , m_outputL(maxBlockSamples, 0.0f)
    , m_outputR(maxBlockSamples, 0.0f)
{
    // Per-slot input staging is fixed at construction (kMaxInputChannels x
    // maxBlockSamples). Never allocated, grown, or shrunk on the audio path.
    for (InputChannelStage& stage : m_inputs)
    {
        stage.raw.assign(maxBlockSamples, 0.0f);
        stage.processed.assign(maxBlockSamples, 0.0f);
    }
}

void RoutingCore::setInputChain(MonoChainFn fn, void* context)
{
    setInputChain(0, fn, context);
}

void RoutingCore::setInputChain(std::size_t channel, MonoChainFn fn, void* context)
{
    if (channel < kMaxInputChannels)
    {
        m_inputs[channel].chain = fn;
        m_inputs[channel].chainContext = context;
    }
}

void RoutingCore::setOutputChain(StereoChainFn fn, void* context)
{
    m_outputChain = fn;
    m_outputChainContext = context;
}

void RoutingCore::clearChains()
{
    for (InputChannelStage& stage : m_inputs)
    {
        stage.chain = nullptr;
        stage.chainContext = nullptr;
    }
    m_outputChain = nullptr;
    m_outputChainContext = nullptr;
    m_inputChainLatency = {};
    m_inputChainLatencyContext = {};
    m_outputChainLatency = nullptr;
    m_outputChainLatencyContext = nullptr;
}

void RoutingCore::setSourceLatency(BusId bus, std::uint32_t samples)
{
    switch (bus)
    {
    case BusId::PhysicalInput1:
        m_physicalInputLatency = samples;
        break;
    case BusId::PlaybackBus:
        m_playbackLatency = samples;
        break;
    default:
        break; // only source buses carry a backend-reported source latency
    }
}

void RoutingCore::setInputChainLatency(LatencyQueryFn fn, void* context)
{
    setInputChainLatency(0, fn, context);
}

void RoutingCore::setInputChainLatency(std::size_t channel, LatencyQueryFn fn, void* context)
{
    if (channel < kMaxInputChannels)
    {
        m_inputChainLatency[channel] = fn;
        m_inputChainLatencyContext[channel] = context;
    }
}

void RoutingCore::setOutputChainLatency(LatencyQueryFn fn, void* context)
{
    m_outputChainLatency = fn;
    m_outputChainLatencyContext = context;
}

std::uint32_t RoutingCore::channelChainLatency(std::size_t channel) const
{
    if (channel >= kMaxInputChannels || m_inputChainLatency[channel] == nullptr)
    {
        return 0u;
    }
    return m_inputChainLatency[channel](m_inputChainLatencyContext[channel]);
}

std::uint32_t RoutingCore::inputChannelProcessedLatency(std::size_t channel) const
{
    if (channel >= kMaxInputChannels)
    {
        return 0u;
    }
    return m_physicalInputLatency + channelChainLatency(channel);
}

LatencySnapshot RoutingCore::latencySnapshot() const
{
    LatencySnapshot model;
    const std::uint32_t inputChainLatency = channelChainLatency(0); // v1 uplink channel during Phase B
    const std::uint32_t outputChainLatency =
        m_outputChainLatency != nullptr ? m_outputChainLatency(m_outputChainLatencyContext) : 0u;

    model.physicalInput = m_physicalInputLatency;
    model.micRaw = m_physicalInputLatency;              // wire tap: raw = source
    model.micProcessed = m_physicalInputLatency + inputChainLatency; // after input chain
    model.micUplink = model.micProcessed;               // uplink carries processed mic only

    model.playbackBus = m_playbackLatency;
    model.outputProcessed = m_playbackLatency + outputChainLatency; // after output chain
    model.physicalOutput = model.outputProcessed;       // render = processed downlink
    return model;
}

void RoutingCore::copyMono(const float* src, float* dst, std::size_t frames) const
{
    if (src != nullptr && dst != nullptr && frames > 0)
    {
        std::memcpy(dst, src, frames * sizeof(float));
    }
}

void RoutingCore::copyStereo(const float* ls, const float* rs, float* ld, float* rd, std::size_t frames) const
{
    if (ls != nullptr && ld != nullptr && frames > 0)
    {
        std::memcpy(ld, ls, frames * sizeof(float));
    }
    if (rs != nullptr && rd != nullptr && frames > 0)
    {
        std::memcpy(rd, rs, frames * sizeof(float));
    }
}

void RoutingCore::routeInputChannels(BlockBinding& binding)
{
    const std::size_t frames = binding.frames;
    if (frames == 0 || frames > m_maxBlock)
    {
        return;
    }
    const std::size_t count = std::min(binding.inputChannels, kMaxInputChannels);
    if (count == 0)
    {
        return;
    }

    for (std::size_t ch = 0; ch < count; ++ch)
    {
        const float* const source = binding.inputSource[ch];
        if (source == nullptr)
        {
            continue; // no input this block: the caller zeroed this slot's sinks; never replay stale
        }
        InputChannelStage& stage = m_inputs[ch];

        // MicRaw tap: raw input before the chain (per-slot; copy semantics
        // preserved from the original single-channel implementation).
        copyMono(source, stage.raw.data(), frames);
        if (binding.inputRawTap[ch] != nullptr)
        {
            copyMono(stage.raw.data(), binding.inputRawTap[ch], frames);
        }

        // Materialize the source into the chain staging, then run the slot's
        // input chain IN-PLACE (input == output), mirroring the EngineGraph
        // idiom that Vst3Chain is designed and tested against: an empty/bypassed
        // chain leaves the staging untouched, which equals passthrough.
        copyMono(source, stage.processed.data(), frames);
        if (stage.chain != nullptr)
        {
            stage.chain(stage.processed.data(), stage.processed.data(), frames, stage.chainContext);
        }

        if (binding.inputProcessed[ch] != nullptr)
        {
            copyMono(stage.processed.data(), binding.inputProcessed[ch], frames);
        }

        // Processed-monitor contribution (fed from the processed signal; the
        // backend render branch decides how/where each slot's feed is mixed).
        if (binding.inputMonitorFeed[ch] != nullptr)
        {
            copyMono(stage.processed.data(), binding.inputMonitorFeed[ch], frames);
        }

        // Uplink: processed-only (never raw) of the SELECTED uplink channel.
        if (ch == binding.micUplinkChannel && binding.micUplink != nullptr)
        {
            copyMono(stage.processed.data(), binding.micUplink, frames);
        }
    }
}

void RoutingCore::routeDownlinkPath(BlockBinding& binding)
{
    const std::size_t frames = binding.frames;
    if (frames == 0 || frames > m_maxBlock)
    {
        return;
    }
    if (binding.playbackLeft == nullptr && binding.playbackRight == nullptr)
    {
        // No downlink source: clean drop. Never replay stale staging as if it
        // were the current block's downlink (AGENTS §7 "never replay stale/
        // uninitialized samples").
        std::fill(m_outputL.begin(), m_outputL.begin() + frames, 0.0f);
        std::fill(m_outputR.begin(), m_outputR.begin() + frames, 0.0f);
    }
    else
    {
        // Materialize the source into the output staging first (a missing side
        // is zeroed, never stale), then run the output chain IN-PLACE, matching
        // the EngineGraph idiom: an empty/bypassed chain leaves the staging
        // untouched, which equals passthrough.
        copyStereo(binding.playbackLeft, binding.playbackRight, m_outputL.data(), m_outputR.data(), frames);
        if (binding.playbackLeft == nullptr)
        {
            std::fill(m_outputL.begin(), m_outputL.begin() + frames, 0.0f);
        }
        if (binding.playbackRight == nullptr)
        {
            std::fill(m_outputR.begin(), m_outputR.begin() + frames, 0.0f);
        }
        if (m_outputChain != nullptr)
        {
            m_outputChain(m_outputL.data(), m_outputR.data(), m_outputL.data(), m_outputR.data(), frames,
                          m_outputChainContext);
        }
    }
    if (binding.outputProcessedLeft != nullptr)
    {
        copyMono(m_outputL.data(), binding.outputProcessedLeft, frames);
    }
    if (binding.outputProcessedRight != nullptr)
    {
        copyMono(m_outputR.data(), binding.outputProcessedRight, frames);
    }

    // Physical render: processed downlink to Output 1/2.
    if (binding.physicalOutputLeft != nullptr)
    {
        copyMono(m_outputL.data(), binding.physicalOutputLeft, frames);
    }
    if (binding.physicalOutputRight != nullptr)
    {
        copyMono(m_outputR.data(), binding.physicalOutputRight, frames);
    }
}

void RoutingCore::process(BlockBinding& binding)
{
    routeInputChannels(binding);
    routeDownlinkPath(binding);
}

} // namespace audient::routing
