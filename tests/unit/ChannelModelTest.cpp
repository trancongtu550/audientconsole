#include "channel/ChannelCollection.h"
#include "channel/ChannelIdentity.h"
#include "channel/ChannelRack.h"
#include "channel/ChannelTypes.h"
#include "channel/InputChannel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using audient::channel::ChannelCollection;
using audient::channel::ChannelIdentity;
using audient::channel::ChannelRack;
using audient::channel::ChannelRackResult;
using audient::channel::ChannelSendState;
using audient::channel::ChannelRuntimeConfig;
using audient::channel::InputChannel;
using audient::channel::makeAnalogInput;
using audient::channel::parsePhysicalInputName;

namespace audient_channel_model_test
{

TEST(ChannelIdentityTest, ParsesIAnalogueInputNamesToCanonicalIdentity)
{
    const ChannelIdentity one = parsePhysicalInputName("Analogue 1");
    ASSERT_TRUE(one.valid());
    EXPECT_EQ(one, makeAnalogInput(1));
    EXPECT_EQ(one.stableName, "Analog Input 1");
    EXPECT_EQ(one.ordinal, 1u);

    const ChannelIdentity two = parsePhysicalInputName("Analogue 2");
    ASSERT_TRUE(two.valid());
    EXPECT_EQ(two, makeAnalogInput(2));
    EXPECT_EQ(two.stableName, "Analog Input 2");
}

TEST(ChannelIdentityTest, ParsesEquivalentInputNameTokens)
{
    const std::vector<std::string> names = {"analog 1", "ANALOG 2", "Input 1", "Mic 1",
                                            "Microphone 2", "Line 1", "Mono 2"};
    const std::vector<ChannelIdentity> expected = {makeAnalogInput(1), makeAnalogInput(2), makeAnalogInput(1),
                                                   makeAnalogInput(1), makeAnalogInput(2), makeAnalogInput(1),
                                                   makeAnalogInput(2)};
    ASSERT_EQ(names.size(), expected.size());
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        const ChannelIdentity resolved = parsePhysicalInputName(names[i]);
        ASSERT_TRUE(resolved.valid()) << names[i];
        EXPECT_EQ(resolved, expected[i]) << names[i];
    }
}

TEST(ChannelIdentityTest, RejectsNonPhysicalInputNames)
{
    const std::vector<std::string> names = {"ADAT 1", "ADAT 8", "Output 1", "Out 1",
                                            "Headphone 1", "Speaker L", "Monitor 1",
                                            "Cue 1", "S/PDIF 1", "Analogue 0", "Analogue 65",
                                            "", "   ", "unknown"};
    for (const std::string& name : names)
    {
        const ChannelIdentity resolved = parsePhysicalInputName(name);
        EXPECT_FALSE(resolved.valid()) << name;
        EXPECT_EQ(resolved.stableName, std::string()) << name;
        EXPECT_EQ(resolved.ordinal, 0u) << name;
    }
}

TEST(ChannelIdentityTest, ResolutionIsDeterministicAndOrderIndependent)
{
    std::mt19937 rng(42);
    std::vector<std::string> names = {"Analogue 1", "Analogue 2", "ADAT 1", "ADAT 2",
                                      "Output 1", "Output 2"};
    for (int round = 0; round < 20; ++round)
    {
        std::shuffle(names.begin(), names.end(), rng);
        for (const std::string& name : names)
        {
            const ChannelIdentity first = parsePhysicalInputName(name);
            const ChannelIdentity second = parsePhysicalInputName(name);
            EXPECT_EQ(first, second) << name;
        }
    }
}

TEST(ChannelIdentityTest, MakeAnalogInputRejectsZeroAndOversizedOrdinal)
{
    EXPECT_FALSE(makeAnalogInput(0).valid());
    EXPECT_FALSE(makeAnalogInput(65).valid());
    EXPECT_TRUE(makeAnalogInput(1).valid());
    EXPECT_TRUE(makeAnalogInput(64).valid());
}

