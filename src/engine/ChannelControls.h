#pragma once

#include "engine/AudioRamp.h"

#include <cstddef>

namespace audient::engine
{

class ChannelControls
{
public:
    void setRampSamples(std::size_t rampSamples);

    void setTrimDb(float decibels);
    void setPolarityInvert(bool invert);
    void setMute(bool muted);
    void setPanicMute(bool panic);

    std::size_t processMono(float* data, std::size_t frames);
    std::size_t processStereo(float* left, float* right, std::size_t frames);

    bool gainSettled() const;

private:
    void applyMonoSample(float& sample);

    AudioRamp m_trimRamp;
    AudioRamp m_muteRamp;
    bool m_polarityInvert = false;
    bool m_panic = false;
};

} // namespace audient::engine