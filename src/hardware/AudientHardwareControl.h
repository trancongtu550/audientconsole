#pragma once

// AudientHardwareControl - shared production module for the proven iD14 MK1
// Monitor/Headphone VOLUME path (ADR-008). The module dynamically loads the
// official Audient user-mode API dll, detects the exact MK1 device, and exposes
// semantic dB get/set only. Raw entity/control values never leave this module.
//
// Scope: Monitor + Headphone VOLUME. Mute/dim/mono/cue/output-assignment/iD-button
// are deliberately NOT implemented here (ADR-008 scope note).
//
// Concurrency: a low-rate non-realtime poll thread (5 Hz / 200 ms production
// default; configurable for diagnostics) reads the current device state for UI
// synchronization. All API calls are serialized behind an internal mutex;
// nothing here is ever called from the ASIO callback. After a physical reconnect
// the module re-opens and re-reads the device - the hardware is the source of
// truth and stale software volumes are never pushed back automatically. Software
// SET results update the cached UI state synchronously (no wait for the poll).
// 5 Hz was chosen from A/B/C/D pop evidence (2026-09-06): 15 Hz measurably
// aggravates USB/passthrough glitches on the test VM; 5 Hz matches the no-poll
// baseline. The remaining occasional pop is VM/environmental, not a product
// regression.

#include <cstdint>
#include <optional>
#include <string>

namespace audient::hardware
{

enum class Result
{
    Ok,
    NotOpen,       // open() never succeeded (or close() called)
    DllLoadFailed, // official Audient API dll not found/loadable
    ExportMissing, // dll loaded but a required export is absent
    DeviceNotFound, // no iD14 MK1 (VID_2708/PID_0002) present
    DeviceAmbiguous, // more than one MK1 present; refusing to guess
    OpenFailed,    // API reported the device but the open call failed
    Disconnected,  // device not currently reachable; no write attempted
    InvalidDb,     // value outside the conservative proven SET window
    WriteFailed,   // SET call returned an error (no retry issued)
    VerifyFailed,  // read-back after SET disagreed; no further writes issued
    InternalError,
};

const char* resultName(Result result);

class AudientHardwareControl
{
public:
    AudientHardwareControl();
    ~AudientHardwareControl();

    AudientHardwareControl(const AudientHardwareControl&) = delete;
    AudientHardwareControl& operator=(const AudientHardwareControl&) = delete;

    // Loads the official API dll, verifies exactly one iD14 MK1 is present, opens
    // it and starts the read poll thread. Idempotent when already open. On any
    // failure the object stays closed and returns a reason; the caller may retry.
    Result open();

    // Stops the poll thread, closes the device and unloads the dll.
    void close();

    // True while the poll thread is active (open() succeeded).
    bool isOpen() const;

    // True when the module currently holds a reachable device handle.
    bool connected() const;

// Poll cadence for the read thread. Production default is 200 ms (5 Hz). Call
// before open(); clamped to [10, 5000] ms. 5 Hz gives ~200-300 ms encoder
// tracking for the UI while matching the no-poll audio baseline (A/B/C/D).
    void setPollPeriodMs(unsigned periodMs);
    unsigned pollPeriodMs() const;

    // Diagnostics: wall time of the last read tick and total successful ticks.
    double lastPollMs() const;
    unsigned long long pollTicks() const;

    // Most recent device state as read from hardware (monitor / headphones dB).
    // Returns nullopt when disconnected or when the raw value was implausible.
    std::optional<double> monitorDb() const;
    std::optional<double> headphoneDb() const;

    // Semantic dB SET. Validates the value, writes with the proven convention
    // (monitor: single SET arg5=0x00; headphone: official 3+4 pair), then reads
    // back. Returns Ok only when the read-back agrees within tolerance; a
    // mismatch returns VerifyFailed and NO further write is attempted.
    Result setMonitorDb(double db);
    Result setHeadphoneDb(double db);

    // Last result detail (device-facing errors only; empty on Ok).
    std::string lastError() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;

    static void pollWorker(AudientHardwareControl& owner);
    static void pollOnce(AudientHardwareControl& owner);
    static void markDeviceLostLocked(Impl& impl);
    static void tryReopenLocked(Impl& impl);
    static int readLevelLocked(Impl& impl, std::uint32_t entity, std::uint32_t control,
                               std::uint32_t arg5, double* outDb);

    Result setVolumeDb(double db, bool headphones);
};

} // namespace audient::hardware
