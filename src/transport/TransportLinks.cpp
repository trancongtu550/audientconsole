#include "transport/TransportLinks.h"

#include <cassert>
#include <cstddef>
#include <stdexcept>

namespace audient::transport
{

namespace
{
void requireValidFormat(const Format& format)
{
    if (format.sampleRateHz <= 0 || format.blockSamples <= 0 || (format.channels != 1 && format.channels != 2))
    {
        throw std::invalid_argument("invalid transport format");
    }
}
} // namespace

UplinkTransport::UplinkTransport(Format format, std::size_t capacityFrames)
    : m_format(format)
    , m_ring(capacityFrames * static_cast<std::size_t>(format.channels))
{
    requireValidFormat(format);
}

Format UplinkTransport::format() const
{
    return m_format;
}

bool UplinkTransport::writeBlock(const float* data, std::size_t frames)
{
    if (frames == 0)
    {
        return false;
    }
    const std::size_t floats = frames * static_cast<std::size_t>(m_format.channels);
    return m_ring.writeBlock(data, floats) == engine::RingBufferStatus::Ok;
}

std::size_t UplinkTransport::capacityFrames() const
{
    return m_ring.capacity() / static_cast<std::size_t>(m_format.channels);
}

std::size_t UplinkTransport::availableFrames() const
{
    return m_ring.available() / static_cast<std::size_t>(m_format.channels);
}

bool UplinkTransport::readBlockInterleaved(float* data, std::size_t frames)
{
    if (frames == 0)
    {
        return false;
    }
    return m_ring.readBlock(data, frames * static_cast<std::size_t>(m_format.channels)) == engine::RingBufferStatus::Ok;
}

engine::LockFreeRingBuffer::Snapshot UplinkTransport::stats() const
{
    return m_ring.snapshot();
}

void UplinkTransport::reset()
{
    m_ring.reset();
}

DownlinkTransport::DownlinkTransport(Format format, std::size_t capacityFrames)
    : m_format(format)
    , m_ring(capacityFrames * static_cast<std::size_t>(format.channels))
{
    requireValidFormat(format);
}

Format DownlinkTransport::format() const
{
    return m_format;
}

std::size_t DownlinkTransport::samplesPerFrame() const
{
    return static_cast<std::size_t>(m_format.channels);
}

std::size_t DownlinkTransport::floatsPerBlock(std::size_t frames) const
{
    return frames * samplesPerFrame();
}

bool DownlinkTransport::writeBlock(const float* data, std::size_t frames)
{
    if (frames == 0)
    {
        return false;
    }
    return m_ring.writeBlock(data, floatsPerBlock(frames)) == engine::RingBufferStatus::Ok;
}

bool DownlinkTransport::readBlockInterleaved(float* data, std::size_t frames)
{
    if (frames == 0)
    {
        return false;
    }
    return m_ring.readBlock(data, floatsPerBlock(frames)) == engine::RingBufferStatus::Ok;
}

std::size_t DownlinkTransport::capacityFrames() const
{
    return m_ring.capacity() / samplesPerFrame();
}

std::size_t DownlinkTransport::availableFrames() const
{
    return m_ring.available() / samplesPerFrame();
}

engine::LockFreeRingBuffer::Snapshot DownlinkTransport::stats() const
{
    return m_ring.snapshot();
}

void DownlinkTransport::reset()
{
    m_ring.reset();
}

} // namespace audient::transport