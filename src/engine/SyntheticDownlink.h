#pragma once

#include <cstddef>

namespace audient::engine
{

class SyntheticDownlink
{
public:
    void configure(double leftHz, double rightHz, double levelDbFs, std::size_t sampleRateHz,
                   double leftPhaseRadians = 0.0, double rightPhaseRadians = 1.5707963267948966);
    bool enabled() const;
    void fill(float* left, float* right, std::size_t frames);

private:
    double m_leftPhase = 0.0;
    double m_rightPhase = 0.0;
    double m_leftStep = 0.0;
    double m_rightStep = 0.0;
    double m_amplitude = 0.0;
    bool m_enabled = false;
};

} // namespace audient::engine