#include "virtual_audio/driver-protocol/SharedRingCaptureSink.h"

#include <cstdlib>
#include <malloc.h>
#include <new>
#include <stdexcept>

namespace audient::virtual_audio
{

namespace
{

void* allocateAlignedRegion(std::uint64_t bytes)
{
    // capture_ring does not require more than natural alignment for the header,
    // but a cache-line alignment keeps the producer/consumer counters on
    // distinct lines; 64 is also what the engine ring uses for its head.
    constexpr std::uint64_t kAlignment = 64u;
    void* p = _aligned_malloc(static_cast<size_t>(bytes), static_cast<size_t>(kAlignment));
    if (p == nullptr)
    {
        throw std::bad_alloc();
    }
    return p;
}

} // namespace

SharedRingCaptureSink::SharedRingCaptureSink(transport::Format format, std::size_t capacityFrames)
    : m_format(format)
    , m_ownedRegion(nullptr)
    , m_header(nullptr)
    , m_regionBytes(0)
{
    if (format.sampleRateHz != audient::capture_ring::REGION_SAMPLE_RATE ||
        format.channels != 1 || format.blockSamples <= 0)
    {
        throw std::invalid_argument("SharedRingCaptureSink requires mono float32 at the contract sample rate");
    }
    if (!capture_ring::CaptureRingCapacityIsLegal(static_cast<capture_ring::U32>(capacityFrames),
                                                  static_cast<capture_ring::U32>(format.blockSamples)))
    {
        throw std::invalid_argument("SharedRingCaptureSink capacity must be a power of two >= block size");
    }

    const std::uint64_t bytes =
        capture_ring::CaptureRingRegionBytes(static_cast<capture_ring::U32>(capacityFrames));
    m_ownedRegion = allocateAlignedRegion(bytes);
    m_header = static_cast<capture_ring::CaptureRingHeader*>(m_ownedRegion);
    m_regionBytes = bytes;

    const int ok = capture_ring::CaptureRingInit(
        m_header, m_regionBytes, static_cast<capture_ring::U32>(capacityFrames),
        static_cast<capture_ring::U32>(format.blockSamples));
    if (!ok)
    {
        _aligned_free(m_ownedRegion);
        m_ownedRegion = nullptr;
        m_header = nullptr;
        throw std::invalid_argument("SharedRingCaptureSink region init failed");
    }
    // Default state mirrors MockDriverCaptureSink (connected until the test/app
    // simulates a disconnect).
    m_connected.store(true, std::memory_order_release);
    m_header->flags |= capture_ring::REGION_FLAG_CONNECTED;
}

SharedRingCaptureSink::SharedRingCaptureSink(transport::Format format, void* region,
                                             std::uint64_t regionBytes)
    : m_format(format)
    , m_ownedRegion(nullptr)
    , m_header(static_cast<capture_ring::CaptureRingHeader*>(region))
    , m_regionBytes(regionBytes)
{
    if (region == nullptr || regionBytes == 0)
    {
        throw std::invalid_argument("SharedRingCaptureSink requires a region");
    }
    const int valid = capture_ring::CaptureRingValidate(
        m_header, m_regionBytes, capture_ring::REGION_FORMAT_MONO_FLOAT32,
        capture_ring::REGION_SAMPLE_RATE);
    if (!valid)
    {
        throw std::invalid_argument("SharedRingCaptureSink region does not match the capture contract");
    }
    if (format.sampleRateHz != static_cast<int>(m_header->sampleRateHz) || format.channels != 1 ||
        static_cast<std::uint64_t>(format.blockSamples) != m_header->blockFrames)
    {
        throw std::invalid_argument("SharedRingCaptureSink format does not match the region");
    }
    // Default state mirrors MockDriverCaptureSink (connected).
    m_connected.store(true, std::memory_order_release);
    m_header->flags |= capture_ring::REGION_FLAG_CONNECTED;
}

SharedRingCaptureSink::~SharedRingCaptureSink()
{
    if (m_ownedRegion != nullptr)
    {
        _aligned_free(m_ownedRegion);
        m_ownedRegion = nullptr;
    }
    m_header = nullptr;
}

bool SharedRingCaptureSink::connected() const
{
    return m_connected.load(std::memory_order_acquire);
}

std::size_t SharedRingCaptureSink::capacityFrames() const
{
    return m_header ? m_header->capacityFrames : 0u;
}

std::size_t SharedRingCaptureSink::availableFrames() const
{
    if (m_header == nullptr)
    {
        return 0u;
    }
    const std::uint64_t write = m_header->writePos;
    const std::uint64_t read = m_header->readPos;
    const std::uint64_t delta = write - read;
    return delta > m_header->capacityFrames ? m_header->capacityFrames
                                            : static_cast<std::size_t>(delta);
}

bool SharedRingCaptureSink::writeMono(const float* mono, std::size_t frames,
                                      std::uint64_t generation, std::uint64_t sequence)
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

