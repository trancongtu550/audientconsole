#include "id14-control/semantic-model/Id14SemanticControl.h"

#include <cmath>
#include <cstddef>

namespace audient::id14
{

namespace
{

constexpr float speakerDbMin = -80.0f;
constexpr float speakerDbMax = 0.0f;
constexpr float speakerStep = 1.0f;

constexpr float levelNormalizedMin = 0.0f;
constexpr float levelNormalizedMax = 1.0f;

constexpr float panMin = -1.0f;
constexpr float panMax = 1.0f;
constexpr float panStep = 0.05f;

constexpr float cueSourceMin = 0.0f;
constexpr float cueSourceMax = 3.0f;
constexpr float cueSourceStep = 1.0f;

const ControlSpec kSpecs[] = {
    // kind                    name                        unit        min                    max                    step               default       persist  risky             rollback
    {Id14ControlKind::SpeakerLevel,      "Speaker volume",   "dB",       speakerDbMin,          speakerDbMax,          speakerStep,       -60.0f,      true, true,     true},
    {Id14ControlKind::SpeakerMute,       "Speaker mute",     "",         0.0f,                  1.0f,                  1.0f,              0.0f,       true, true,     true},
    {Id14ControlKind::Dim,               "Dim",              "dB",       -20.0f,                -6.0f,                 2.0f,              -12.0f,     false, true,     true},
    {Id14ControlKind::Mono,              "Mono",             "",         0.0f,                  1.0f,                  1.0f,              0.0f,       true, false,    true},
    {Id14ControlKind::HeadphoneLevel,    "Headphone volume", "dB",       speakerDbMin,          speakerDbMax,          speakerStep,       -60.0f,     true, true,     true},
    {Id14ControlKind::HeadphoneMute,     "Headphone mute",   "",         0.0f,                  1.0f,                  1.0f,              0.0f,       true, true,     true},
    {Id14ControlKind::PolarityInvert,    "Polarity invert",  "",         0.0f,                  1.0f,                  1.0f,              0.0f,       false, false,    true},
    {Id14ControlKind::CueSource,         "Cue source",       "index",    cueSourceMin,          cueSourceMax,          cueSourceStep,     0.0f,       true, false,    true},
    {Id14ControlKind::CueLevel,          "Cue level",        "dB",       speakerDbMin,          speakerDbMax,          speakerStep,       -60.0f,     true, true,     true},
    {Id14ControlKind::CueMute,           "Cue mute",         "",         0.0f,                  1.0f,                  1.0f,              0.0f,       true, true,     true},
    {Id14ControlKind::MainSpeakerRoute,  "Main route",       "index",    levelNormalizedMin,    levelNormalizedMax,    1.0f,              0.0f,       true, true,     true},
    {Id14ControlKind::HeadphoneRoute,    "Headphone route",  "index",    levelNormalizedMin,    levelNormalizedMax,    1.0f,              0.0f,       true, true,     true},
    {Id14ControlKind::OutputAssignment,  "Output assignment","index",    levelNormalizedMin,    levelNormalizedMax,    1.0f,              0.0f,       true, true,     true},
    {Id14ControlKind::ChannelFader,      "Channel fader",    "dB",       -80.0f,                6.0f,                  speakerStep,      0.0f,       true, true,     true},
    {Id14ControlKind::TalkbackLevel,     "Talkback level",   "dB",       -80.0f,                0.0f,                  speakerStep,      -12.0f,     false, true,     true},
    {Id14ControlKind::IdButtonMode,      "iD button mode",   "index",    levelNormalizedMin,    levelNormalizedMax,    1.0f,              0.0f,       true, false,    true},
};

constexpr std::size_t kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

} // namespace

Id14SupportClass supportFor(Id14ControlKind kind, Id14ControlTuple tuple)
{
    (void)tuple;
    // Nothing is proven on the exact MK1 tuple yet (no protocol evidence or
    // read-only/write proof exists). Every candidate stays non-writable until
    // ADR-005 gates are met (project guidelines §10). Physical-only controls remain
    // physical-only; firmware/unknown regions are forbidden outright.
    switch (kind)
    {
    case Id14ControlKind::SpeakerLevel:
    case Id14ControlKind::SpeakerMute:
    case Id14ControlKind::Dim:
    case Id14ControlKind::Mono:
    case Id14ControlKind::HeadphoneLevel:
    case Id14ControlKind::HeadphoneMute:
    case Id14ControlKind::PolarityInvert:
    case Id14ControlKind::CueSource:
    case Id14ControlKind::CueLevel:
    case Id14ControlKind::CueMute:
    case Id14ControlKind::MainSpeakerRoute:
    case Id14ControlKind::HeadphoneRoute:
    case Id14ControlKind::OutputAssignment:
    case Id14ControlKind::ChannelFader:
    case Id14ControlKind::TalkbackLevel:
    case Id14ControlKind::IdButtonMode:
        return Id14SupportClass::Unverified;
    }
    return Id14SupportClass::Unverified;
}

const ControlSpec* specFor(Id14ControlKind kind)
{
    for (const ControlSpec& spec : kSpecs)
    {
        if (spec.kind == kind)
        {
            return &spec;
        }
    }
    return nullptr;
}

bool valueInRange(Id14ControlKind kind, float value)
{
    const ControlSpec* spec = specFor(kind);
    return spec != nullptr && value >= spec->min && value <= spec->max;
}

bool alignedToStep(Id14ControlKind kind, float value)
{
    const ControlSpec* spec = specFor(kind);
    if (spec == nullptr)
    {
        return false;
    }
    const float offset = value - spec->min;
    const float rounded = std::round(offset / spec->step);
    return std::fabs(offset - rounded * spec->step) < 0.001f;
}

float clampToRange(Id14ControlKind kind, float value)
{
    const ControlSpec* spec = specFor(kind);
    if (spec == nullptr)
    {
        return value;
    }
    return value < spec->min ? spec->min : (value > spec->max ? spec->max : value);
}

float snapToStep(Id14ControlKind kind, float value)
{
    const ControlSpec* spec = specFor(kind);
    if (spec == nullptr)
    {
        return value;
    }
    const float clamped = clampToRange(kind, value);
    const float normalizedStep = std::round((clamped - spec->min) / spec->step);
    return spec->min + normalizedStep * spec->step;
}

std::size_t controlCount()
{
    return kSpecCount;
}

} // namespace audient::id14