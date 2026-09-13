#include "virtual_audio/endpoint/SoftwareCaptureEndpoint.h"

#include <algorithm>

namespace audient::virtual_audio
{

SoftwareCaptureEndpoint::SoftwareCaptureEndpoint(transport::VirtualCaptureTransport& transport)
    : m_transport(transport)
{
}

transport::Format SoftwareCaptureEndpoint::format() const
{
    return m_transport.format();
}

const char* SoftwareCaptureEndpoint::friendlyName() const
{
    return kVirtualMicFriendlyName;
}

std::size_t SoftwareCaptureEndpoint::captureMono(float* mono, std::size_t frames, std::size_t staleThresholdFrames)
{
    if (mono == nullptr || frames == 0)
    {
        return 0;
    }
    (void)captureMonoFresh(mono, frames, staleThresholdFrames);
    return frames;
}

bool SoftwareCaptureEndpoint::captureMonoFresh(float* mono, std::size_t frames, std::size_t staleThresholdFrames)
{
    if (mono == nullptr || frames == 0)
    {
        return false;
    }

    if (m_transport.readProcessedMicFresh(mono, frames, staleThresholdFrames))
    {
        m_freshServedFrames.fetch_add(frames, std::memory_order_relaxed);
        return true;
    }

    // Engine absent/late/transport empty: output EXACT digital silence. Never
    // block, never replay stale samples, never invent audio (AGENTS §11).
    std::fill(mono, mono + frames, 0.0f);
    m_silenceServedFrames.fetch_add(frames, std::memory_order_relaxed);
    return false;
}

bool SoftwareCaptureEndpoint::producerActive() const
{
    return m_transport.producerSequence() > 0;
}

EndpointCounters SoftwareCaptureEndpoint::counters() const
{
    EndpointCounters result;
    const transport::VirtualCaptureTransport::CaptureStatistics transportStats = m_transport.stats();
    result.producedSamples = transportStats.produced;
    result.overflowDrops = transportStats.overflowDrops;
    result.staleCatchupDrops = transportStats.staleCatchupDrops;
    result.underruns = transportStats.underruns;
    result.freshServedFrames = m_freshServedFrames.load(std::memory_order_relaxed);
    result.silenceServedFrames = m_silenceServedFrames.load(std::memory_order_relaxed);
    return result;
}

void SoftwareCaptureEndpoint::reset()
{
    m_transport.reset();
    m_freshServedFrames.store(0, std::memory_order_relaxed);
    m_silenceServedFrames.store(0, std::memory_order_relaxed);
}

} // namespace audient::virtual_audio