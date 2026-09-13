#include "asio/AsioStreamBridge.h"

#include <algorithm>
#include <cassert>

namespace audient::asio
{

AsioStreamBridge::AsioStreamBridge(AsioBackend& backend)
    : m_backend(backend)
{
}

void AsioStreamBridge::attach(engine::EngineGraph& graph, std::size_t maxBlockSamples)
{
    m_graph = &graph;
    m_micUplink.assign(maxBlockSamples, 0.0f);
    m_stageLeft.assign(maxBlockSamples, 0.0f);
    m_stageRight.assign(maxBlockSamples, 0.0f);
    m_downlinkScratch.assign(maxBlockSamples * 2, 0.0f);
    m_streamFade.configure(std::max<std::size_t>(maxBlockSamples * 3, 1));
    m_backend.setStreamProcessor(&AsioStreamBridge::onAsioCallback, this);
}

void AsioStreamBridge::enableSyntheticDownlink(engine::SyntheticDownlink* source)
{
    m_synthetic = source;
}

void AsioStreamBridge::setUplinkTransport(transport::UplinkTransport* uplink)
{
    m_uplinkTransport = uplink;
}

void AsioStreamBridge::setDownlinkTransport(transport::DownlinkTransport* downlink)
{
    m_downlinkTransport = downlink;
}

void AsioStreamBridge::detach()
{
    m_backend.setStreamProcessor(nullptr, nullptr);
    m_graph = nullptr;
    m_synthetic = nullptr;
    m_uplinkTransport = nullptr;
    m_downlinkTransport = nullptr;
}

void AsioStreamBridge::configureStreamFade(std::size_t fadeSamples)
{
    m_streamFade.configure(fadeSamples);
}

void AsioStreamBridge::requestFadeIn()
{
    m_streamFade.requestFadeIn();
}

void AsioStreamBridge::requestFadeOut()
{
    m_streamFade.requestFadeOut();
}

bool AsioStreamBridge::fadeIsMuted() const
{
    return m_streamFade.isMuted();
}

bool AsioStreamBridge::fadeIsFull() const
{
    return m_streamFade.isFull();
}

const std::vector<float>& AsioStreamBridge::micUplinkBuffer() const
{
    return m_micUplink;
}

float AsioStreamBridge::micUplinkPeak() const
{
    return m_micUplinkPeak.load(std::memory_order_relaxed);
}

void AsioStreamBridge::onAsioCallback(const AsioCallbackInfo& info, void* context)
{
    auto* bridge = static_cast<AsioStreamBridge*>(context);
    bridge->handle(info);
}

void AsioStreamBridge::fillDownlink(std::size_t frames, const float*& left, const float*& right)
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
        return;
    }

    if (m_synthetic != nullptr)
    {
        m_synthetic->fill(m_stageLeft.data(), m_stageRight.data(), frames);
        left = m_stageLeft.data();
        right = m_stageRight.data();
    }
}

void AsioStreamBridge::handle(const AsioCallbackInfo& info)
{
    if (m_graph == nullptr)
    {
        if (info.outputs != nullptr)
        {
            for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
            {
                std::fill(info.outputs[channel], info.outputs[channel] + info.sampleCount, 0.0f);
            }
        }
        return;
    }

    const std::size_t frames = static_cast<std::size_t>(info.sampleCount);
    assert(frames <= m_micUplink.size());

    const float* downlinkLeft = nullptr;
    const float* downlinkRight = nullptr;
    fillDownlink(frames, downlinkLeft, downlinkRight);

    engine::EngineStreamData data{};
    data.frames = info.sampleCount;
    data.physicalInputMono = info.inputs != nullptr && info.inputChannels > 0 ? info.inputs[0] : nullptr;
    data.physicalOutputLeft = info.outputs != nullptr && info.outputChannels > 0 ? info.outputs[0] : nullptr;
    data.physicalOutputRight = info.outputs != nullptr && info.outputChannels > 1 ? info.outputs[1] : nullptr;
    data.micUplinkMono = m_micUplink.data();
    data.systemDownlinkLeft = downlinkLeft;
    data.systemDownlinkRight = downlinkRight;

m_graph->process(data);

    for (std::size_t i = 0; i < frames; ++i)
    {
        const float envelope = m_streamFade.next();
        if (info.outputs != nullptr && info.outputChannels > 0)
        {
            info.outputs[0][i] *= envelope;
        }
        if (info.outputs != nullptr && info.outputChannels > 1)
        {
            info.outputs[1][i] *= envelope;
        }
        m_micUplink[i] *= envelope;
    }

    float micPeak = 0.0f;
    for (std::size_t i = 0; i < frames; ++i)
    {
        const float magnitude = m_micUplink[i] >= 0.0f ? m_micUplink[i] : -m_micUplink[i];
        micPeak = magnitude > micPeak ? magnitude : micPeak;
    }
    m_micUplinkPeak.store(micPeak, std::memory_order_relaxed);

    if (m_uplinkTransport != nullptr)
    {
        m_uplinkTransport->writeBlock(m_micUplink.data(), frames);
    }
}

} // namespace audient::asio