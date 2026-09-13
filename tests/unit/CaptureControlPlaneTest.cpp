// CaptureControlPlaneTest.cpp - deterministic host tests for the Q5-A3B control
// plane ABI (CaptureControlPlane.h). The same header is compiled UNCHANGED into
// the WDK kernel build (audientcontrolplane.cpp CONNECT handler), so these tests
// prove the profile/geometry/negotiation rules the kernel enforces.
//
// Everything exercised here is pure header logic (no kernel or Windows calls):
//   - IOCTL code derivation matches CTL_CODE(FILE_DEVICE_UNKNOWN, fn,
//     METHOD_BUFFERED, FILE_ANY_ACCESS);
//   - the single negotiated profile (mono float32, 48 kHz, block 64, capacity
//     8192) matches the shared transport geometry;
//   - CaptureControlValidateProfile accepts an exact request and rejects each
//     incompatible field (magic, version, format, rate, channels, block,
//     capacity, regionBytes, userBytes, userBase) — the same checks the kernel
//     CONNECT handler runs before it locks/publishes any region;
//   - mapped-size page rounding for MapViewOfFile;
//   - GUID byte layout matches the documented {8ECC7B3A-4D29-4E7C-9611-A20C536E2140}.

#include "virtual_audio/driver-protocol/CaptureControlPlane.h"

#include <cstring>
#include <gtest/gtest.h>

namespace audient::capture_control
{

TEST(CaptureControlPlaneTest, IoctlCodesMatchCtlCodeLayout)
{
    // CTL_CODE(FILE_DEVICE_UNKNOWN, fn, METHOD_BUFFERED, FILE_ANY_ACCESS)
    // FILE_DEVICE_UNKNOWN == 0x22, Method 0 (buffered), Access 0 (any).
    const auto build = [](audient::capture_ring::U32 fn) -> audient::capture_ring::U32 {
        return static_cast<audient::capture_ring::U32>(
            (0x22u << 16) | (fn << 2));
    };

    EXPECT_EQ(IOCTL_AUDIENT_CAPTURE_QUERY_CAPS, build(CONTROL_FN_QUERY_CAPS));
    EXPECT_EQ(IOCTL_AUDIENT_CAPTURE_CONNECT, build(CONTROL_FN_CONNECT));
    EXPECT_EQ(IOCTL_AUDIENT_CAPTURE_DISCONNECT, build(CONTROL_FN_DISCONNECT));
    EXPECT_EQ(IOCTL_AUDIENT_CAPTURE_FLUSH, build(CONTROL_FN_FLUSH));
    EXPECT_EQ(IOCTL_AUDIENT_CAPTURE_QUERY_STATE, build(CONTROL_FN_QUERY_STATE));

    // Distinct function codes -> distinct IOCTLs.
    EXPECT_NE(IOCTL_AUDIENT_CAPTURE_QUERY_CAPS, IOCTL_AUDIENT_CAPTURE_CONNECT);
    EXPECT_NE(IOCTL_AUDIENT_CAPTURE_CONNECT, IOCTL_AUDIENT_CAPTURE_DISCONNECT);
    EXPECT_NE(IOCTL_AUDIENT_CAPTURE_DISCONNECT, IOCTL_AUDIENT_CAPTURE_FLUSH);
    EXPECT_NE(IOCTL_AUDIENT_CAPTURE_FLUSH, IOCTL_AUDIENT_CAPTURE_QUERY_STATE);
}

TEST(CaptureControlPlaneTest, GeometryMatchesSharedTransportContract)
{
    // The profile must match the shared transport (/ transport::Format rules).
    ASSERT_EQ(CONTROL_SAMPLE_RATE_HZ, 48000u);
    ASSERT_EQ(CONTROL_FORMAT_TAG, audient::capture_ring::REGION_FORMAT_MONO_FLOAT32);
    ASSERT_EQ(CONTROL_CHANNELS, 1u);
    ASSERT_EQ(CONTROL_BLOCK_FRAMES, 64u);

    // Capacity is a power of two >= block and >= the smallest supported ring.
    EXPECT_GT(CONTROL_CAPACITY_FRAMES, CONTROL_BLOCK_FRAMES);
    EXPECT_EQ(CONTROL_CAPACITY_FRAMES & (CONTROL_CAPACITY_FRAMES - 1u), 0u); // power of two
    EXPECT_TRUE(audient::capture_ring::CaptureRingCapacityIsLegal(
        CONTROL_CAPACITY_FRAMES, CONTROL_BLOCK_FRAMES));
}

TEST(CaptureControlPlaneTest, RegionBytesMatchContractLayout)
{
    const audient::capture_ring::U64 bytes = CaptureControlRegionBytes();
    EXPECT_EQ(bytes,
              audient::capture_ring::CaptureRingRegionBytes(CONTROL_CAPACITY_FRAMES));
    // header 0x88 + frames * sizeof(float)
    EXPECT_EQ(bytes, static_cast<audient::capture_ring::U64>(0x88u) +
                         static_cast<audient::capture_ring::U64>(CONTROL_CAPACITY_FRAMES) * 4u);

    // MapViewOfFile granularity: mapped size must be page-rounded and cover the
    // negotiated region.
    const audient::capture_ring::U64 mapped = CaptureControlMappedBytes();
    EXPECT_GE(mapped, bytes);
    EXPECT_EQ(mapped % 4096u, 0u);
    EXPECT_LT(mapped, bytes + 4096u);
}

TEST(CaptureControlPlaneTest, ValidRequestAccepts)
{
    CaptureControlConnectRequest req;
    std::memset(&req, 0, sizeof(req));
    req.magic = CONTROL_PROTOCOL_MAGIC;
    req.protocolVersion = CONTROL_PROTOCOL_VERSION;
    req.formatTag = CONTROL_FORMAT_TAG;
    req.sampleRateHz = CONTROL_SAMPLE_RATE_HZ;
    req.channels = CONTROL_CHANNELS;
    req.blockFrames = CONTROL_BLOCK_FRAMES;
    req.capacityFrames = CONTROL_CAPACITY_FRAMES;
    req.regionBytes = CaptureControlRegionBytes();
    req.userBase = 0x1000ull; // page-aligned fake VA
    req.userBytes = CaptureControlMappedBytes();

    EXPECT_EQ(CaptureControlValidateProfile(&req, req.regionBytes), CONTROL_PROFILE_OK);
}

TEST(CaptureControlPlaneTest, RejectsEveryIncompatibleField)
{
    auto valid = []() -> CaptureControlConnectRequest {
        CaptureControlConnectRequest req;
        std::memset(&req, 0, sizeof(req));
        req.magic = CONTROL_PROTOCOL_MAGIC;
        req.protocolVersion = CONTROL_PROTOCOL_VERSION;
        req.formatTag = CONTROL_FORMAT_TAG;
        req.sampleRateHz = CONTROL_SAMPLE_RATE_HZ;
        req.channels = CONTROL_CHANNELS;
        req.blockFrames = CONTROL_BLOCK_FRAMES;
        req.capacityFrames = CONTROL_CAPACITY_FRAMES;
        req.regionBytes = CaptureControlRegionBytes();
        req.userBase = 0x1000ull;
        req.userBytes = CaptureControlMappedBytes();
        return req;
    };

    {
        CaptureControlConnectRequest r = valid();
        r.magic = 0xDEADBEEFu;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_MAGIC);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.protocolVersion = CONTROL_PROTOCOL_VERSION + 1u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes),
                  CONTROL_PROFILE_BAD_PROTOCOL_VERSION);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.formatTag = 0x999u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_FORMAT);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.sampleRateHz = 44100u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_RATE);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.channels = 2u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_CHANNELS);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.blockFrames = 128u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_BLOCK);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.capacityFrames = 4096u; // power of two but not the negotiated one
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_CAPACITY);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.capacityFrames = 100u; // not a power of two
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_CAPACITY);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.regionBytes = CaptureControlRegionBytes() + 4u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_REGION_BYTES);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.userBytes = r.regionBytes - 1u; // mapped view too small for the ring
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_USER_BYTES);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.userBytes = 0u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_USER_BYTES);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.userBase = 0u;
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_USER_BASE);
    }
    {
        CaptureControlConnectRequest r = valid();
        r.userBase = 0x1001ull; // not pointer-aligned
        EXPECT_EQ(CaptureControlValidateProfile(&r, r.regionBytes), CONTROL_PROFILE_BAD_USER_BASE);
    }
    {
        EXPECT_EQ(CaptureControlValidateProfile(nullptr, 0u), CONTROL_PROFILE_BAD_USER_BASE);
    }
}

