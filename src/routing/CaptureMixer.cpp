#include "routing/CaptureMixer.h"

#include <cassert>

namespace audient::routing
{

void CaptureMixer::mixMono(std::size_t frames, const float* const sources[kCapacity],
                           const float gains[kCapacity], std::size_t activeChannels, float* dest)
{
    if (dest == nullptr || frames == 0 || activeChannels == 0)
    {
        if (dest != nullptr && frames > 0)
        {
            for (std::size_t i = 0; i < frames; ++i)
            {
                dest[i] = 0.0f;
            }
        }
        return;
    }
    if (activeChannels > kCapacity)
    {
        activeChannels = kCapacity;
    }
    for (std::size_t i = 0; i < frames; ++i)
    {
        float sum = 0.0f;
        for (std::size_t ch = 0; ch < activeChannels; ++ch)
        {
            const float* src = sources[ch];
            if (src != nullptr)
            {
                sum += src[i] * gains[ch];
            }
        }
        dest[i] = sum;
    }
}

void CaptureMixer::mixMonoRamped(std::size_t frames, const float* const sources[kCapacity],
                                 engine::AudioRamp* gainRamps, std::size_t activeChannels, float* dest)
{
    assert(gainRamps != nullptr);
    if (dest == nullptr || frames == 0 || activeChannels == 0)
    {
        if (dest != nullptr && frames > 0)
        {
            for (std::size_t i = 0; i < frames; ++i)
            {
                dest[i] = 0.0f;
            }
        }
        return;
    }
    if (activeChannels > kCapacity)
    {
        activeChannels = kCapacity;
    }
    for (std::size_t i = 0; i < frames; ++i)
    {
        float sum = 0.0f;
        for (std::size_t ch = 0; ch < activeChannels; ++ch)
        {
            const float* src = sources[ch];
            const float g = gainRamps[ch].next();
            if (src != nullptr)
            {
                sum += src[i] * g;
            }
        }
        dest[i] = sum;
    }
}

} // namespace audient::routing
