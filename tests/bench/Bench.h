#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>

namespace audient::bench
{

struct TimingResult
{
    std::size_t iterations = 0;
    double minMs = 0.0;
    double meanMs = 0.0;
    double maxMs = 0.0;
};

inline TimingResult measure(std::size_t iterations, const std::function<void()>& body)
{
    TimingResult result;
    result.iterations = iterations;
    result.minMs = 1e18;
    result.maxMs = 0.0;

    double total = 0.0;
    for (std::size_t i = 0; i < iterations; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        total += ms;
        result.minMs = std::min(result.minMs, ms);
        result.maxMs = std::max(result.maxMs, ms);
    }
    result.meanMs = total / static_cast<double>(iterations);
    return result;
}

inline void printTiming(const char* name, const TimingResult& result)
{
    std::printf("%-32s iters=%10zu min=%9.5f ms mean=%9.5f ms max=%9.5f ms\n", name,
                result.iterations, result.minMs, result.meanMs, result.maxMs);
}

} // namespace audient::bench