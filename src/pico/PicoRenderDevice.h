#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace audient::pico
{

// Result of a Pico render-endpoint discovery/open attempt.
struct PicoDeviceOpenResult
{
    bool ok = false;
    std::string deviceName;
    std::string error;
};

// Hardware-side render sink for the VoxBridge Pico v0.1.1 UAC2 device.
//
// The Pico is a STANDARD class-compliant USB Audio Class 2 device: Windows
// binds its built-in usbaudio.sys driver. The PC-side transport is therefore a
// plain WASAPI render stream to the bridge playback endpoint
// ("Speakers (... Audient Console Bridge)"), NOT WinUSB/HID/vendor reports.
//
// This interface isolates the WasapiPicoRenderDevice (COM/device lifetime) from
// PicoUsbWorker (ring drain + mono->stereo interleave + lifetime/state machine)
// so the worker logic is fully testable without audio hardware.
//
// Threading: every method except the destructor is called on the PicoUsbWorker
// NORMAL worker thread. None of these run in the ASIO realtime callback.
class IPicoRenderDevice
{
public:
    virtual ~IPicoRenderDevice() = default;

    // Discover + initialize + start the render stream. Called on the worker
    // thread. On success isOpen() is true and sampleRate()/channels() report the
    // negotiated shared-mode mix format (expected 48 kHz stereo).
    virtual PicoDeviceOpenResult open() = 0;

    // Release the stream (idempotent). Does not uninitialize COM.
    virtual void close() = 0;

    virtual bool isOpen() const = 0;

    // True when the endpoint disappeared or changed and the stream must be
    // torn down and re-discovered (device loss / removal notification).
    virtual bool needsReopen() const = 0;

    // Block up to timeoutMs for render capacity to become available (WASAPI
    // event). Returns false on timeout or when needsReopen() becomes true.
    virtual bool waitForRenderReady(unsigned timeoutMs) = 0;

    // Wake a blocked waitForRenderReady() (used by requestStop()).
    virtual void wake() = 0;

    // Free frames in the current WASAPI render buffer (0 when none/after loss).
    virtual unsigned availableRenderFrames() = 0;

    // Push up to `frames` interleaved stereo float frames (L,R). Returns true
    // when the frames were committed. Sets *deviceLost when the endpoint
    // disappeared (caller must close + re-discover). Never called from RT.
    virtual bool submit(const float* stereoInterleaved, std::size_t frames, bool* deviceLost) = 0;

    virtual unsigned sampleRate() const = 0;
    virtual unsigned channels() const = 0;
    virtual const std::string& deviceName() const = 0;

    // Called once on the worker thread just before it exits, so the device can
    // balance any per-thread COM initialization it performed in open().
    virtual void threadShutdown() = 0;
};

} // namespace audient::pico
