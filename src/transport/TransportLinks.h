#pragma once

#include "engine/RingBuffer.h"
#include "transport/TransportFormat.h"

#include <cstddef>
#include <cstdint>

namespace audient::transport
{

class UplinkTransport
{
public:
    UplinkTransport(Format format, std::size_t capacityFrames);

    Format format() const;
    bool writeBlock(const float* data, std::size_t frames);
    bool readBlockInterleaved(float* data, std::size_t frames);
    std::size_t capacityFrames() const;
    std::size_t availableFrames() const;

    engine::LockFreeRingBuffer::Snapshot stats() const;
    void reset();

private:
    Format m_format;
    engine::LockFreeRingBuffer m_ring;
};

class DownlinkTransport
{
public:
    DownlinkTransport(Format format, std::size_t capacityFrames);

    Format format() const;
    bool writeBlock(const float* data, std::size_t frames);
    bool readBlockInterleaved(float* data, std::size_t frames);
    std::size_t capacityFrames() const;
    std::size_t availableFrames() const;

    engine::LockFreeRingBuffer::Snapshot stats() const;
    void reset();

private:
    std::size_t samplesPerFrame() const;
    std::size_t floatsPerBlock(std::size_t frames) const;

    Format m_format;
    engine::LockFreeRingBuffer m_ring;
};

} // namespace audient::transport