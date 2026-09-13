#pragma once

// CaptureControlClient.h - user-mode client for the Q5-A3B capture control plane.
//
// Thin, deterministic wrapper over the control-device IOCTLs defined in
// CaptureControlPlane.h. It is the counterpart of the kernel module
// audientcontrolplane.cpp. It performs ONLY control-plane operations (never the
// realtime callback): open the device interface, query caps, connect a ring,
// disconnect, flush, query state. The producer path stays in the app worker
// (VirtualMicFeeder -> SharedRingCaptureSink), as fixed by Q3/Q5-A3.
//
// This header is the USER-MODE side; it uses Win32 (CreateFile/DeviceIoControl)
// and SetupAPI to find the control device. It deliberately does NOT know about
// the application's audio graph (routing/vst3/transport of the app).
//
// The client is responsible for:
//   - creating the shared SECTION (CreateFileMapping) and mapping a view,
//   - passing the user-mode base + mapped length to CONNECT,
//   - keeping the view mapped for the whole connection (the kernel locks those
//     pages), and
//   - writing fresh mono float32 blocks through the transport contract.

#include "virtual_audio/driver-protocol/CaptureControlPlane.h"

#include <windows.h>
#include <cstdint>

namespace audient
{
namespace virtual_audio
{

class CaptureControlClient
{
public:
    CaptureControlClient();
    ~CaptureControlClient();

    CaptureControlClient(const CaptureControlClient&) = delete;
    CaptureControlClient& operator=(const CaptureControlClient&) = delete;

    // Open the control device by its stable interface GUID. Returns false when
    // the driver/control device is not present.
    bool open();

    // Negotiation: the driver's single supported profile.
    bool queryCaps(audient::capture_control::CaptureControlCaps* caps) const;

    // Connect the given (already mapped) ring view. userBase/userBytes must be
    // the client's own mapping; the driver validates the profile and locks the
    // pages. Returns the attach generation on success (0 on failure).
    std::uint64_t connect(void* userBase, std::uint64_t userBytes);

    bool disconnect();
    bool flush();
    bool queryState(audient::capture_control::CaptureControlState* state) const;

    bool isOpen() const { return m_handle != INVALID_HANDLE_VALUE; }
    HANDLE handle() const { return m_handle; }

private:
    bool deviceIoControl(DWORD ioctl, void* inBuf, DWORD inLen,
                         void* outBuf, DWORD outLen, DWORD* returned) const;

    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

} // namespace virtual_audio
} // namespace audient