TEST(CaptureControlPlaneTest, GuidLayoutMatchesDocumentedValue)
{
    unsigned char b[16] = {0};
    CaptureControlGuidFill(b);

    // {8ECC7B3A-4D29-4E7C-9611-A20C536E2140}
    const unsigned char expected[16] = {
        0x3A, 0x7B, 0xCC, 0x8E, // Data1 little-endian 0x8ECC7B3A
        0x29, 0x4D,             // Data2 little-endian 0x4D29
        0x7C, 0x4E,             // Data3 little-endian 0x4E7C
        0x96, 0x11, 0xA2, 0x0C, 0x53, 0x6E, 0x21, 0x40 // Data4
    };
    EXPECT_EQ(std::memcmp(b, expected, sizeof(expected)), 0);
}

TEST(CaptureControlPlaneTest, CapsStructFitsResponseBounding)
{
    EXPECT_LE(sizeof(CaptureControlCaps), CONTROL_MAX_RESPONSE_BYTES);
    EXPECT_LE(sizeof(CaptureControlConnectResponse), CONTROL_MAX_RESPONSE_BYTES);
    EXPECT_LE(sizeof(CaptureControlState), CONTROL_MAX_RESPONSE_BYTES);
    // Connect request is fixed size (nothing unbounded).
    EXPECT_EQ(sizeof(CaptureControlConnectRequest) % 8u, 0u);
}

} // namespace audient::capture_control