    const capture_ring::WriteResult result = capture_ring::CaptureRingProducerWrite(
        m_header, mono, static_cast<capture_ring::U32>(frames), generation, sequence);
    if (result == capture_ring::WRITE_DISCONNECTED)
    {
        m_rejectedDisconnected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (result == capture_ring::WRITE_STALLED)
    {
        m_rejectedStall.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (result != capture_ring::WRITE_OK)
    {
        return false;
    }

    // Track the region's live generation (flushed on attach inside the
    // producer) so snapshot().lastGeneration mirrors the mock.
    m_lastGeneration.store(m_header->generation, std::memory_order_relaxed);
    m_lastAcceptedSequence.store(sequence, std::memory_order_relaxed);
    m_acceptedWrites.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void SharedRingCaptureSink::setConnected(bool connected)
{
    m_connected.store(connected, std::memory_order_release);
    if (m_header != nullptr)
    {
        if (connected)
        {
            m_header->flags |= capture_ring::REGION_FLAG_CONNECTED;
        }
        else
        {
            m_header->flags &= ~capture_ring::REGION_FLAG_CONNECTED;
        }
    }
}

bool SharedRingCaptureSink::readMono(float* mono, std::size_t frames,
                                     std::size_t staleThresholdFrames)
{
    if (mono == nullptr || frames == 0)
    {
        return false;
    }
    return capture_ring::CaptureRingConsumerRead(
               m_header, mono, static_cast<capture_ring::U32>(frames),
               static_cast<capture_ring::U32>(staleThresholdFrames)) != 0;
}

std::uint32_t SharedRingCaptureSink::fillStereo(float* stereo, std::uint32_t frames,
                                                std::uint32_t staleThresholdFrames)
{
    return static_cast<std::uint32_t>(capture_ring::CaptureRingConsumerFillStereo(
        m_header, stereo, frames, staleThresholdFrames));
}

capture_ring::CaptureRingHeader* SharedRingCaptureSink::region()
{
    return m_header;
}

SharedRingCaptureSink::Snapshot SharedRingCaptureSink::snapshot() const
{
    Snapshot result;
    result.lastGeneration = m_lastGeneration.load(std::memory_order_relaxed);
    result.lastAcceptedSequence = m_lastAcceptedSequence.load(std::memory_order_relaxed);
    result.acceptedWrites = m_acceptedWrites.load(std::memory_order_relaxed);
    result.rejectedDisconnected = m_rejectedDisconnected.load(std::memory_order_relaxed);
    result.rejectedStall = m_rejectedStall.load(std::memory_order_relaxed);

    if (m_header != nullptr)
    {
        result.generationFlushes = m_header->generationFlushes;
        result.producedSamples = m_header->producedSamples;
        result.consumedSamples = m_header->consumedSamples;
        result.staleCatchupDrops = m_header->staleCatchupDrops;
        result.underruns = m_header->underruns;
    }
    return result;
}

} // namespace audient::virtual_audio
