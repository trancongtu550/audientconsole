// Unit coverage for the pure signed Q8.8 volume codec + conservative SET-window
// guards (ADR-008). No device/API calls here - the module's device path is gated
// by manual hardware acceptance.

#include "hardware/AudientHardwareControl.h"
#include "hardware/VolumeCodec.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace audient::hardware::detail;

namespace
{

constexpr std::int16_t kI16Min = std::numeric_limits<std::int16_t>::min();
constexpr std::int16_t kI16Max = std::numeric_limits<std::int16_t>::max();

} // namespace

TEST(VolumeCodecTest, Q88RoundTripIsExactOnTheRawGrid)
{
    for (int16_t raw : {static_cast<int16_t>(0xDFD2), static_cast<int16_t>(0xD7DB),
                        static_cast<int16_t>(0xD72A), static_cast<int16_t>(0xDED2),
                        static_cast<int16_t>(0xEBB7), static_cast<int16_t>(0xCE0B)})
    {
        const double db = q88RawToDb(static_cast<std::uint16_t>(raw));
        EXPECT_LE(db, 0.0);
        EXPECT_NEAR(db, static_cast<double>(raw) / 256.0, 1e-9);
        EXPECT_EQ(dbToQ88Raw(db), raw);
    }
}

TEST(VolumeCodecTest, SignedDecodeIsNeverUnsigned)
{
    // 0xEBB7 must decode as a negative value (-5193/256 dB), never +43577/256.
    const double db = q88RawToDb(0xEBB7);
    EXPECT_LT(db, 0.0);
    EXPECT_NEAR(db, -5193.0 / 256.0, 1e-9);
}

TEST(VolumeCodecTest, RoundingIsNearest)
{
    // -32.18 * 256 = -8238.08 -> -8238 (0xDFD2), matching the proven monitor read.
    EXPECT_EQ(dbToQ88Raw(-32.18), static_cast<int16_t>(0xDFD2));
    EXPECT_EQ(dbToQ88Raw(-33.18), static_cast<int16_t>(0xDED2));
    EXPECT_EQ(dbToQ88Raw(-40.0), static_cast<int16_t>(-10240));
}

TEST(VolumeCodecTest, WriteWindowRejectsHotOrDeepValues)
{
    EXPECT_FALSE(validWriteDb(0.0));
    EXPECT_FALSE(validWriteDb(-1.0));
    EXPECT_FALSE(validWriteDb(-5.99));
    EXPECT_FALSE(validWriteDb(-128.0)); // below the -127 guard (1 dB above device floor)
    EXPECT_FALSE(validWriteDb(-1000.0));
    EXPECT_FALSE(validWriteDb(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(validWriteDb(std::numeric_limits<double>::infinity()));

    EXPECT_TRUE(validWriteDb(kDbWriteCeiling));
    EXPECT_TRUE(validWriteDb(kDbWriteFloor));
    EXPECT_TRUE(validWriteDb(-32.18));
    EXPECT_TRUE(validWriteDb(-40.0));
    EXPECT_TRUE(validWriteDb(-100.0));
}

TEST(VolumeCodecTest, WriteWindowHasSafeMarginAboveDeviceFloor)
{
    // Measured floor is 0x8000 = -128.0 dB; the guard sits 1 dB above it and
    // never equals the int16 minimum.
    EXPECT_GT(dbToQ88Raw(kDbWriteFloor), std::numeric_limits<std::int16_t>::min());
    EXPECT_GT(dbToQ88Raw(kDbWriteFloor), dbToQ88Raw(-128.0));
    EXPECT_EQ(q88RawToDb(0x8000), -128.0);
}

TEST(VolumeCodecTest, WindowCanNeverOverflowInt16)
{
    // Sanity: the whole allowed window maps to well-inside int16.
    for (double db = kDbWriteFloor; db <= kDbWriteCeiling; db += 0.5)
    {
        const std::int16_t raw = dbToQ88Raw(db);
        EXPECT_GT(raw, kI16Min);
        EXPECT_LT(raw, kI16Max);
    }
    EXPECT_GT(dbToQ88Raw(kDbWriteFloor), kI16Min);
    EXPECT_LT(dbToQ88Raw(kDbWriteCeiling), 0);
}

TEST(VolumeCodecTest, ReadValidationRejectsPositiveOrStaleZero)
{
    // 0x0000 decodes to 0.0 dB, a legitimate attenuation boundary for READ
    // display (the probe's HP "0x0000" came from a wrong read mode, not from
    // decoding). It is never a valid SET target (ceiling guard rejects >= -12).
    const auto zero = validateReadRaw(0x0000);
    ASSERT_TRUE(zero.has_value());
    EXPECT_NEAR(*zero, 0.0, 1e-9);

    // Positive raw is never an attenuation.
    EXPECT_FALSE(validateReadRaw(0x7FFF).has_value());
    EXPECT_FALSE(validateReadRaw(0x0100).has_value()); // +1 dB is not an attenuation

    // Real attenuations decode fine.
    const auto db = validateReadRaw(0xD7DB);
    ASSERT_TRUE(db.has_value());
    EXPECT_NEAR(*db, -10277.0 / 256.0, 1e-9);
}

TEST(VolumeCodecTest, ResultNamesAreStable)
{
    using audient::hardware::Result;
    EXPECT_STREQ(audient::hardware::resultName(Result::Ok), "Ok");
    EXPECT_STREQ(audient::hardware::resultName(Result::VerifyFailed), "VerifyFailed");
    EXPECT_STREQ(audient::hardware::resultName(Result::InvalidDb), "InvalidDb");
    EXPECT_STREQ(audient::hardware::resultName(Result::DeviceAmbiguous), "DeviceAmbiguous");
}
