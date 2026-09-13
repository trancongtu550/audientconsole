#include "engine/EngineGraph.h"

#include <algorithm>
#include <cmath>
#include <cassert>

namespace audient::engine
{

float clampToUnit(float value)
{
    if (value > 1.0f)
    {
        return 1.0f;
    }
    if (value < -1.0f)
    {
        return -1.0f;
    }
    return value;
}

namespace
{
float linear(float decibels)
{
    return std::pow(10.0f, decibels / 20.0f);
}
} // namespace

EngineGraph::EngineGraph(std::size_t maxBlockSamples)
    : m_maxBlock(maxBlockSamples)
{
    assert(maxBlockSamples > 0);
    const std::size_t ramp = std::max<std::size_t>(maxBlockSamples, 1);
    m_inputGain.configure(ramp);
    m_micSendGain.configure(ramp);
    m_micMonitorGain.configure(ramp);
    m_downlinkGain.configure(ramp);
    m_physicalOutputGain.configure(ramp);

    m_micProcessed.resize(maxBlockSamples, 0.0f);
    m_monitorBuffer.resize(maxBlockSamples, 0.0f);
    m_outLeft.resize(maxBlockSamples, 0.0f);
    m_outRight.resize(maxBlockSamples, 0.0f);
}

std::size_t EngineGraph::maxBlockSamples() const
{
    return m_maxBlock;
}

void EngineGraph::publishConfig(const EngineConfig& config)
{
    m_config.publish(config);
}

EngineConfig EngineGraph::configSnapshot() const
{
    return m_config.read();
}

void EngineGraph::setMicChain(ChainProcess process, void* context)
{
    m_micChain = process;
    m_micChainContext = context;
}

void EngineGraph::setOutputChain(StereoChainProcess process, void* context)
{
    m_outputChain = process;
    m_outputChainContext = context;
}

void EngineGraph::applyConfig(const EngineConfig& config)
{
    m_inputGain.setTarget(linear(config.inputTrimDb));
    m_micSendGain.setTarget(linear(config.micSendDb) * (config.micSendMute ? 0.0f : 1.0f));
    m_micMonitorGain.setTarget(linear(config.micMonitorDb) * (config.micMonitorMute ? 0.0f : 1.0f));
    m_downlinkGain.setTarget(linear(config.downlinkDb) * (config.downlinkMute ? 0.0f : 1.0f));
    m_physicalOutputGain.setTarget(linear(config.physicalOutputDb) * (config.physicalOutputMute ? 0.0f : 1.0f));
    m_polarityInvert = config.polarityInvert;
    m_panicMute = config.panicMute;
    m_appliedConfig = config;
    m_applied = true;
}

void EngineGraph::process(EngineStreamData& data)
{
    if (data.frames <= 0)
    {
        return;
    }
    const std::size_t frames = static_cast<std::size_t>(data.frames);
    if (frames > m_maxBlock)
    {
        return;
    }

    const EngineConfig config = m_config.read();
    if (!m_applied || config.revision != m_appliedConfig.revision)
    {
        applyConfig(config);
    }

    const bool haveInput = data.physicalInputMono != nullptr;

    for (std::size_t i = 0; i < frames; ++i)
    {
        float sample = haveInput ? data.physicalInputMono[i] : 0.0f;
        sample *= m_inputGain.next();
        if (m_polarityInvert)
        {
            sample = -sample;
        }
        m_micProcessed[i] = sample;
    }

    if (m_micChain != nullptr)
    {
        m_micChain(m_micProcessed.data(), m_micProcessed.data(), frames, m_micChainContext);
    }

    if (data.micUplinkMono != nullptr)
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            data.micUplinkMono[i] = m_micProcessed[i] * m_micSendGain.next();
        }
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        m_monitorBuffer[i] = m_micProcessed[i] * m_micMonitorGain.next();
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        m_outLeft[i] = (data.systemDownlinkLeft != nullptr ? data.systemDownlinkLeft[i] : 0.0f) * m_downlinkGain.next();
        m_outRight[i] = (data.systemDownlinkRight != nullptr ? data.systemDownlinkRight[i] : 0.0f) * m_downlinkGain.next();
    }

    if (m_outputChain != nullptr)
    {
        m_outputChain(m_outLeft.data(), m_outRight.data(), m_outLeft.data(), m_outRight.data(), frames, m_outputChainContext);
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        float left = clampToUnit(m_outLeft[i] + m_monitorBuffer[i]);
        float right = clampToUnit(m_outRight[i] + m_monitorBuffer[i]);

        const float outGain = m_physicalOutputGain.next() * (m_panicMute ? 0.0f : 1.0f);
        if (data.physicalOutputLeft != nullptr)
        {
            data.physicalOutputLeft[i] = clampToUnit(left * outGain);
        }
        if (data.physicalOutputRight != nullptr)
        {
            data.physicalOutputRight[i] = clampToUnit(right * outGain);
        }
    }

    m_processedBlocks.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t EngineGraph::processedBlocks() const
{
    return m_processedBlocks.load(std::memory_order_relaxed);
}

} // namespace audient::engine