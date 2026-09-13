#include "bench/Bench.h"
#include "core/AllocationTracker.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace core = audient::core;

namespace
{
constexpr std::size_t kBlockSamples = 64;
} // namespace

void runCallbackTimingBench()
{
    std::printf("AudientConsole benchmark: 64-sample callback body timing\n");

    std::array<float, kBlockSamples> input{};
    std::array<float, kBlockSamples> output{};
    for (std::size_t i = 0; i < kBlockSamples; ++i)
    {
        input[i] = static_cast<float>(i) * 1e-3f;
    }

    const bool enabled = core::isAllocationTrackerEnabled() ||
                         core::installAllocationTracker() == core::TrackerState::Installed;

    const audient::bench::TimingResult timing = audient::bench::measure(100000, [&]() {
        std::memcpy(output.data(), input.data(), input.size() * sizeof(float));
    });

    audient::bench::printTiming("copy 64 floats", timing);
    std::printf("allocation tracker enabled: %s\n", enabled ? "yes" : "no");
}