#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace audient::channel
{

// v1 product ceiling per input channel (product spec §4 / Phase B brief): the rack
// rejects a fifth slot. This is a PRODUCT rule; the reusable Vst3Chain engine
// keeps its own larger internal capability (Vst3Chain::kMaxSlots).
inline constexpr std::size_t kMaxSlotsPerChannel = 4;

// Engine capacity for physical input channels. v1 scope is two (iD14 MK1
// Analogue 1/2); the architecture is not hard-coded to two.
inline constexpr std::size_t kMaxPhysicalInputChannels = 8;

// Per-channel send state (Phase B foundation; the Phase C mixer consumes it).
// Unit/range/step of levelDb are specified with the mixer; v1 default -18 dB.
struct ChannelSendState
{
    bool enabled = false;
    float levelDb = -18.0f;
    bool muted = false;
    std::uint64_t revision = 0;

    bool operator==(const ChannelSendState&) const = default;
    bool operator!=(const ChannelSendState&) const = default;
};

// Control-side runtime configuration of one physical input channel. Every
// channel owns an independent instance and an independent revision counter:
// mutating channel i never touches channel j's object or revision.
struct ChannelRuntimeConfig
{
    std::uint64_t revision = 0;
    ChannelSendState virtualMicSend;    // virtual-mic send enable/level/mute
    ChannelSendState localMonitorSend;  // feed into the single v1 local-monitor mix (default OFF)

    bool operator==(const ChannelRuntimeConfig&) const = default;
    bool operator!=(const ChannelRuntimeConfig&) const = default;
};

struct ChannelRuntimeSnapshot
{
    ChannelSendState virtualMicSend;
    ChannelSendState localMonitorSend;
    std::uint64_t revision = 0;
};

inline float linearGain(const ChannelSendState& send)
{
    if (!send.enabled || send.muted)
    {
        return 0.0f;
    }
    float gain = std::pow(10.0f, send.levelDb / 20.0f);
    if (gain < 0.0f)
    {
        gain = 0.0f;
    }
    if (gain > 2.0f)
    {
        gain = 2.0f;
    }
    return gain;
}

// Default virtual-mic send for the v1 uplink channel (channel 0 / Analog Input
// 1): enabled at unity, preserving the current proven single-mic behavior until
// the Phase C mixer sums additional channels.
inline ChannelSendState defaultUplinkSendState()
{
    ChannelSendState send;
    send.enabled = true;
    send.levelDb = 0.0f;
    send.muted = false;
    return send;
}

} // namespace audient::channel