TEST(ChannelRackTest, RejectsFifthSlotAtProductCeiling)
{
    ChannelRack rack;
    EXPECT_TRUE(rack.empty());
    EXPECT_FALSE(rack.full());
    for (std::size_t i = 0; i < ChannelRack::kMaxSlots; ++i)
    {
        EXPECT_EQ(rack.add("plug " + std::to_string(i)), ChannelRackResult::Ok);
    }
    EXPECT_EQ(rack.size(), ChannelRack::kMaxSlots);
    EXPECT_TRUE(rack.full());
    EXPECT_EQ(rack.add("plug 5"), ChannelRackResult::Full);
    EXPECT_EQ(rack.size(), ChannelRack::kMaxSlots);
    EXPECT_EQ(rack.slot(ChannelRack::kMaxSlots), nullptr);
    EXPECT_EQ(rack.slot(ChannelRack::kMaxSlots - 1)->name, "plug 3");
}

TEST(ChannelRackTest, RemoveReorderAndBypassStayInBounds)
{
    ChannelRack rack;
    ASSERT_EQ(rack.add("a"), ChannelRackResult::Ok);
    ASSERT_EQ(rack.add("b"), ChannelRackResult::Ok);
    ASSERT_EQ(rack.add("c"), ChannelRackResult::Ok);

    EXPECT_EQ(rack.removeAt(7), ChannelRackResult::NotFound);
    EXPECT_EQ(rack.move(0, 0), ChannelRackResult::Invalid);
    EXPECT_EQ(rack.move(0, 7), ChannelRackResult::NotFound);
    EXPECT_EQ(rack.setBypass(7, true), ChannelRackResult::NotFound);
    EXPECT_EQ(rack.slot(7), nullptr);

    EXPECT_EQ(rack.move(0, 2), ChannelRackResult::Ok);
    ASSERT_NE(rack.slot(0), nullptr);
    EXPECT_EQ(rack.slot(0)->name, "b");
    EXPECT_EQ(rack.slot(2)->name, "a");

    EXPECT_EQ(rack.setBypass(1, true), ChannelRackResult::Ok);
    EXPECT_TRUE(rack.slot(1)->bypass);
    EXPECT_FALSE(rack.slot(0)->bypass);
    EXPECT_FALSE(rack.wholeChainBypass());

    rack.setWholeChainBypass(true);
    EXPECT_TRUE(rack.wholeChainBypass());

    EXPECT_EQ(rack.removeAt(2), ChannelRackResult::Ok);
    EXPECT_EQ(rack.size(), 2u);
    rack.clear();
    EXPECT_TRUE(rack.empty());
    EXPECT_FALSE(rack.wholeChainBypass());
}

TEST(ChannelRackTest, SetMissingFlagsPlaceholderState)
{
    ChannelRack rack;
    ASSERT_EQ(rack.add("missing-plugin"), ChannelRackResult::Ok);
    EXPECT_EQ(rack.setMissing(0, true), ChannelRackResult::Ok);
    ASSERT_NE(rack.slot(0), nullptr);
    EXPECT_TRUE(rack.slot(0)->missing);
    EXPECT_EQ(rack.setMissing(5, true), ChannelRackResult::NotFound);
}

