#include "transport/TransportFormat.h"

namespace audient::transport
{

std::string Format::describe() const
{
    std::string result = std::to_string(sampleRateHz) + " Hz / " + std::to_string(channels) + "ch / ";
    result += std::to_string(blockSamples);
    result += sampleType == SampleType::Float32 ? " float32" : " other";
    return result;
}

bool isCompatibleBlock(const Format& format, std::size_t frames)
{
    return frames == static_cast<std::size_t>(format.blockSamples);
}

} // namespace audient::transport