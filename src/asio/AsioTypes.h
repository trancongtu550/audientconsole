#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audient::asio
{

enum class SampleFormat
{
    Int16LE,
    Int24LE,
    Int32LE,
    Float32LE,
    Float64LE,
};

struct DriverIdentity
{
    std::string clsid;
    std::string name;
};

struct BufferSizeInfo
{
    long minimum = 0;
    long maximum = 0;
    long preferred = 0;
    long granularity = 0;
};

struct LatencyInfo
{
    long input = 0;
    long output = 0;
};

struct ChannelInfo
{
    long index = 0;
    bool isInput = false;
    bool isActive = false;
    std::string name;
    SampleFormat preferredFormat = SampleFormat::Float32LE;
};

struct DriverCapabilities
{
    long inputChannels = 0;
    long outputChannels = 0;
    std::vector<long> supportedSampleRates;
    std::vector<ChannelInfo> channels;
    BufferSizeInfo bufferSizes;
    LatencyInfo latencies;

    bool supportsSampleRate(long sampleRate) const;
};

struct AsioCallbackInfo
{
    long sampleCount = 0;
    std::size_t inputChannels = 0;
    std::size_t outputChannels = 0;
    const float* const* inputs = nullptr;
    float* const* outputs = nullptr;
    std::int64_t samplePosition = 0;
    double nanoSeconds = 0.0;
};

using AsioCallbackFn = void (*)(const AsioCallbackInfo&, void* context);

} // namespace audient::asio