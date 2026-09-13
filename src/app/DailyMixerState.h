#pragma once

// Persistent Daily mixer/session state (v0.1.0).
//
// Kept intentionally separate from the small user-preferences schema
// (settings.json): this file carries mutable mixer/session state only.
//   %LOCALAPPDATA%\Audient Console\mixer-state.json
//   %LOCALAPPDATA%\Audient Console\State\<id>.vststate   (opaque VST3 blobs)
//
// Never touched from the ASIO callback. Writes are atomic (temp + replace) and
// a malformed/oversized file never blocks startup: callers fall back to an
// empty safe mixer.

#include <string>
#include <vector>

namespace audient::daily
{

struct MixerInsert
{
    std::string id;        // stable per-insert id (hex)
    std::string path;      // plug-in module path (may be missing on disk)
    std::string name;      // last known plug-in name (for display/diagnostics)
    bool bypass = false;   // host-side per-slot bypass
    std::string stateFile; // file name under State\ (empty = no saved state)
    bool missing = false;  // transient: was unavailable at last load (not serialized)
};

struct MixerChannel
{
    std::vector<MixerInsert> inserts; // ordered
    bool wholeBypass = false;
};

struct MixerState
{
    int schema = 1;
    MixerChannel ch0;
    MixerChannel ch1;
};

// %LOCALAPPDATA%\Audient Console (created if missing). Empty on failure.
std::string appDataDir();
// appDataDir()\mixer-state.json
std::string mixerStatePath();
// appDataDir()\State
std::string stateBlobDir();
// appDataDir()\State\<file>
std::string stateBlobPath(const std::string& fileName);

// Fresh random hex id for a new insert.
std::string newInsertId();

// Serialize to the documented JSON shape (deterministic field order).
std::string serializeMixerState(const MixerState& state);

// Parse a mixer-state JSON document. On malformed input returns false and
// leaves `out` at its defaults; the caller must start with a safe empty mixer.
bool parseMixerState(const std::string& json, MixerState& out, std::string& error);

// Load from disk. Missing file => returns false with error "not found" (caller
// treats as first run, not an error condition).
bool loadMixerState(const std::string& path, MixerState& out, std::string& error);

// Atomic save: write <path>.tmp, flush+close, then replace <path>.
bool saveMixerStateAtomic(const std::string& path, const MixerState& state, std::string& error);

// Whole-file read/write helpers for opaque VST3 blobs (binary safe).
bool readBinaryFile(const std::string& path, std::vector<unsigned char>& out);
bool writeBinaryFileAtomic(const std::string& path, const std::vector<unsigned char>& data,
                           std::string& error);

} // namespace audient::daily
