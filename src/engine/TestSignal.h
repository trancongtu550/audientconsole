#pragma once

#include <cstddef>

namespace audient::engine
{

class TestSignalSource
{
public:
    void configure(double frequencyHz, double levelDbFs, std::size_t sampleRateHz, double phaseRadians = 0.0);
    bool enabled() const;
    void fillMono(float* out, std::size_t frames);
    void fillStereo(float* outLeft, float* outRight, std::size_t frames);
    double currentPhaseRadians() const;

private:
    double m_frequencyHz = 1000.0;
    double m_levelDbFs = -18.0;
    std::size_t m_sampleRateHz = 48000;
    double m_phaseRadians = 0.0;
    double m_phaseStep = 0.0;
    double m_amplitude = 0.0;
    bool m_enabled = false;
};

} // namespace audient::engine