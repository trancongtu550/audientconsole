#include "engine/ChannelControls.h"

#include <cmath>

namespace audient::engine
{

void ChannelControls::setRampSamples(std::size_t rampSamples)
{
    m_trimRamp.configure(rampSamples);
    m_muteRamp.configure(rampSamples);
}

void ChannelControls::setTrimDb(float decibels)
{
    const float linear = std::pow(10.0f, decibels / 20.0f);
    m_trimRamp.setTarget(linear);
}

void ChannelControls::setPolarityInvert(bool invert)
{
    m_polarityInvert = invert;
}

void ChannelControls::setMute(bool muted)
{
    m_muteRamp.setTarget(muted ? 0.0f : 1.0f);
}

void ChannelControls::setPanicMute(bool panic)
{
    m_panic = panic;
}

void ChannelControls::applyMonoSample(float& sample)
{
    if (m_panic)
    {
        sample = 0.0f;
        return;
    }
    const float gain = m_trimRamp.next() * m_muteRamp.next();
    sample = m_polarityInvert ? sample * -gain : sample * gain;
}

std::size_t ChannelControls::processMono(float* data, std::size_t frames)
{
    for (std::size_t i = 0; i < frames; ++i)
    {
        applyMonoSample(data[i]);
    }
    return frames;
}

std::size_t ChannelControls::processStereo(float* left, float* right, std::size_t frames)
{
    for (std::size_t i = 0; i < frames; ++i)
    {
        applyMonoSample(left[i]);
        applyMonoSample(right[i]);
    }
    return frames;
}

bool ChannelControls::gainSettled() const
{
    return m_trimRamp.settled() && m_muteRamp.settled();
}

} // namespace audient::engine