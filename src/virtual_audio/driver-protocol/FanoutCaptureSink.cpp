#include "virtual_audio/driver-protocol/FanoutCaptureSink.h"

#include <algorithm>
#include <limits>

namespace audient::virtual_audio
{

void FanoutCaptureSink::add(IDriverCaptureSink* sink)
{
    if (sink == nullptr)
    {
        return;
    }
    if (m_count >= kMaxSinks)
    {
        m_overflowRegistrations.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_sinks[m_count++] = sink;
}

bool FanoutCaptureSink::connected() const
{
    for (std::size_t i = 0; i < m_count; ++i)
    {
        if (m_sinks[i]->connected())
        {
            return true;
        }
    }
    return false;
}

std::size_t FanoutCaptureSink::capacityFrames() const
{
    std::size_t tightest = std::numeric_limits<std::size_t>::max();
    bool any = false;
    for (std::size_t i = 0; i < m_count; ++i)
    {
        if (!m_sinks[i]->connected())
        {
            continue;
        }
        tightest = std::min(tightest, m_sinks[i]->capacityFrames());
        any = true;
    }
    return any ? tightest : 0u;
}

std::size_t FanoutCaptureSink::availableFrames() const
{
    std::size_t tightest = std::numeric_limits<std::size_t>::max();
    bool any = false;
    for (std::size_t i = 0; i < m_count; ++i)
    {
        if (!m_sinks[i]->connected())
        {
            continue;
        }
        tightest = std::min(tightest, m_sinks[i]->availableFrames());
        any = true;
    }
    return any ? tightest : 0u;
}

bool FanoutCaptureSink::writeMono(const float* mono, std::size_t frames, std::uint64_t generation,
                                  std::uint64_t sequence)
{
    m_writes.fetch_add(1, std::memory_order_relaxed);

    bool anyAccepted = false;
    for (std::size_t i = 0; i < m_count; ++i)
    {
        if (m_sinks[i]->writeMono(mono, frames, generation, sequence))
        {
            m_acceptedBy[i].fetch_add(1, std::memory_order_relaxed);
            anyAccepted = true;
        }
        else
        {
            m_rejectedBy[i].fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (!anyAccepted)
    {
        // No sink accepted (all disconnected or all full). The feeder counts
        // this as a rejection; no sink is ever blocked.
        m_allRejected.fetch_add(1, std::memory_order_relaxed);
    }
    return anyAccepted;
}

FanoutCaptureSink::Snapshot FanoutCaptureSink::snapshot() const
{
    Snapshot result;
    result.sinks = m_count;
    result.fanoutWrites = m_writes.load(std::memory_order_relaxed);
    result.allRejected = m_allRejected.load(std::memory_order_relaxed);
    result.overflowRegistrations = m_overflowRegistrations.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kMaxSinks; ++i)
    {
        result.acceptedBy[i] = m_acceptedBy[i].load(std::memory_order_relaxed);
        result.rejectedBy[i] = m_rejectedBy[i].load(std::memory_order_relaxed);
    }
    return result;
}

} // namespace audient::virtual_audio
