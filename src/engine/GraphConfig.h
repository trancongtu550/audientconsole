#pragma once

#include <cstdint>

namespace audient::engine
{

struct EngineConfig
{
    std::uint32_t sampleRateHz = 48000;
    std::uint32_t maxBlockSamples = 64;
    float inputTrimDb = 0.0f;
    float micSendDb = 0.0f;
    float micMonitorDb = -6.0f;
    float downlinkDb = 0.0f;
    float physicalOutputDb = 0.0f;
    float outputHeadroomDb = 3.0f;
    bool polarityInvert = false;
    bool micSendMute = false;
    bool micMonitorMute = false;
    bool downlinkMute = false;
    bool physicalOutputMute = false;
    bool panicMute = false;
    std::uint64_t revision = 0;
};

} // namespace audient::engine