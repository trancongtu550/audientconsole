#pragma once

#include <cstddef>

namespace audient::engine
{

class AudioRamp
{
public:
    AudioRamp() = default;

    void configure(std::size_t rampSamples);
    void setTarget(float target);
    void jumpTo(float target);
    float next();
    bool settled() const;

private:
    float m_start = 1.0f;
    float m_target = 1.0f;
    float m_current = 1.0f;
    std::size_t m_total = 0;
    std::size_t m_position = 0;
};

} // namespace audient::engine