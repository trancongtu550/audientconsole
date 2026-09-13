#pragma once

#include "virtual_audio/endpoint/VirtualCaptureEndpoint.h"

#include "transport/VirtualCaptureTransport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::virtual_audio
{

// Slice Q2 — user-mode software implementation of the Windows virtual
// microphone endpoint contract. Pulls the processed mic uplink off a
// transport::VirtualCaptureTransport using the Slice-Q1 freshness policy and
// enforces the endpoint rules: fresh-only delivery, digital silence on absent
// producer, and wholly-counted drops.
//
// This is the thin, fully-testable beam that the future Phase-8 WaveRT-based
// kernel capture endpoint and the app-side bridge must both honor. It is NOT
// the kernel driver; it contains no Windows/driver-specific code and builds and
// tests on this daily machine without the WDK.
class SoftwareCaptureEndpoint final : public VirtualCaptureEndpoint
{
public:
    explicit SoftwareCaptureEndpoint(transport::VirtualCaptureTransport& transport);

    transport::Format format() const override;
    const char* friendlyName() const override;
    std::size_t captureMono(float* mono, std::size_t frames, std::size_t staleThresholdFrames) override;
    bool captureMonoFresh(float* mono, std::size_t frames, std::size_t staleThresholdFrames) override;
    bool producerActive() const override;
    void reset() override;
    EndpointCounters counters() const override;

private:
    transport::VirtualCaptureTransport& m_transport;
    std::atomic<std::uint64_t> m_freshServedFrames{0};
    std::atomic<std::uint64_t> m_silenceServedFrames{0};
};

} // namespace audient::virtual_audio