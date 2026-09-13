#pragma once

// CaptureControlPlane.h - versioned app<->driver CAPTURE CONTROL PLANE contract.
//
// Q5-A3B / plan "§9 step 3 -> publish a real region" slice. This header is the
// SINGLE canonical layout for the control plane that publishes the shared
// capture ring (CaptureRingContract.h) into the AudientConsoleMic WaveRT
// driver. It is compiled UNCHANGED in BOTH:
//   - the user-mode CMake/MSVC C++20 build (client + unit tests), and
//   - the WDK kernel-mode C++ build (files in src/virtual_audio/driver).
//
// HARD COMPILE CONTRACT (identical to CaptureRingContract.h):
//   - NO <atomic>, NO STL, NO exceptions, NO RTTI, NO dynamic allocation.
//   - Only fixed-width fundamental types (U32 = unsigned int, U64 = unsigned
//     long long) so user and kernel MSVC x64 layouts agree. Struct field ORDER
//     is the ABI; never reorder, never repurpose a field, bump
//     CONTROL_PROTOCOL_VERSION when the layout changes.
//   - IOCTL codes are computed with plain integer arithmetic here so the header
//     does not depend on winioctl.h/ntddk.h availability in either build.
//
// CONTROL PLANE (not the audio path):
//   - A control DEVICE OBJECT created by the driver exposes a STABLE device
//     interface GUID (below). The app opens it with CreateFile (via SetupAPI or
//     a device path/symlink), then performs the control operations. The
//     realtime ASIO callback and the WaveRT DMA fill NEVER issue any of these
//     IOCTLs (AGENTS §7: no DeviceIoControl/mapping/allocation/locks/kernel
//     transitions in the callbacks).
//   - The region itself is a user-mode section (file mapping) whose pages the
//     driver locks (MmProbeAndLockPages) on CONNECT and reads via the MDL's
//     SYSTEM address - so DISPATCH-level audio code never dereferences a raw
//     user pointer; it reads a locked, validated, kernel-mapped ring.
//
// Lifecycle operations (all METHOD_BUFFERED, PASSIVE_LEVEL):
//   QUERY_CAPS  -> returns the single supported profile (negotiation).
//   CONNECT     -> validates negotiated profile, locks the caller's ring
//                  mapping, initializes the contract region, publishes it to
//                  the capture source, assigns a new attach generation, and
//                  enforces ONE active producer (device busy on second).
//   DISCONNECT  -> unpublishes the region (DMA serves exact silence), clears
//                  CONNECTED, bumps generation; releases the producer binding.
//   FLUSH       -> generation reset: re-initializes the current region to a
//                  fresh empty epoch (no stale audio after reconnect).
//   QUERY_STATE -> returns transport counters (positions, drops, underruns,
//                  generation) for diagnostics/tests.
//   Handle close / process exit of the connected file object performs an
//   automatic DISCONNECT (deterministic cleanup: IRP_MJ_CLEANUP).

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "CaptureRingContract.h"