TEST(InputChannelStateIsolationTest, MutatingOneChannelNeverTouchesAnother)
{
    InputChannel channelA(makeAnalogInput(1));
    InputChannel channelB(makeAnalogInput(2));

    const audient::channel::ChannelSendState originalA = channelA.virtualMicSend();
    const audient::channel::ChannelSendState originalB = channelB.virtualMicSend();
    const audient::channel::ChannelSendState originalMonitorB = channelB.localMonitorSend();
    EXPECT_EQ(originalA, channelA.virtualMicSend());
    EXPECT_EQ(originalB, channelB.virtualMicSend());

    audient::channel::ChannelSendState changedA;
    changedA.enabled = true;
    changedA.levelDb = -6.0f;
    changedA.muted = true;
    changedA.revision = 0;

    const std::uint64_t revisionBefore = channelB.revision();
    channelA.setVirtualMicSend(changedA);
    channelA.setLocalMonitorSend(changedA);

    EXPECT_EQ(channelA.virtualMicSend(), changedA);
    EXPECT_EQ(channelA.localMonitorSend(), changedA);
    EXPECT_GT(channelA.revision(), 0u);

    EXPECT_EQ(channelB.virtualMicSend(), originalB);
    EXPECT_EQ(channelB.localMonitorSend(), originalMonitorB);
    EXPECT_EQ(channelB.revision(), revisionBefore);
    EXPECT_NE(channelA.virtualMicSend(), channelB.virtualMicSend());

    const auto snapshotA = channelA.runtimeSnapshot();
    const auto snapshotB = channelB.runtimeSnapshot();
    EXPECT_EQ(snapshotA.revision, channelA.revision());
    EXPECT_EQ(snapshotA.virtualMicSend, channelA.virtualMicSend());
    EXPECT_EQ(snapshotA.localMonitorSend, channelA.localMonitorSend());
    EXPECT_EQ(snapshotB.revision, channelB.revision());
}

TEST(InputChannelStateIsolationTest, MutateRuntimeBumpsOnlyThatChannelRevision)
{
    InputChannel channelA(makeAnalogInput(1));
    InputChannel channelB(makeAnalogInput(2));
    const std::uint64_t revisionB = channelB.revision();

    channelA.mutateRuntime([](audient::channel::ChannelRuntimeConfig& config) {
        config.localMonitorSend.enabled = true;
        config.localMonitorSend.levelDb = -18.0f;
        config.virtualMicSend.muted = true;
    });

    EXPECT_EQ(channelA.revision(), 1u);
    EXPECT_TRUE(channelA.localMonitorSend().enabled);
    EXPECT_TRUE(channelA.virtualMicSend().muted);
    EXPECT_EQ(channelB.revision(), revisionB);
    EXPECT_FALSE(channelB.localMonitorSend().enabled);
    EXPECT_FALSE(channelB.virtualMicSend().muted);
}

TEST(ChannelCollectionTest, HoldsIndependentChannelsWithStableIdentityOrder)
{
    std::vector<ChannelIdentity> identities;
    identities.push_back(makeAnalogInput(1));
    identities.push_back(makeAnalogInput(2));

    ChannelCollection collection(std::move(identities));
    EXPECT_EQ(collection.activeCount(), 2u);
    EXPECT_FALSE(collection.empty());
    EXPECT_EQ(collection.channel(0).identity(), makeAnalogInput(1));
    EXPECT_EQ(collection.channel(1).identity(), makeAnalogInput(2));
    EXPECT_EQ(collection.channel(0).channelName(), "Analog Input 1");
    EXPECT_THROW(collection.channel(2), std::out_of_range);
    EXPECT_THROW(collection.channel(99), std::out_of_range);

    collection.channel(0).setVirtualMicSend(audient::channel::defaultUplinkSendState());
    EXPECT_TRUE(collection.channel(0).virtualMicSend().enabled);
    EXPECT_EQ(collection.channel(0).virtualMicSend().levelDb, 0.0f);
    EXPECT_FALSE(collection.channel(1).virtualMicSend().enabled);
    EXPECT_EQ(collection.channel(1).revision(), 0u);
}

TEST(ChannelCollectionTest, RejectsEmptyAndOversizedCollections)
{
    EXPECT_THROW(ChannelCollection({}), std::invalid_argument);

    std::vector<ChannelIdentity> tooMany;
    for (unsigned i = 1; i <= audient::channel::kMaxPhysicalInputChannels + 1; ++i)
    {
        tooMany.push_back(makeAnalogInput(i));
    }
    EXPECT_THROW(ChannelCollection(std::move(tooMany)), std::invalid_argument);

    std::vector<ChannelIdentity> invalid;
    invalid.push_back(ChannelIdentity{});
    EXPECT_THROW(ChannelCollection(std::move(invalid)), std::invalid_argument);
}

} // namespace audient_channel_model_test
