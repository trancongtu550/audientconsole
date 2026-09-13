#pragma once

#include <cstddef>
#include <cstdint>

namespace audient::engine
{

struct SanitizeCounters
{
    std::uint64_t nanReplaced = 0;
    std::uint64_t infClamped = 0;
};

float sanitizeValue(float value);
void sanitizeBlock(float* data, std::size_t frames, SanitizeCounters& counters);
void enableFlushToZero();

} // namespace audient::engine