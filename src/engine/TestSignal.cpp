#include "engine/TestSignal.h"

#include <cmath>

namespace audient::engine
{

namespace
{

bool diagnosticBuildEnabled()
{
#if defined(NDEBUG)
    return false;
#else
    return true;
#endif
}

} // namespace

void TestSignalSource::configure(double frequencyHz, double levelDbFs, std::size_t sampleRateHz, double phaseRadians)
{
    m_frequencyHz = frequencyHz;
    m_levelDbFs = levelDbFs;
    m_sampleRateHz = sampleRateHz;
    m_phaseStep = 2.0 * 3.14159265358979323846 * frequencyHz / static_cast<double>(sampleRateHz);
    m_amplitude = std::pow(10.0, levelDbFs / 20.0);
    m_phaseRadians = phaseRadians;
    m_enabled = diagnosticBuildEnabled() && sampleRateHz != 0;
}

bool TestSignalSource::enabled() const
{
    return m_enabled;
}

void TestSignalSource::fillMono(float* out, std::size_t frames)
{
    if (!m_enabled)
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            out[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        out[i] = static_cast<float>(m_amplitude * std::cos(m_phaseRadians));
        m_phaseRadians += m_phaseStep;
    }
}

void TestSignalSource::fillStereo(float* outLeft, float* outRight, std::size_t frames)
{
    if (!m_enabled)
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            outLeft[i] = 0.0f;
            outRight[i] = 0.0f;
        }
        return;
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        const float value = static_cast<float>(m_amplitude * std::cos(m_phaseRadians));
        outLeft[i] = value;
        outRight[i] = value;
        m_phaseRadians += m_phaseStep;
    }
}

double TestSignalSource::currentPhaseRadians() const
{
    return m_phaseRadians;
}

} // namespace audient::engine