#pragma once

#include "asio/AsioTypes.h"
#include "channel/ChannelIdentity.h"
#include "channel/ChannelTypes.h"

#include <array>
#include <cstdint>
#include <vector>

namespace audient::asio
{

// One explicit stream input binding: physical identity, the resolved raw driver
// channel index, and the runtime slot it occupies. These are three DISTINCT
// concepts (plus collection position) that the binding table resolves at
// configuration time; the engine never conflates them (AGENTS §4; project plan
// Phase B mapping design). An invalid identity means the driver name could not
// be resolved to a canonical analogue input (explicit index, no guessed
// identity).
struct StreamInputBinding
{
    channel::ChannelIdentity identity{}; // canonical "Analog Input N" (invalid == unverified name)
    long asioChannelIndex = -1;          // resolved driver channel index for this stream input
    std::uint32_t runtimeSlot = 0;       // runtime processing slot this stream input occupies

    bool operator==(const StreamInputBinding&) const = default;
    bool operator!=(const StreamInputBinding&) const = default;
};

// Channel plan for one configured stream. The CANONICAL production input
// representation is the explicit binding table (inputs[0..inputCount)) ordered
// by runtime slot; `micInput` is a legacy single-uplink compatibility alias for
// inputs[0].asioChannelIndex that the channel maps keep in sync. A legacy
// caller that reassigns `micInput` directly is honored at configure time as an
// explicit single-input override (it never silently changes the table).
struct ChannelPlan
{
    static constexpr std::uint32_t kMaxInputs = channel::kMaxPhysicalInputChannels;

    std::array<StreamInputBinding, kMaxInputs> inputs{}; // canonical binding table
    std::uint32_t inputCount = 0;                        // number of live input bindings (0..kMaxInputs)

    long outputLeft = -1;
    long outputRight = -1;

    // Optional SECOND stereo output pair (e.g. a headphone bus driven by the
    // same program but with its own software gain/mute). -1 = absent; when >= 0
    // both channels of the pair must be valid. The ASIO request order is plan
    // order (input bindings in runtime-slot order, then outputLeft/Right, then
    // outputLeft2/Right2), so callback output slots 0/1 = the primary pair and
    // slots 2/3 = the secondary pair.
    long outputLeft2 = -1;
    long outputRight2 = -1;

    // Legacy single-uplink compatibility alias for inputs[0].asioChannelIndex
    // (kept in sync by the channel maps; -1 when no input is bound).
    long micInput = -1;

    // Number of input channels this plan requests (canonical table when
    // populated, else the legacy single alias).
    std::uint32_t requestedInputCount() const;

    // ASIO channel index bound to a runtime slot (-1 when not bound).
    long inputAsioIndex(std::uint32_t runtimeSlot) const;

    bool valid() const;
};

class AsioChannelMap
{
public:
    // Legacy single-uplink resolver (unchanged semantics): binds exactly one
    // primary analogue input to runtime slot 0.
    static ChannelPlan planMicUplinkAndStereoDownlink(const std::vector<ChannelInfo>& inputs,
                                                      const std::vector<ChannelInfo>& outputs,
                                                      bool allowUnverifiedDefault);

    // Production resolver: binds every resolvable analogue input (by canonical
    // name) to runtime slots in identity-ordinal order, independent of driver
    // enumeration position, index contiguity, or reversed ordering. iD14 v1
    // resolves exactly two; the resolver is not hard-coded to two. When no
    // analogue name resolves, allowUnverifiedDefault may bind a single active
    // input as an explicit unverified slot 0 (never a guessed identity).
    static ChannelPlan planProductionInputsAndStereoDownlink(const std::vector<ChannelInfo>& inputs,
                                                             const std::vector<ChannelInfo>& outputs,
                                                             bool allowUnverifiedDefault);
};

} // namespace audient::asio
