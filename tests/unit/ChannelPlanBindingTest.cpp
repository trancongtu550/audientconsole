#include "asio/AsioChannelMap.h"
#include "channel/ChannelIdentity.h"
#include "channel/ChannelTypes.h"
#include "routing/RoutingTypes.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

using namespace audient::asio;
using audient::channel::ChannelIdentity;
using audient::channel::makeAnalogInput;

namespace
{

ChannelInfo in(long index, std::string name)
{
    ChannelInfo channel{};
    channel.index = index;
    channel.isInput = true;
    channel.isActive = true;
    channel.name = std::move(name);
    channel.preferredFormat = SampleFormat::Float32LE;
    return channel;
}

ChannelInfo out(long index, std::string name)
{
    ChannelInfo channel{};
    channel.index = index;
    channel.isInput = false;
    channel.isActive = true;
    channel.name = std::move(name);
    channel.preferredFormat = SampleFormat::Float32LE;
    return channel;
}

std::vector<ChannelInfo> stereoOutputs()
{
    std::vector<ChannelInfo> outputs;
    outputs.push_back(out(0, "Output 1"));
    outputs.push_back(out(1, "Output 2"));
    return outputs;
}

// Builds a plan straight from a binding table so duplicate/ordering rules can
// be tested at the ChannelPlan level (no resolver involved).
ChannelPlan planFromTable(std::array<StreamInputBinding, ChannelPlan::kMaxInputs> table, std::uint32_t count)
{
    ChannelPlan plan;
    plan.inputs = table;
    plan.inputCount = count;
    plan.outputLeft = 0;
    plan.outputRight = 1;
    plan.micInput = count > 0 ? plan.inputs[0].asioChannelIndex : -1;
    return plan;
}

} // namespace

static_assert(audient::routing::kMaxInputChannels == audient::channel::kMaxPhysicalInputChannels,
              "the routing processing ceiling and the channel domain ceiling must stay equal");

TEST(ChannelPlanBindingTest, ReversedNonContiguousEnumerationBindsByIdentityOrdinal)
{
    // #1/#2: driver reports "Analogue 2" at index 7 BEFORE "Analogue 1" at
    // index 4 (reversed order AND non-contiguous indexes). The production
    // resolver must bind by identity ordinal, never by position or index.
    std::vector<ChannelInfo> inputs;
    inputs.push_back(in(7, "Analogue 2"));
    inputs.push_back(in(4, "Analogue 1"));
    inputs.push_back(in(9, "S/PDIF")); // non-analogue name must be ignored

    const ChannelPlan plan = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), false);
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);

    EXPECT_EQ(plan.inputs[0].identity, makeAnalogInput(1)) << "runtime slot 0 must be Analog Input 1";
    EXPECT_EQ(plan.inputs[0].asioChannelIndex, 4L);
    EXPECT_EQ(plan.inputs[0].runtimeSlot, 0u);

    EXPECT_EQ(plan.inputs[1].identity, makeAnalogInput(2)) << "runtime slot 1 must be Analog Input 2";
    EXPECT_EQ(plan.inputs[1].asioChannelIndex, 7L);
    EXPECT_EQ(plan.inputs[1].runtimeSlot, 1u);

    EXPECT_EQ(plan.inputAsioIndex(0), 4L);
    EXPECT_EQ(plan.inputAsioIndex(1), 7L);
    EXPECT_EQ(plan.requestedInputCount(), 2u);
    EXPECT_EQ(plan.micInput, 4L) << "legacy alias tracks runtime slot 0";
}

TEST(ChannelPlanBindingTest, ContiguousButReversedEnumerationDoesNotSwapIdentity)
{
    // Reversed enumeration order with contiguous indexes 0/1: still resolves to
    // Analog Input 1 -> slot 0 (index 0), Analog Input 2 -> slot 1 (index 1).
    std::vector<ChannelInfo> inputs;
    inputs.push_back(in(1, "Analogue 2"));
    inputs.push_back(in(0, "Analogue 1"));

    const ChannelPlan plan = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), false);
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    EXPECT_EQ(plan.inputs[0].identity, makeAnalogInput(1));
    EXPECT_EQ(plan.inputs[0].asioChannelIndex, 0L);
    EXPECT_EQ(plan.inputs[1].identity, makeAnalogInput(2));
    EXPECT_EQ(plan.inputs[1].asioChannelIndex, 1L);
}

TEST(ChannelPlanBindingTest, InterleavedAdatChannelsDoNotBreakBinding)
{
    // Non-contiguous analogue indexes separated by other channel types.
    std::vector<ChannelInfo> inputs;
    inputs.push_back(in(0, "Analogue 1"));
    inputs.push_back(in(2, "ADAT 1")); // not a physical analogue input
    inputs.push_back(in(5, "Analogue 2"));

    const ChannelPlan plan = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), false);
    ASSERT_TRUE(plan.valid());
    ASSERT_EQ(plan.inputCount, 2u);
    EXPECT_EQ(plan.inputAsioIndex(0), 0L);
    EXPECT_EQ(plan.inputAsioIndex(1), 5L);
}

