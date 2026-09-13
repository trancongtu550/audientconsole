#include "asio/AsioChannelMap.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace audient::asio
{

namespace
{

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool containsAny(const std::string& haystack, const std::vector<std::string>& needles)
{
    for (const std::string& needle : needles)
    {
        if (haystack.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool activeChannel(const ChannelInfo& channel, std::size_t expectedIndex)
{
    return channel.index == static_cast<long>(expectedIndex) && channel.isActive;
}

// Fills outputLeft/outputRight (and the optional second pair) exactly as the
// legacy single-uplink resolver did. Shared by both resolvers.
void resolveOutputs(ChannelPlan& plan, const std::vector<ChannelInfo>& outputs, bool allowUnverifiedDefault)
{
    static const std::vector<std::string> kOutputLeftTokens = {"output 1", "out 1", "analog 1", "analogue 1", "monitor 1", "mon 1", "1"};
    bool foundLeft = false;
    for (std::size_t i = 0; i < outputs.size(); ++i)
    {
        if (outputs[i].isActive && containsAny(lower(outputs[i].name), kOutputLeftTokens))
        {
            plan.outputLeft = static_cast<long>(i);
            foundLeft = true;
            break;
        }
    }

    if (foundLeft)
    {
        static const std::vector<std::string> kOutputRightTokens = {"output 2", "out 2", "analog 2", "analogue 2", "monitor 2", "mon 2", "2"};
        const std::size_t leftIndex = static_cast<std::size_t>(plan.outputLeft);
        if (leftIndex + 1 < outputs.size() && activeChannel(outputs[leftIndex + 1], leftIndex + 1))
        {
            plan.outputRight = static_cast<long>(leftIndex + 1);
        }
        else
        {
            for (std::size_t i = 0; i < outputs.size(); ++i)
            {
                if (outputs[i].isActive && containsAny(lower(outputs[i].name), kOutputRightTokens))
                {
                    plan.outputRight = static_cast<long>(i);
                    break;
                }
            }
        }
    }

    if (plan.outputLeft < 0 && allowUnverifiedDefault && outputs.size() >= 2)
    {
        if (activeChannel(outputs[0], 0) && activeChannel(outputs[1], 1))
        {
            plan.outputLeft = 0;
            plan.outputRight = 1;
        }
    }

    if (plan.outputLeft < 0 || plan.outputRight < 0)
    {
        return;
    }

    // Optional second stereo output pair (slot 2/3 in the ASIO request = a
    // headphone bus). Resolve by name first; otherwise pick the next contiguous
    // active pair after the primary when the driver exposes >= 4 outputs.
    static const std::vector<std::string> kPair2LeftTokens = {"output 3", "out 3", "analog 3", "analogue 3",
                                                              "headphone 3", "hp 3", "line 3", "3"};
    static const std::vector<std::string> kPair2RightTokens = {"output 4", "out 4", "analog 4", "analogue 4",
                                                               "headphone 4", "hp 4", "line 4", "4"};
    const auto isAlreadyUsed = [&](long index) {
        return index == plan.outputLeft || index == plan.outputRight;
    };
    for (std::size_t i = 0; i < outputs.size() && plan.outputLeft2 < 0; ++i)
    {
        if (outputs[i].isActive && !isAlreadyUsed(static_cast<long>(i)) &&
            containsAny(lower(outputs[i].name), kPair2LeftTokens))
        {
            plan.outputLeft2 = static_cast<long>(i);
        }
    }
    if (plan.outputLeft2 >= 0)
    {
        for (std::size_t i = 0; i < outputs.size() && plan.outputRight2 < 0; ++i)
        {
            if (outputs[i].isActive && i != static_cast<std::size_t>(plan.outputLeft2) &&
                containsAny(lower(outputs[i].name), kPair2RightTokens))
            {
                plan.outputRight2 = static_cast<long>(i);
            }
        }
    }
    if (plan.outputRight2 < 0 && plan.outputLeft2 >= 0)
    {
        plan.outputLeft2 = -1; // incomplete name match: fall through to the numeric default
    }
    if (plan.outputLeft2 < 0)
    {
        const long secondBase = plan.outputLeft + 2;
        if (secondBase >= 0 && secondBase + 1 < static_cast<long>(outputs.size()) &&
            activeChannel(outputs[static_cast<std::size_t>(secondBase)], secondBase) &&
            activeChannel(outputs[static_cast<std::size_t>(secondBase + 1)], secondBase + 1))
        {
            plan.outputLeft2 = secondBase;
            plan.outputRight2 = secondBase + 1;
        }
    }
}

// Sets a single explicit slot-0 binding and keeps the legacy alias in sync.
void setSlot0Binding(ChannelPlan& plan, channel::ChannelIdentity identity, long asioChannelIndex)
{
    plan.inputCount = 1;
    plan.inputs[0].identity = identity;
    plan.inputs[0].asioChannelIndex = asioChannelIndex;
    plan.inputs[0].runtimeSlot = 0;
    plan.micInput = asioChannelIndex;
}

} // namespace

std::uint32_t ChannelPlan::requestedInputCount() const
{
    if (inputCount > 0)
    {
        return inputCount;
    }
    return micInput >= 0 ? 1u : 0u;
}

long ChannelPlan::inputAsioIndex(std::uint32_t runtimeSlot) const
{
    if (runtimeSlot >= inputCount || runtimeSlot >= kMaxInputs)
    {
        return -1;
    }
    return inputs[runtimeSlot].asioChannelIndex;
}

bool ChannelPlan::valid() const
{
    if (outputLeft < 0 || outputRight < 0)
    {
        return false;
    }
    // The secondary pair is optional; when present both halves must be real.
    if ((outputLeft2 < 0) != (outputRight2 < 0))
    {
        return false;
    }
    if (inputCount == 0)
    {
        return micInput >= 0; // legacy-only single alias (no canonical table)
    }
    if (inputCount > kMaxInputs)
    {
        return false;
    }

    for (std::uint32_t i = 0; i < inputCount; ++i)
    {
        const StreamInputBinding& binding = inputs[i];
        // Slots are stored in runtime-slot order (slot == array position).
        if (binding.runtimeSlot != i || binding.asioChannelIndex < 0)
        {
            return false;
        }
        for (std::uint32_t j = i + 1; j < inputCount; ++j)
        {
            // A driver index must not feed two runtime slots.
            if (binding.asioChannelIndex == inputs[j].asioChannelIndex)
            {
                return false;
            }
            // A duplicate identity must never be guessed between two slots.
            if (binding.identity.valid() && binding.identity == inputs[j].identity)
            {
                return false;
            }
        }
    }
    return true;
}

ChannelPlan AsioChannelMap::planMicUplinkAndStereoDownlink(const std::vector<ChannelInfo>& inputs,
                                                           const std::vector<ChannelInfo>& outputs,
                                                           bool allowUnverifiedDefault)
{
    ChannelPlan plan;

    static const std::vector<std::string> kInputTokens = {"input 1", "in 1", "analog 1", "analogue 1", "mic 1", "mono 1"};
    for (std::size_t i = 0; i < inputs.size(); ++i)
    {
        if (inputs[i].isActive && containsAny(lower(inputs[i].name), kInputTokens))
        {
            const long index = static_cast<long>(i);
            setSlot0Binding(plan, channel::parsePhysicalInputName(inputs[i].name), index);
            break;
        }
    }
    if (plan.inputCount == 0 && allowUnverifiedDefault && inputs.size() == 1)
    {
        if (activeChannel(inputs[0], 0))
        {
            // Explicit single unverified binding (index 0, identity unresolved).
            setSlot0Binding(plan, channel::ChannelIdentity{}, 0);
        }
    }

    resolveOutputs(plan, outputs, allowUnverifiedDefault);
    return plan;
}

ChannelPlan AsioChannelMap::planProductionInputsAndStereoDownlink(const std::vector<ChannelInfo>& inputs,
                                                                  const std::vector<ChannelInfo>& outputs,
                                                                  bool allowUnverifiedDefault)
{
    ChannelPlan plan;

    // Bind every resolvable analogue input by canonical identity, then order by
    // identity ordinal (NOT by enumeration position or index order). Reversed,
    // interleaved, or non-contiguous driver enumeration therefore cannot swap or
    // reorder channel identity.
    std::vector<std::pair<unsigned, long>> resolvable;
    for (const ChannelInfo& channel : inputs)
    {
        if (!channel.isActive)
        {
            continue;
        }
        const channel::ChannelIdentity identity = channel::parsePhysicalInputName(channel.name);
        if (identity.valid())
        {
            resolvable.push_back({identity.ordinal, channel.index});
        }
    }
    std::stable_sort(resolvable.begin(), resolvable.end(),
                     [](const std::pair<unsigned, long>& a, const std::pair<unsigned, long>& b) {
                         return a.first < b.first;
                     });

    if (resolvable.empty())
    {
        if (allowUnverifiedDefault && inputs.size() == 1 && activeChannel(inputs[0], 0))
        {
            // Explicit single unverified binding (no guessed identity).
            setSlot0Binding(plan, channel::ChannelIdentity{}, 0);
        }
        resolveOutputs(plan, outputs, allowUnverifiedDefault);
        return plan;
    }

    if (resolvable.size() > ChannelPlan::kMaxInputs)
    {
        // More resolvable analogue inputs than the engine can bind: refuse
        // rather than silently dropping channels.
        return plan;
    }

    for (std::size_t slot = 0; slot < resolvable.size(); ++slot)
    {
        const long asioIndex = resolvable[slot].second;
        StreamInputBinding& binding = plan.inputs[slot];
        binding.identity = channel::makeAnalogInput(resolvable[slot].first);
        binding.asioChannelIndex = asioIndex;
        binding.runtimeSlot = static_cast<std::uint32_t>(slot);
        plan.inputCount = static_cast<std::uint32_t>(slot + 1);
    }
    plan.micInput = plan.inputs[0].asioChannelIndex; // legacy alias: runtime slot 0

    resolveOutputs(plan, outputs, allowUnverifiedDefault);
    return plan;
}

} // namespace audient::asio
