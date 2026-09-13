#include "engine/AudioRamp.h"

namespace audient::engine
{

void AudioRamp::configure(std::size_t rampSamples)
{
    m_total = rampSamples;
    m_position = 0;
    m_start = m_current;
}

void AudioRamp::setTarget(float target)
{
    m_start = m_current;
    m_target = target;
    m_position = 0;
}

void AudioRamp::jumpTo(float target)
{
    m_current = target;
    m_target = target;
    m_start = target;
    m_position = 0;
}

float AudioRamp::next()
{
    if (m_total == 0 || m_position >= m_total)
    {
        m_current = m_target;
        return m_current;
    }

    const float t = static_cast<float>(m_position) / static_cast<float>(m_total);
    m_current = m_start + (m_target - m_start) * t;
    ++m_position;
    return m_current;
}

bool AudioRamp::settled() const
{
    return m_total == 0 || m_position >= m_total;
}

} // namespace audient::engine