TEST(ChannelPlanBindingTest, MissingSecondAnalogueDegradesToSingleSlot)
{
    // #4: only one resolvable analogue input -> a valid single-slot plan (no
    // guess, no alias of any other channel into slot 1).
    std::vector<ChannelInfo> inputs;
    inputs.push_back(in(0, "Analogue 1"));
    inputs.push_back(in(1, "ADAT 1"));

    const ChannelPlan plan = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), false);
    ASSERT_TRUE(plan.valid());
    EXPECT_EQ(plan.inputCount, 1u);
    EXPECT_EQ(plan.inputs[0].identity, makeAnalogInput(1));
    EXPECT_EQ(plan.inputs[0].asioChannelIndex, 0L);
    EXPECT_EQ(plan.requestedInputCount(), 1u);
}

TEST(ChannelPlanBindingTest, DuplicateDriverIndexIsRejectedAtConfiguration)
{
    // #3: the same driver index must never feed two runtime slots.
    std::array<StreamInputBinding, ChannelPlan::kMaxInputs> table{};
    table[0].identity = makeAnalogInput(1);
    table[0].asioChannelIndex = 4;
    table[0].runtimeSlot = 0;
    table[1].identity = makeAnalogInput(2);
    table[1].asioChannelIndex = 4; // duplicate index
    table[1].runtimeSlot = 1;

    const ChannelPlan plan = planFromTable(table, 2);
    EXPECT_FALSE(plan.valid());
}

TEST(ChannelPlanBindingTest, DuplicateIdentityIsRejectedNeverGuessed)
{
    // #3: two slots may never both claim the same physical analogue input.
    std::array<StreamInputBinding, ChannelPlan::kMaxInputs> table{};
    table[0].identity = makeAnalogInput(2);
    table[0].asioChannelIndex = 1;
    table[0].runtimeSlot = 0;
    table[1].identity = makeAnalogInput(2); // duplicate ordinal
    table[1].asioChannelIndex = 5;
    table[1].runtimeSlot = 1;

    const ChannelPlan plan = planFromTable(table, 2);
    EXPECT_FALSE(plan.valid());
}

TEST(ChannelPlanBindingTest, RuntimeSlotMustMatchTablePosition)
{
    // Slots are stored in runtime-slot order (slot == array position); a plan
    // that claims otherwise is malformed.
    std::array<StreamInputBinding, ChannelPlan::kMaxInputs> table{};
    table[0].identity = makeAnalogInput(1);
    table[0].asioChannelIndex = 4;
    table[0].runtimeSlot = 1; // mismatch
    const ChannelPlan plan = planFromTable(table, 1);
    EXPECT_FALSE(plan.valid());
}

TEST(ChannelPlanBindingTest, UnverifiedSingleInputNeedsExplicitAllowance)
{
    // An unresolvable single input may bind as an explicit unverified slot 0
    // ONLY when allowUnverifiedDefault says so; the identity stays invalid.
    std::vector<ChannelInfo> inputs;
    inputs.push_back(in(0, "Mic Line"));

    const ChannelPlan reject = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), false);
    EXPECT_FALSE(reject.valid());

    const ChannelPlan allow = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), true);
    ASSERT_TRUE(allow.valid());
    ASSERT_EQ(allow.inputCount, 1u);
    EXPECT_FALSE(allow.inputs[0].identity.valid()) << "no identity may be guessed";
    EXPECT_EQ(allow.inputs[0].asioChannelIndex, 0L);
    EXPECT_EQ(allow.requestedInputCount(), 1u);
}

TEST(ChannelPlanBindingTest, ProductionDiscoveryNeverAutoSelectsUnverifiedInputs)
{
    // Invariant 2: discovery failure must never degrade into "assign an
    // arbitrary remaining ASIO input because only one channel is wanted".
    // Two active inputs whose names cannot be identity-verified produce an
    // INVALID plan even when the unverified allowance is enabled (the
    // allowance only ever covers ONE explicitly indexed channel).
    std::vector<ChannelInfo> inputs;
    inputs.push_back(in(0, "Preamp L"));
    inputs.push_back(in(1, "Preamp R"));

    const ChannelPlan strict = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), false);
    EXPECT_FALSE(strict.valid());

    const ChannelPlan permissive = AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, stereoOutputs(), true);
    EXPECT_FALSE(permissive.valid()) << "two unverifiable channels must not be auto-picked";
}

TEST(ChannelPlanBindingTest, LegacyResolverKeepsSingleSlotSemantics)
{
    // Backward compatibility: the legacy mono resolver binds exactly ONE input
    // to runtime slot 0 even when the driver exposes a second analogue input.
    std::vector<ChannelInfo> inputs;
    inputs.push_back(in(0, "Analog 1"));
    inputs.push_back(in(1, "Analog 2"));

    const ChannelPlan plan = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, stereoOutputs(), false);
    ASSERT_TRUE(plan.valid());
    EXPECT_EQ(plan.inputCount, 1u);
    EXPECT_EQ(plan.requestedInputCount(), 1u);
    EXPECT_EQ(plan.inputs[0].identity, makeAnalogInput(1));
    EXPECT_EQ(plan.inputs[0].asioChannelIndex, 0L);
    EXPECT_EQ(plan.micInput, 0L) << "legacy scalar alias preserved";
}
