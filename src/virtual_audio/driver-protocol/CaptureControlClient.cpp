#include "virtual_audio/driver-protocol/CaptureControlClient.h"

#include <initguid.h>
#include <setupapi.h>
#include <devguid.h>

#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")

// Basis for finding the control device via SetupAPI: the stable interface GUID
// lives in CaptureControlPlane.h; we materialize it here once.
namespace
{

GUID controlInterfaceGuid()
{
    GUID g;
    audient::capture_control::CaptureControlGuidFill(reinterpret_cast<unsigned char*>(&g));
    return g;
}

// Resolve the device-interface path for the control plane GUID (SetupAPI
// enumeration). Returns an empty string when not found.
std::wstring findControlInterfacePath()
{
    const GUID guid = controlInterfaceGuid();

    HDEVINFO devs = SetupDiGetClassDevsW(
        &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE)
    {
        return std::wstring();
    }

    SP_DEVICE_INTERFACE_DATA ifData;
    std::memset(&ifData, 0, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    std::wstring path;
    if (SetupDiEnumDeviceInterfaces(devs, nullptr, &guid, 0, &ifData))
    {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, &required, nullptr);
        if (required > 0)
        {
            std::vector<unsigned char> buffer(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(devs, &ifData, detail, required, nullptr, nullptr))
            {
                path = detail->DevicePath;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return path;
}

} // namespace

namespace audient
{
namespace virtual_audio
{

CaptureControlClient::CaptureControlClient() = default;

CaptureControlClient::~CaptureControlClient()
{
    if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr)
    {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

bool CaptureControlClient::open()
{
    if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr)
    {
        return true;
    }

    // Prefer SetupAPI discovery by the stable interface GUID; fall back to the
    // fixed control-device symlink (same target as the SAMPLES' control DO).
    std::wstring path = findControlInterfacePath();
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!path.empty())
    {
        h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE || h == nullptr)
    {
        h = CreateFileW(L"\\\\.\\AudientConsoleControl",
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE || h == nullptr)
    {
        return false;
    }
    m_handle = h;
    return true;
}

bool CaptureControlClient::deviceIoControl(DWORD ioctl, void* inBuf, DWORD inLen,
                                           void* outBuf, DWORD outLen,
                                           DWORD* returned) const
{
    if (m_handle == INVALID_HANDLE_VALUE || m_handle == nullptr)
    {
        return false;
    }
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(
        m_handle, ioctl, inBuf, inLen, outBuf, outLen, &bytesReturned, nullptr);
    if (returned != nullptr)
    {
        *returned = bytesReturned;
    }
    return ok != FALSE;
}

bool CaptureControlClient::queryCaps(audient::capture_control::CaptureControlCaps* caps) const
{
    if (caps == nullptr)
    {
        return false;
    }
    DWORD returned = 0;
    if (!deviceIoControl(IOCTL_AUDIENT_CAPTURE_QUERY_CAPS, nullptr, 0,
                         caps, sizeof(*caps), &returned))
    {
        return false;
    }
    return returned == sizeof(*caps) &&
           caps->magic == audient::capture_control::CONTROL_PROTOCOL_MAGIC &&
           caps->protocolVersion == audient::capture_control::CONTROL_PROTOCOL_VERSION;
}

std::uint64_t CaptureControlClient::connect(void* userBase, std::uint64_t userBytes)
{
    audient::capture_control::CaptureControlConnectRequest req;
    std::memset(&req, 0, sizeof(req));
    req.magic = audient::capture_control::CONTROL_PROTOCOL_MAGIC;
    req.protocolVersion = audient::capture_control::CONTROL_PROTOCOL_VERSION;
    req.formatTag = audient::capture_control::CONTROL_FORMAT_TAG;
    req.sampleRateHz = audient::capture_control::CONTROL_SAMPLE_RATE_HZ;
    req.channels = audient::capture_control::CONTROL_CHANNELS;
    req.blockFrames = audient::capture_control::CONTROL_BLOCK_FRAMES;
    req.capacityFrames = audient::capture_control::CONTROL_CAPACITY_FRAMES;
    req.regionBytes = audient::capture_control::CaptureControlRegionBytes();
    req.userBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(userBase));
    req.userBytes = userBytes;

    audient::capture_control::CaptureControlConnectResponse resp;
    std::memset(&resp, 0, sizeof(resp));

    DWORD returned = 0;
    if (!deviceIoControl(IOCTL_AUDIENT_CAPTURE_CONNECT, &req, sizeof(req),
                         &resp, sizeof(resp), &returned))
    {
        return 0;
    }
    if (resp.status != 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(resp.generation);
}

bool CaptureControlClient::disconnect()
{
    DWORD returned = 0;
    return deviceIoControl(IOCTL_AUDIENT_CAPTURE_DISCONNECT, nullptr, 0,
                           nullptr, 0, &returned);
}

bool CaptureControlClient::flush()
{
    DWORD returned = 0;
    return deviceIoControl(IOCTL_AUDIENT_CAPTURE_FLUSH, nullptr, 0,
                           nullptr, 0, &returned);
}

bool CaptureControlClient::queryState(audient::capture_control::CaptureControlState* state) const
{
    if (state == nullptr)
    {
        return false;
    }
    DWORD returned = 0;
    if (!deviceIoControl(IOCTL_AUDIENT_CAPTURE_QUERY_STATE, nullptr, 0,
                         state, sizeof(*state), &returned))
    {
        return false;
    }
    return returned == sizeof(*state);
}

} // namespace virtual_audio
} // namespace audient