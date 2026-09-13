#include "pico/PicoVirtualMicSink.h"

namespace audient::pico
{

PicoVirtualMicSink::PicoVirtualMicSink(transport::Format format, std::size_t capacityFrames)
    : m_ring(format, capacityFrames)
{
}

bool PicoVirtualMicSink::connected() const
{
    return m_connected.load(std::memory_order_acquire);
}

std::size_t PicoVirtualMicSink::capacityFrames() const
{
    return m_ring.capacityFrames();
}

std::size_t PicoVirtualMicSink::availableFrames() const
{
    return m_ring.availableFrames();
}

bool PicoVirtualMicSink::writeMono(const float* mono, std::size_t frames, std::uint64_t generation,
                                   std::uint64_t sequence)
{
    if (!m_connected.load(std::memory_order_acquire))
    {
        m_rejectedDisconnected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (mono == nullptr || frames == 0)
    {
        return false;
    }

    // Feeder epoch changed: audio buffered from the previous epoch is stale and
    // must never reach the Pico after a reconnect.
    const std::uint64_t lastGeneration = m_lastGeneration.load(std::memory_order_relaxed);
    if (lastGeneration != generation)
    {
        m_ring.reset();
        m_lastGeneration.store(generation, std::memory_order_relaxed);
        m_generationFlushes.fetch_add(1, std::memory_order_relaxed);
    }

    if (m_ring.writeProcessedMic(mono, frames))
    {
        m_lastAcceptedSequence.store(sequence, std::memory_order_relaxed);
        m_acceptedWrites.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    m_rejectedStall.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void PicoVirtualMicSink::setConnected(bool connected)
{
    const bool previous = m_connected.exchange(connected, std::memory_order_acq_rel);
    if (previous == connected)
    {
        return;
    }
    // Device appeared or disappeared: open a fresh epoch so a reconnect never
    // replays pre-disconnect PCM. Control-plane only (worker thread).
    m_ring.reset();
    m_generationFlushes.fetch_add(1, std::memory_order_relaxed);
}

bool PicoVirtualMicSink::readMono(float* mono, std::size_t frames, std::size_t staleThresholdFrames)
{
    return m_ring.readProcessedMicFresh(mono, frames, staleThresholdFrames);
}

PicoVirtualMicSink::Snapshot PicoVirtualMicSink::snapshot() const
{
    Snapshot result;
    result.lastGeneration = m_lastGeneration.load(std::memory_order_relaxed);
    result.lastAcceptedSequence = m_lastAcceptedSequence.load(std::memory_order_relaxed);
    result.acceptedWrites = m_acceptedWrites.load(std::memory_order_relaxed);
    result.rejectedDisconnected = m_rejectedDisconnected.load(std::memory_order_relaxed);
    result.rejectedStall = m_rejectedStall.load(std::memory_order_relaxed);
    result.generationFlushes = m_generationFlushes.load(std::memory_order_relaxed);

    const transport::VirtualCaptureTransport::CaptureStatistics ring = m_ring.stats();
    result.producedSamples = ring.produced;
    result.consumedSamples = ring.consumed;
    result.staleCatchupDrops = ring.staleCatchupDrops;
    result.underruns = ring.underruns;
    return result;
}

} // namespace audient::pico
