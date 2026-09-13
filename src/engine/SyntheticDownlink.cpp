#include "engine/SyntheticDownlink.h"

#include <cmath>

namespace audient::engine
{

namespace
{

bool diagnosticsEnabled()
{
#if defined(NDEBUG)
    return false;
#else
    return true;
#endif
}

} // namespace

void SyntheticDownlink::configure(double leftHz, double rightHz, double levelDbFs, std::size_t sampleRateHz,
                                  double leftPhaseRadians, double rightPhaseRadians)
{
    const double twoPi = 2.0 * 3.14159265358979323846;
    m_leftStep = twoPi * leftHz / static_cast<double>(sampleRateHz);
    m_rightStep = twoPi * rightHz / static_cast<double>(sampleRateHz);
    m_leftPhase = leftPhaseRadians;
    m_rightPhase = rightPhaseRadians;
    m_amplitude = std::pow(10.0, levelDbFs / 20.0);
    m_enabled = diagnosticsEnabled() && sampleRateHz != 0;
}

bool SyntheticDownlink::enabled() const
{
    return m_enabled;
}

void SyntheticDownlink::fill(float* left, float* right, std::size_t frames)
{
    if (!m_enabled)
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            left[i] = 0.0f;
            right[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        left[i] = static_cast<float>(m_amplitude * std::cos(m_leftPhase));
        right[i] = static_cast<float>(m_amplitude * std::cos(m_rightPhase));
        m_leftPhase += m_leftStep;
        m_rightPhase += m_rightStep;
    }
}

} // namespace audient::engine