namespace audient
{
namespace capture_control
{

// ---------------------------------------------------------------------------
// Stable device interface GUID for the control plane.
// ---------------------------------------------------------------------------
// Registered by the driver on its control device object. The app enumerates
// this GUID (SetupAPI) and opens the returned device path to issue IOCTLs.
// Stable across driver updates (documented as part of the ABI).
//
// ABI value:  {8ECC7B3A-4D29-4E7C-9611-A20C536E2140}
// The driver and the user-mode client both convert this into a Windows GUID
// with CaptureControlGuidFill() (Data1..3 are stored little-endian integers in
// a Windows GUID; Data4 is a plain byte array). This header deliberately does
// not reference the platform GUID type so it compiles unchanged in both user
// MSVC C++20 and the WDK kernel build (same rule as CaptureRingContract.h).
inline void CaptureControlGuidFill(unsigned char bytes[16])
{
    // Data1 = 0x8ECC7B3A, Data2 = 0x4D29, Data3 = 0x4E7C (all LE)
    bytes[0] = 0x3A; bytes[1] = 0x7B; bytes[2] = 0xCC; bytes[3] = 0x8E;
    bytes[4] = 0x29; bytes[5] = 0x4D; bytes[6] = 0x7C; bytes[7] = 0x4E;
    // Data4 = { 0x96, 0x11, 0xA2, 0x0C, 0x53, 0x6E, 0x21, 0x40 }
    bytes[8] = 0x96; bytes[9] = 0x11; bytes[10] = 0xA2; bytes[11] = 0x0C;
    bytes[12] = 0x53; bytes[13] = 0x6E; bytes[14] = 0x21; bytes[15] = 0x40;
}

// Control-plane protocol version. Bump whenever the request/response layouts,
// profile rules, or IOCTL set change incompatibly. Independent of
// REGION_VERSION (the ring ABI).
enum ControlConstants
{
    CONTROL_PROTOCOL_MAGIC = 0x41554332u,   // '2CUA' little-endian (distinct from region magic '1CUA')
    CONTROL_PROTOCOL_VERSION = 1u,          // IOCTL/struct layout version
    CONTROL_DEVICE_TYPE = 0x00000022u,      // FILE_DEVICE_UNKNOWN
    CONTROL_IOCTL_FUNCTION_BASE = 0x0800u,
    CONTROL_MAX_RESPONSE_BYTES = 256u       // bound for METHOD_BUFFERED responses
};

// Single supported negotiated profile (from CaptureRingContract.h rules):
//   mono float32, 48000 Hz, producer block 64 frames, ring capacity 8192
// frames (power of two >= block). Kept in this header so BOTH sides derive the
// exact same geometry and every host/VM test validates against it.
enum ControlProfileConstants
{
    CONTROL_FORMAT_TAG = audient::capture_ring::REGION_FORMAT_MONO_FLOAT32,
    CONTROL_SAMPLE_RATE_HZ = audient::capture_ring::REGION_SAMPLE_RATE,
    CONTROL_CHANNELS = audient::capture_ring::REGION_CHANNELS,
    CONTROL_BLOCK_FRAMES = 64u,
    CONTROL_CAPACITY_FRAMES = 8192u          // 2^13; ~170 ms @ 48 kHz mono
};

// Byte size of the locked region the client must map and hand to the driver.
inline audient::capture_ring::U64 CaptureControlRegionBytes()
{
    return audient::capture_ring::CaptureRingRegionBytes(CONTROL_CAPACITY_FRAMES);
}

// IOCTL code for the control device (plain arithmetic; matches CTL_CODE).
// CTL_CODE(FILE_DEVICE_UNKNOWN, fn, METHOD_BUFFERED, FILE_ANY_ACCESS)
//   = ((0x22)<<16) | ((0)<<14) | ((fn)<<2) | (0)
// constexpr so the values can be used as `case` labels in kernel/C++ builds.
inline constexpr audient::capture_ring::U32 CaptureControlIoctl(
    audient::capture_ring::U32 function)
{
    return static_cast<audient::capture_ring::U32>(
        (CONTROL_DEVICE_TYPE << 16) | (function << 2));
}

enum ControlFunctions
{
    CONTROL_FN_QUERY_CAPS   = 0x0801u,
    CONTROL_FN_CONNECT      = 0x0802u,
    CONTROL_FN_DISCONNECT   = 0x0803u,
    CONTROL_FN_FLUSH        = 0x0804u,
    CONTROL_FN_QUERY_STATE  = 0x0805u
};

#define IOCTL_AUDIENT_CAPTURE_QUERY_CAPS   audient::capture_control::CaptureControlIoctl(audient::capture_control::CONTROL_FN_QUERY_CAPS)
#define IOCTL_AUDIENT_CAPTURE_CONNECT      audient::capture_control::CaptureControlIoctl(audient::capture_control::CONTROL_FN_CONNECT)
#define IOCTL_AUDIENT_CAPTURE_DISCONNECT   audient::capture_control::CaptureControlIoctl(audient::capture_control::CONTROL_FN_DISCONNECT)
#define IOCTL_AUDIENT_CAPTURE_FLUSH        audient::capture_control::CaptureControlIoctl(audient::capture_control::CONTROL_FN_FLUSH)
#define IOCTL_AUDIENT_CAPTURE_QUERY_STATE  audient::capture_control::CaptureControlIoctl(audient::capture_control::CONTROL_FN_QUERY_STATE)

// ---------------------------------------------------------------------------
// Query capabilities (negotiation). The driver returns its single supported
// profile; the app compares against its own build-time geometry and refuses to
// connect if ANY field differs.
// ---------------------------------------------------------------------------
typedef struct CaptureControlCaps
{
    audient::capture_ring::U32 magic;            // CONTROL_PROTOCOL_MAGIC
    audient::capture_ring::U32 protocolVersion;  // CONTROL_PROTOCOL_VERSION
    audient::capture_ring::U32 formatTag;        // CONTROL_FORMAT_TAG
    audient::capture_ring::U32 sampleRateHz;     // CONTROL_SAMPLE_RATE_HZ
    audient::capture_ring::U32 channels;         // CONTROL_CHANNELS
    audient::capture_ring::U32 blockFrames;      // CONTROL_BLOCK_FRAMES
    audient::capture_ring::U32 capacityFrames;   // CONTROL_CAPACITY_FRAMES
    audient::capture_ring::U64 regionBytes;      // CaptureControlRegionBytes()
    audient::capture_ring::U32 reserved[2];
} CaptureControlCaps, *PCaptureControlCaps;

// ---------------------------------------------------------------------------
// CONNECT request. The app passes the negotiated profile PLUS the base address
// (and byte length) of its OWN mapped ring region. The driver validates every
// field, locks the caller's pages, and publishes the region through the shared
// contract. userBase/userBytes are only ever touched at PASSIVE_LEVEL in the
// control path (probe+lock); DISPATCH code reads the MDL system address.
// ---------------------------------------------------------------------------
typedef struct CaptureControlConnectRequest
{
    audient::capture_ring::U32 magic;            // CONTROL_PROTOCOL_MAGIC
    audient::capture_ring::U32 protocolVersion;  // CONTROL_PROTOCOL_VERSION
    audient::capture_ring::U32 formatTag;        // CONTROL_FORMAT_TAG
    audient::capture_ring::U32 sampleRateHz;     // CONTROL_SAMPLE_RATE_HZ
    audient::capture_ring::U32 channels;         // CONTROL_CHANNELS
    audient::capture_ring::U32 blockFrames;      // CONTROL_BLOCK_FRAMES
    audient::capture_ring::U32 capacityFrames;   // CONTROL_CAPACITY_FRAMES
    audient::capture_ring::U64 regionBytes;      // CaptureControlRegionBytes()
    audient::capture_ring::U64 userBase;         // user-mode base of the mapped region
    audient::capture_ring::U64 userBytes;        // mapped byte length (>= regionBytes)
} CaptureControlConnectRequest, *PCaptureControlConnectRequest;

// ---------------------------------------------------------------------------
// CONNECT / FLUSH response. generation is the attach epoch the app must use as
// the transport generation for every subsequent writeMono (see
// SharedRingCaptureSink / VirtualMicFeeder).
// ---------------------------------------------------------------------------
typedef struct CaptureControlConnectResponse
{
    audient::capture_ring::U32 status;           // NTSTATUS (kernel), 0 == success
    audient::capture_ring::U32 reserved;
    audient::capture_ring::U64 generation;       // attach generation assigned by kernel
    audient::capture_ring::U32 connected;        // 1 when the region is published
    audient::capture_ring::U32 activeProducers;  // 0/1 (NULL until first query)
} CaptureControlConnectResponse, *PCaptureControlConnectResponse;

// ---------------------------------------------------------------------------
// QUERY_STATE response: transport counters (positions / drops / underruns).
// ---------------------------------------------------------------------------
typedef struct CaptureControlState
{
    audient::capture_ring::U64 generation;
    audient::capture_ring::U64 writePos;
    audient::capture_ring::U64 readPos;
    audient::capture_ring::U64 producedSamples;
    audient::capture_ring::U64 consumedSamples;
    audient::capture_ring::U64 overflowDrops;
    audient::capture_ring::U64 staleCatchupDrops;
    audient::capture_ring::U64 underruns;
    audient::capture_ring::U64 generationFlushes;
    audient::capture_ring::U32 connected;
    audient::capture_ring::U32 protocolVersion;
} CaptureControlState, *PCaptureControlState;

// ---------------------------------------------------------------------------
// Pure profile validation, shared verbatim by the kernel CONNECT handler and
// the host unit tests. Returns 0 when the request matches the single supported
// profile, or a stable rejection code (mirrored in tests).
// ---------------------------------------------------------------------------
enum ControlProfileResult
{
    CONTROL_PROFILE_OK = 0,
    CONTROL_PROFILE_BAD_MAGIC = 1,
    CONTROL_PROFILE_BAD_PROTOCOL_VERSION = 2,
    CONTROL_PROFILE_BAD_FORMAT = 3,
    CONTROL_PROFILE_BAD_RATE = 4,
    CONTROL_PROFILE_BAD_CHANNELS = 5,
    CONTROL_PROFILE_BAD_BLOCK = 6,
    CONTROL_PROFILE_BAD_CAPACITY = 7,
    CONTROL_PROFILE_BAD_REGION_BYTES = 8,
    CONTROL_PROFILE_BAD_USER_BYTES = 9,
    CONTROL_PROFILE_BAD_USER_BASE = 10
};

inline int CaptureControlValidateProfile(
    const CaptureControlConnectRequest* req,
    audient::capture_ring::U64 minUserBytes)
{
    if (req == 0)
    {
        return CONTROL_PROFILE_BAD_USER_BASE;
    }
    if (req->magic != CONTROL_PROTOCOL_MAGIC)
    {
        return CONTROL_PROFILE_BAD_MAGIC;
    }
    if (req->protocolVersion != CONTROL_PROTOCOL_VERSION)
    {
        return CONTROL_PROFILE_BAD_PROTOCOL_VERSION;
    }
    if (req->formatTag != CONTROL_FORMAT_TAG)
    {
        return CONTROL_PROFILE_BAD_FORMAT;
    }
    if (req->sampleRateHz != CONTROL_SAMPLE_RATE_HZ)
    {
        return CONTROL_PROFILE_BAD_RATE;
    }
    if (req->channels != CONTROL_CHANNELS)
    {
        return CONTROL_PROFILE_BAD_CHANNELS;
    }
    if (req->blockFrames != CONTROL_BLOCK_FRAMES)
    {
        return CONTROL_PROFILE_BAD_BLOCK;
    }
    if (req->capacityFrames != CONTROL_CAPACITY_FRAMES ||
        !audient::capture_ring::CaptureRingCapacityIsLegal(req->capacityFrames, req->blockFrames))
    {
        return CONTROL_PROFILE_BAD_CAPACITY;
    }
    if (req->regionBytes != CaptureControlRegionBytes())
    {
        return CONTROL_PROFILE_BAD_REGION_BYTES;
    }
    if (req->userBytes < req->regionBytes ||
        (minUserBytes != 0 && req->userBytes < minUserBytes))
    {
        return CONTROL_PROFILE_BAD_USER_BYTES;
    }
    if (req->userBase == 0 ||
        (req->userBase & (static_cast<audient::capture_ring::U64>(sizeof(void*)) - 1ull)) != 0ull)
    {
        return CONTROL_PROFILE_BAD_USER_BASE;
    }
    return CONTROL_PROFILE_OK;
}

// Wait/cleanup helper: a mapped view of exactly the negotiated size is
// page-rounded on the CLIENT side (MapViewOfFile granularity); the driver only
// ever probes/locks `regionBytes` from `userBase`, so this returns the number
// of bytes the client must actually map (next multiple of the page size).
inline audient::capture_ring::U64 CaptureControlMappedBytes()
{
    // MapViewOfFile granularity is one page (4096); round the negotiated region
    // up so the client maps enough bytes (the driver locks `regionBytes` from
    // `userBase`, so extra mapped bytes are harmless).
    const audient::capture_ring::U64 region = CaptureControlRegionBytes();
    const audient::capture_ring::U64 pageMask = 0xFFFu; // 4096-1
    return (region + pageMask) & ~pageMask;
}

} // namespace capture_control
} // namespace audient