#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace audient::engine
{

enum class RingBufferStatus
{
    Ok,
    Full,
    Empty,
};

class LockFreeRingBuffer
{
public:
    struct Snapshot
    {
        std::uint64_t produced = 0;
        std::uint64_t consumed = 0;
        std::uint64_t dropped = 0;
        std::uint64_t underrunFrames = 0;
    };

    explicit LockFreeRingBuffer(std::size_t capacityFrames);

    RingBufferStatus writeBlock(const float* frames, std::size_t count);
    RingBufferStatus readBlock(float* frames, std::size_t count);

    // Consumer-side discard: advance the READ head by up to `count` committed
    // samples WITHOUT copying them. The cancel-consumer may call this to drop
    // a stale backlog and resynchronize closer to the producer's write head.
    // The producer thread NEVER calls this (the write head is the only head the
    // producer may move), so single-consumer ownership of the read head is
    // preserved (SPSC contract). Returns false if `count` exceeds what is
    // currently committed (nothing is discarded in that case).
    bool discard(std::size_t count);

    std::size_t capacity() const;
    std::size_t available() const;
    Snapshot snapshot() const;

    void reset();

private:
    std::size_t m_capacity = 0;
    std::size_t m_mask = 0;
    std::unique_ptr<float[]> m_memory;

    alignas(64) std::atomic<std::uint64_t> m_readPos{0};
    std::atomic<std::uint64_t> m_writePos{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<std::uint64_t> m_underrunFrames{0};
};

} // namespace audient::engine

#if defined(_MSC_VER)
#pragma warning(pop)
#endif