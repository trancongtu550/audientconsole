#pragma once

#include <string>

namespace audient::channel
{

// Stable logical identity of one physical input channel, independent of the raw
// ASIO channel index (project plan Phase B mapping design). v1 resolves the iD14 MK1
// analogue input names ("Analogue 1", "Analogue 2" and equivalent tokens) to
// canonical "Analog Input N" labels. Output/ADAT and unknown names do not
// resolve; an invalid identity means "not a physical analogue input".
struct ChannelIdentity
{
    std::string stableName; // canonical label, e.g. "Analog Input 1"
    unsigned ordinal = 0;   // 1-based physical analogue ordinal (1, 2, ...)

    bool valid() const { return !stableName.empty() && ordinal != 0u; }

    bool operator==(const ChannelIdentity&) const = default;
    bool operator!=(const ChannelIdentity&) const = default;
};

// Canonical identity for a physical analogue input ordinal (1-based).
ChannelIdentity makeAnalogInput(unsigned ordinal);

// Resolves one driver channel name to a physical analogue input identity.
// Returns an invalid identity when the name is not a resolvable analogue input
// (ADAT, output/headphone, unknown, empty, malformed, out-of-range ordinal).
ChannelIdentity parsePhysicalInputName(const std::string& channelName);

} // namespace audient::channel
