#pragma once

// Pure signed Q8.8 dB helpers for the Audient iD14 volume path (ADR-008).
// No Windows/device dependency here so unit tests can exercise the exact
// encoding/guards without the device.

#include <cmath>
#include <cstdint>
#include <optional>

namespace audient::hardware::detail
{

// Conservative SET window. Floor measured on-device 2026-09-05: dragging each
// official slider fully down reads 0x8000 = -128.0 dB (int16 Q8.8 floor) on both
// Monitor and Headphones, and levels round-trip back up. Guard keeps 1 dB margin
// above that floor (-127) and stops 6 dB short of full-scale 0 dB (no accidental
// 0 dB / full-scale jump); the 0 dB end is not written until a loud-side test.
constexpr double kDbWriteFloor = -127.0;
constexpr double kDbWriteCeiling = -6.0;

// dB -> Q8.8 int16 (banker's rounding is fine here; device stores exact 1/256).
inline std::int16_t dbToQ88Raw(double db)
{
    return static_cast<std::int16_t>(std::lround(db * 256.0));
}

// Q8.8 int16 (as stored little-endian at payload offset 0) -> dB.
inline double q88RawToDb(std::uint16_t raw)
{
    return static_cast<double>(static_cast<std::int16_t>(raw)) / 256.0;
}

// A SET target must be finite and inside the conservative proven window.
inline bool validWriteDb(double db)
{
    return std::isfinite(db) && db >= kDbWriteFloor && db <= kDbWriteCeiling;
}

// A decoded READ is only plausible as an attenuation in [-128 dB, 0 dB]; anything
// positive is the "unsupported/stale" marker family (e.g. 0x0000) and must be
// rejected, never displayed as a real level.
inline bool plausibleReadDb(double db)
{
    return std::isfinite(db) && db >= -128.0 && db <= 0.0;
}

// Decode+validate a raw payload value for display/state. Returns nullopt when the
// raw is not a plausible attenuation.
inline std::optional<double> validateReadRaw(std::uint16_t raw)
{
    const double db = q88RawToDb(raw);
    if (!plausibleReadDb(db))
    {
        return std::nullopt;
    }
    return db;
}

} // namespace audient::hardware::detail
