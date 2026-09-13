#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace audient::engine
{

class EngineCounters
{
public:
    struct Snapshot
    {
        std::uint64_t callbacks = 0;
        std::uint64_t xruns = 0;
        std::uint64_t overloads = 0;
        std::uint64_t bufferSequence = 0;
        std::uint64_t sanitizedSamples = 0;
    };

    void recordCallback();
    void recordXrun();
    void recordOverload();
    void recordSequenceStep();
    void recordSanitized(std::uint64_t count);
    void reset();
    Snapshot snapshot() const;

private:
    alignas(64) std::atomic<std::uint64_t> m_callbacks{0};
    std::atomic<std::uint64_t> m_xruns{0};
    std::atomic<std::uint64_t> m_overloads{0};
    std::atomic<std::uint64_t> m_bufferSequence{0};
    std::atomic<std::uint64_t> m_sanitizedSamples{0};
};

class CallbackTimingHistogram
{
public:
    static constexpr std::size_t kBucketCount = 64;
    static constexpr double kMinMs = 0.005;
    static constexpr double kMaxMs = 512.0;

    void record(double milliseconds);
    void reset();
    std::uint64_t count() const;
    double lastMs() const;
    double maxMs() const;
    double meanMs() const;
    double percentileMs(double quantile) const;
    std::uint64_t bucketCount(std::size_t index) const;

private:
    std::size_t bucketIndex(double ms) const;

    std::array<std::atomic<std::uint64_t>, kBucketCount> m_buckets{};
    std::atomic<std::uint64_t> m_count{0};
    std::atomic<double> m_lastMs{0.0};
    std::atomic<double> m_maxMs{0.0};
    std::atomic<double> m_sumMs{0.0};
};

} // namespace audient::engine