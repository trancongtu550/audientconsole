#pragma once

#include <string>

namespace audient::id14
{

// Typed semantic controls for the Audient iD14 MK1 (v1). These are protocol-bytes
// independent by design (ADR-005): UI/session code addresses a Kind with a bounded
// value; only the verified adapter may translate a whitelisted command to raw bytes.

enum class Id14ControlKind
{
    SpeakerLevel,
    SpeakerMute,
    Dim,
    Mono,
    HeadphoneLevel,
    HeadphoneMute,
    PolarityInvert,
    CueSource,
    CueLevel,
    CueMute,
    MainSpeakerRoute,
    HeadphoneRoute,
    OutputAssignment,
    ChannelFader,
    TalkbackLevel,
    IdButtonMode,
};

enum class Id14SupportClass
{
    Supported,   // verified on the exact hardware/driver/firmware tuple
    ReadOnly,    // observable, not writable
    PhysicalOnly, // physical front-panel control, not software-accessible
    Unverified,  // candidate, not yet proven safe on the exact tuple
    Forbidden,   // must never be addressed (firmware/calibration/unknown regions)
};

// Support classification grid for the exact MK1 tuple
// (Audient iD14 MK1; driver `audientusbaudioasio_x64.dll` 5.72.0.0; firmware unknown).
// Entries are Not Run until protocol evidence exists (project guidelines §10.1/§10.2).
enum class Id14ControlTuple
{
    Mk1Current, // exact pinned tuple
};

Id14SupportClass supportFor(Id14ControlKind kind, Id14ControlTuple tuple);

struct ControlSpec
{
    Id14ControlKind kind;
    const char* name;
    const char* unit;

    float min;
    float max;
    float step;
    float defaultValue;

    bool isPersistent;
    bool isAudiblyRisky; // level/route changes that could produce a level step
    bool hasRollback;
};

const ControlSpec* specFor(Id14ControlKind kind);

bool valueInRange(Id14ControlKind kind, float value);
bool alignedToStep(Id14ControlKind kind, float value);
float clampToRange(Id14ControlKind kind, float value);
float snapToStep(Id14ControlKind kind, float value);
std::size_t controlCount();

} // namespace audient::id14