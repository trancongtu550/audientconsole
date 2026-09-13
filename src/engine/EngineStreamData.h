#pragma once

#include <cstddef>

namespace audient::engine
{

struct EngineStreamData
{
    long frames = 0;
    const float* physicalInputMono = nullptr;
    float* physicalOutputLeft = nullptr;
    float* physicalOutputRight = nullptr;
    float* micUplinkMono = nullptr;
    const float* systemDownlinkLeft = nullptr;
    const float* systemDownlinkRight = nullptr;
};

} // namespace audient::engine