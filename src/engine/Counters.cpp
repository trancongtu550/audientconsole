#include "engine/Counters.h"

namespace audient::engine
{

void EngineCounters::recordCallback()
{
    m_callbacks.fetch_add(1, std::memory_order_relaxed);
}

void EngineCounters::recordXrun()
{
    m_xruns.fetch_add(1, std::memory_order_relaxed);
}

void EngineCounters::recordOverload()
{
    m_overloads.fetch_add(1, std::memory_order_relaxed);
}

void EngineCounters::recordSequenceStep()
{
    m_bufferSequence.fetch_add(1, std::memory_order_relaxed);
}

void EngineCounters::recordSanitized(std::uint64_t count)
{
    m_sanitizedSamples.fetch_add(count, std::memory_order_relaxed);
}

void EngineCounters::reset()
{
    m_callbacks.store(0, std::memory_order_relaxed);
    m_xruns.store(0, std::memory_order_relaxed);
    m_overloads.store(0, std::memory_order_relaxed);
    m_bufferSequence.store(0, std::memory_order_relaxed);
    m_sanitizedSamples.store(0, std::memory_order_relaxed);
}

EngineCounters::Snapshot EngineCounters::snapshot() const
{
    Snapshot result;
    result.callbacks = m_callbacks.load(std::memory_order_relaxed);
    result.xruns = m_xruns.load(std::memory_order_relaxed);
    result.overloads = m_overloads.load(std::memory_order_relaxed);
    result.bufferSequence = m_bufferSequence.load(std::memory_order_relaxed);
    result.sanitizedSamples = m_sanitizedSamples.load(std::memory_order_relaxed);
    return result;
}

void CallbackTimingHistogram::record(double milliseconds)
{
    const double clamped = std::clamp(milliseconds, kMinMs, kMaxMs);
    m_buckets[bucketIndex(clamped)].fetch_add(1, std::memory_order_relaxed);
    m_count.fetch_add(1, std::memory_order_relaxed);
    m_lastMs.store(milliseconds, std::memory_order_relaxed);
    m_sumMs.fetch_add(milliseconds, std::memory_order_relaxed);

    double current = m_maxMs.load(std::memory_order_relaxed);
    while (milliseconds > current && !m_maxMs.compare_exchange_weak(current, milliseconds))
    {
    }
}

std::size_t CallbackTimingHistogram::bucketIndex(double ms) const
{
    const double ratio = ms / kMinMs;
    const double logRatio = std::log2(ratio);
    const double span = std::log2(kMaxMs / kMinMs);
    const double raw = logRatio / span * static_cast<double>(kBucketCount);
    const double clamped = std::clamp(raw, 0.0, static_cast<double>(kBucketCount - 1));
    return static_cast<std::size_t>(clamped);
}

std::uint64_t CallbackTimingHistogram::count() const
{
    return m_count.load(std::memory_order_relaxed);
}

void CallbackTimingHistogram::reset()
{
    for (std::atomic<std::uint64_t>& bucket : m_buckets)
    {
        bucket.store(0, std::memory_order_relaxed);
    }
    m_count.store(0, std::memory_order_relaxed);
    m_lastMs.store(0.0, std::memory_order_relaxed);
    m_maxMs.store(0.0, std::memory_order_relaxed);
    m_sumMs.store(0.0, std::memory_order_relaxed);
}

double CallbackTimingHistogram::lastMs() const
{
    return m_lastMs.load(std::memory_order_relaxed);
}

double CallbackTimingHistogram::maxMs() const
{
    return m_maxMs.load(std::memory_order_relaxed);
}

double CallbackTimingHistogram::meanMs() const
{
    const std::uint64_t total = count();
    if (total == 0)
    {
        return 0.0;
    }
    return m_sumMs.load(std::memory_order_relaxed) / static_cast<double>(total);
}

double CallbackTimingHistogram::percentileMs(double quantile) const
{
    const std::uint64_t total = count();
    if (total == 0)
    {
        return 0.0;
    }

    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i)
    {
        cumulative += m_buckets[i].load(std::memory_order_relaxed);
        if (static_cast<double>(cumulative) / static_cast<double>(total) >= quantile)
        {
            return kMinMs * std::pow(2.0, static_cast<double>(i + 1) / static_cast<double>(kBucketCount) * std::log2(kMaxMs / kMinMs));
        }
    }
    return kMaxMs;
}

std::uint64_t CallbackTimingHistogram::bucketCount(std::size_t index) const
{
    if (index >= kBucketCount)
    {
        return 0;
    }
    return m_buckets[index].load(std::memory_order_relaxed);
}

} // namespace audient::engine