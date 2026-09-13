#pragma once

#include "channel/ChannelTypes.h"

namespace audient::channel
{

// ADR-011: selectable single-source virtual microphone. Input 1 == runtime slot
// 0 (default); Input 2 == runtime slot 1. There is no both/mix mode.
enum class VirtualMicSource : int
{
    Input1 = 0,
    Input2 = 1,
};

// The mutually-exclusive per-channel virtual-mic send for a source selection.
// Exactly one channel is enabled (Input 2 only when two inputs are configured).
// The unselected channel's send is explicitly disabled so it can never
// contribute to (or leak into) the mono virtual-mic uplink.
struct VirtualMicSends
{
    ChannelSendState channel0;
    ChannelSendState channel1;
};

inline VirtualMicSends virtualMicSendsFor(VirtualMicSource source, bool dualInputs)
{
    const bool useInput2 = dualInputs && source == VirtualMicSource::Input2;
    VirtualMicSends sends;
    sends.channel0.enabled = !useInput2;
    sends.channel0.levelDb = 0.0f;
    sends.channel0.muted = false;
    sends.channel0.revision = 1;
    sends.channel1.enabled = useInput2;
    sends.channel1.levelDb = 0.0f;
    sends.channel1.muted = false;
    sends.channel1.revision = 1;
    return sends;
}

} // namespace audient::channel
