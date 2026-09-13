#include "transport/VirtualCaptureTransport.h"

#include <stdexcept>

namespace audient::transport
{

VirtualCaptureTransport::VirtualCaptureTransport(Format format, std::size_t capacityFrames)
    : m_format(format)
    , m_ring(capacityFrames)
{
    // Mono processed-mic capture only; the power-of-two capacity requirement is
    // enforced by LockFreeRingBuffer (throws std::invalid_argument).
    if (format.sampleRateHz <= 0 || format.blockSamples <= 0 || format.channels != 1)
    {
        throw std::invalid_argument("VirtualCaptureTransport requires a mono, non-empty stream format");
    }
}

Format VirtualCaptureTransport::format() const
{
    return m_format;
}

std::size_t VirtualCaptureTransport::capacityFrames() const
{
    // channels == 1, so ring floats == frames.
    return m_ring.capacity();
}

std::size_t VirtualCaptureTransport::availableFrames() const
{
    return m_ring.available();
}

bool VirtualCaptureTransport::writeProcessedMic(const float* mono, std::size_t frames)
{
    if (frames == 0 || mono == nullptr)
    {
        return false;
    }
    if (m_ring.writeBlock(mono, frames) == engine::RingBufferStatus::Ok)
    {
        m_producerSeq.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool VirtualCaptureTransport::readProcessedMic(float* mono, std::size_t frames)
{
    if (frames == 0 || mono == nullptr)
    {
        return false;
    }
    if (m_ring.readBlock(mono, frames) == engine::RingBufferStatus::Ok)
    {
        m_consumerSeq.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool VirtualCaptureTransport::readProcessedMicFresh(float* mono, std::size_t frames,
                                                    std::size_t staleThresholdFrames)
{
    // Freshness resynchronization: a backlog larger than the threshold is a
    // stalled backlog. The consumer discards the stale complete blocks and
    // advances its OWN read head to the write frontier (the producer never
    // moves the read head, so this cannot race with or block the producer).
    // After this, only blocks produced afterwards are served.
    const std::size_t buffered = m_ring.available();
    if (buffered > staleThresholdFrames)
    {
        const std::size_t block = static_cast<std::size_t>(m_format.blockSamples);
        const std::size_t toDiscard = (buffered / block) * block; // whole complete blocks
        if (toDiscard > 0 && m_ring.discard(toDiscard))
        {
            m_staleCatchupDrops.fetch_add(toDiscard, std::memory_order_relaxed);
        }
    }
    return readProcessedMic(mono, frames);
}

std::uint64_t VirtualCaptureTransport::producerSequence() const
{
    return m_producerSeq.load(std::memory_order_relaxed);
}

std::uint64_t VirtualCaptureTransport::consumerSequence() const
{
    return m_consumerSeq.load(std::memory_order_relaxed);
}

VirtualCaptureTransport::CaptureStatistics VirtualCaptureTransport::stats() const
{
    CaptureStatistics result;
    const engine::LockFreeRingBuffer::Snapshot ring = m_ring.snapshot();
    // Ring "produced" counts attempts (committed + rejected); committed is the
    // write-head position.
    result.produced = ring.produced - ring.dropped;
    result.consumed = ring.consumed;
    result.overflowDrops = ring.dropped;
    result.underruns = ring.underrunFrames;
    result.staleCatchupDrops = m_staleCatchupDrops.load(std::memory_order_relaxed);
    return result;
}

void VirtualCaptureTransport::reset()
{
    m_ring.reset();
    m_producerSeq.store(0, std::memory_order_relaxed);
    m_consumerSeq.store(0, std::memory_order_relaxed);
    m_staleCatchupDrops.store(0, std::memory_order_relaxed);
}

} // namespace audient::transport