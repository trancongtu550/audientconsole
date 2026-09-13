#pragma once

#include <string>

namespace audient::transport
{

enum class SampleType
{
    Float32,
};

struct Format
{
    int sampleRateHz = 48000;
    int channels = 1;
    int blockSamples = 64;
    SampleType sampleType = SampleType::Float32;

    bool operator==(const Format&) const = default;

    std::string describe() const;
};

bool isCompatibleBlock(const Format& format, std::size_t frames);

} // namespace audient::transport