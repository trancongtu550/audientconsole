#pragma once

#include "engine/AudioRamp.h"
#include "routing/RoutingTypes.h"

#include <cstddef>

namespace audient::routing
{

class CaptureMixer
{
public:
    static constexpr std::size_t kCapacity = kMaxInputChannels;

    static void mixMono(std::size_t frames, const float* const sources[kCapacity],
                        const float gains[kCapacity], std::size_t activeChannels, float* dest);

    static void mixMonoRamped(std::size_t frames, const float* const sources[kCapacity],
                              engine::AudioRamp* gainRamps, std::size_t activeChannels, float* dest);
};

} // namespace audient::routing
