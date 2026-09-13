#include "engine/RingBuffer.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace audient::engine
{

LockFreeRingBuffer::LockFreeRingBuffer(std::size_t capacityFrames)
    : m_capacity(capacityFrames)
{
    if (capacityFrames == 0 || (capacityFrames & (capacityFrames - 1)) != 0)
    {
        throw std::invalid_argument("LockFreeRingBuffer capacity must be a nonzero power of two");
    }
    m_mask = m_capacity - 1;
    m_memory = std::make_unique<float[]>(m_capacity);
}

RingBufferStatus LockFreeRingBuffer::writeBlock(const float* frames, std::size_t count)
{
    if (count == 0)
    {
        return RingBufferStatus::Ok;
    }
    if (count > m_capacity)
    {
        m_dropped.fetch_add(count, std::memory_order_relaxed);
        return RingBufferStatus::Full;
    }

    const std::uint64_t read = m_readPos.load(std::memory_order_acquire);
    const std::uint64_t write = m_writePos.load(std::memory_order_relaxed);

    if (write + count > read + m_capacity)
    {
        m_dropped.fetch_add(count, std::memory_order_relaxed);
        return RingBufferStatus::Full;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t index = static_cast<std::size_t>((write + i) & m_mask);
        m_memory[index] = frames[i];
    }

    m_writePos.fetch_add(count, std::memory_order_release);
    return RingBufferStatus::Ok;
}

RingBufferStatus LockFreeRingBuffer::readBlock(float* frames, std::size_t count)
{
    if (count == 0)
    {
        return RingBufferStatus::Ok;
    }

    const std::uint64_t write = m_writePos.load(std::memory_order_acquire);
    const std::uint64_t read = m_readPos.load(std::memory_order_relaxed);

    if (read + count > write)
    {
        const std::uint64_t underrun = read + count - write;
        m_underrunFrames.fetch_add(underrun, std::memory_order_relaxed);
        return RingBufferStatus::Empty;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t index = static_cast<std::size_t>((read + i) & m_mask);
        frames[i] = m_memory[index];
    }

    m_readPos.fetch_add(count, std::memory_order_release);
    return RingBufferStatus::Ok;
}

bool LockFreeRingBuffer::discard(std::size_t count)
{
    if (count == 0)
    {
        return true;
    }

    const std::uint64_t write = m_writePos.load(std::memory_order_acquire);
    const std::uint64_t read = m_readPos.load(std::memory_order_relaxed);
    if (read + count > write)
    {
        return false;
    }

    m_readPos.fetch_add(count, std::memory_order_release);
    return true;
}

std::size_t LockFreeRingBuffer::capacity() const
{
    return m_capacity;
}

std::size_t LockFreeRingBuffer::available() const
{
    const std::uint64_t write = m_writePos.load(std::memory_order_acquire);
    const std::uint64_t read = m_readPos.load(std::memory_order_relaxed);
    const std::uint64_t delta = write - read;
    return static_cast<std::size_t>(std::min<std::uint64_t>(delta, m_capacity));
}

LockFreeRingBuffer::Snapshot LockFreeRingBuffer::snapshot() const
{
    Snapshot result;
    result.produced = m_writePos.load(std::memory_order_relaxed) + m_dropped.load(std::memory_order_relaxed);
    result.consumed = m_readPos.load(std::memory_order_relaxed);
    result.dropped = m_dropped.load(std::memory_order_relaxed);
    result.underrunFrames = m_underrunFrames.load(std::memory_order_relaxed);
    return result;
}

void LockFreeRingBuffer::reset()
{
    m_readPos.store(0, std::memory_order_relaxed);
    m_writePos.store(0, std::memory_order_relaxed);
    m_dropped.store(0, std::memory_order_relaxed);
    m_underrunFrames.store(0, std::memory_order_relaxed);
}

} // namespace audient::engine