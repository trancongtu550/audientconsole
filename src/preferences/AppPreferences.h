#pragma once

#include <string>

namespace audient::preferences
{

// Selectable single-source virtual microphone (ADR-011): 0 == Input 1
// (runtime slot 0, default), 1 == Input 2 (runtime slot 1). Stored as a plain
// bounded int so the preferences layer stays decoupled from the audio types.
inline constexpr int kVirtualMicSourceInput1 = 0;
inline constexpr int kVirtualMicSourceInput2 = 1;

// Presentation-only view mode (Daily GUI). 0 = Main Mixer, 1 = Mini Monitor.
// Never affects audio/engine state; stored so the chosen view survives a restart.
inline constexpr int kUiModeMain = 0;
inline constexpr int kUiModeMini = 1;

// Bounded, user-only application preferences. This intentionally holds ONLY UI/
// product preferences. It must never carry runtime or device source-of-truth
// values (actual sample rate, actual ASIO buffer, ASIO/Pico connection state,
// xruns, overloads, hardware monitor/headphone dB) — those always come from the
// live runtime/driver.
struct AppPreferences
{
    int virtualMicSource = kVirtualMicSourceInput1; // ADR-011 (0/1)
    bool closeToTray = true;
    bool startMinimized = false;  // next launch starts hidden (no autostart needed)
    int uiMode = kUiModeMain;     // 0 = Main Mixer, 1 = Mini Monitor

    bool operator==(const AppPreferences&) const = default;
};

// Versioned schema of the on-disk file. Unknown keys are ignored and a newer
// schema is read with best-effort defaults (never overwritten destructively by
// older builds). The schema is intentionally flat and bounded.
inline constexpr int kPreferencesSchema = 1;

// Pure serialization (no I/O) — unit-testable.
std::string serializePreferences(const AppPreferences& preferences);

// Tolerant, bounded parser. Returns safe defaults on a missing/corrupt/invalid
// document instead of throwing. Unknown keys and value types are skipped. When
// `warning` is non-null it receives a short human-readable reason on fallback.
AppPreferences parsePreferences(const std::string& text, std::string* warning = nullptr);

// Default file: %LOCALAPPDATA%\Audient Console\settings.json (empty when the
// environment has no LOCALAPPDATA, e.g. non-Windows test hosts).
std::string defaultPreferencesPath();

// File I/O. Writes are atomic where practical (temp file + rename). All
// functions report failure via the return value / `error` and NEVER throw, so a
// persistence problem can never prevent audio startup.
bool loadPreferencesFromFile(const std::string& path, AppPreferences& out, std::string* error = nullptr);
bool savePreferencesToFile(const std::string& path, const AppPreferences& preferences, std::string* error = nullptr);

bool loadPreferences(AppPreferences& out, std::string* error = nullptr);
bool savePreferences(const AppPreferences& preferences, std::string* error = nullptr);

const char* virtualMicSourceName(int source);

} // namespace audient::preferences
