#include "asio/AsioChannelMap.h"
#include "asio/AsioDeviceState.h"
#include "asio/AudientDriverMatcher.h"

#include <gtest/gtest.h>

#include <vector>

using namespace audient::asio;

TEST(AudientDriverMatcherTest, PrefersRememberedClsid)
{
    AudientDriverMatcher matcher;
    DriverIdentity first{"{00000000-0000-0000-0000-000000000001}", "Audient iD14 ASIO"};
    DriverIdentity second{"{00000000-0000-0000-0000-000000000002}", "Some other ASIO"};

    matcher.rememberPreferredIdentity(second.clsid);
    DriverIdentity matched;
    ASSERT_TRUE(matcher.match({first, second}, matched));
    EXPECT_EQ(matched.clsid, second.clsid);
}

TEST(AudientDriverMatcherTest, FallsBackToHumanReadableName)
{
    AudientDriverMatcher matcher;
    const std::vector<DriverIdentity> available = {
        {"{A}", "Generic ASIO Driver"},
        {"{B}", "Audient iD14 USB ASIO Driver"},
    };

    DriverIdentity matched;
    ASSERT_TRUE(matcher.match(available, matched));
    EXPECT_EQ(matched.clsid, "{B}");
}

TEST(AudientDriverMatcherTest, NoMatchReturnsFalse)
{
    AudientDriverMatcher matcher;
    DriverIdentity other{"{A}", "Focusrite USB ASIO Driver"};
    DriverIdentity out;
    EXPECT_FALSE(matcher.match({other}, out));
    EXPECT_FALSE(matcher.match({}, out));
}

TEST(AudientDriverMatcherTest, NameMatchIsCaseInsensitive)
{
    AudientDriverMatcher matcher;
    DriverIdentity audient{"{B}", "audient iD14 MK1 driver"};
    DriverIdentity out;
    EXPECT_TRUE(matcher.match({audient}, out));
}

TEST(AsioChannelMapTest, MapsMicToInputOneAndStereoOutput)
{
    std::vector<ChannelInfo> inputs;
    inputs.push_back({0, true, true, "Analog 1", SampleFormat::Float32LE});
    inputs.push_back({1, true, true, "Analog 2", SampleFormat::Float32LE});

    std::vector<ChannelInfo> outputs;
    outputs.push_back({0, false, true, "Analog 1", SampleFormat::Float32LE});
    outputs.push_back({1, false, true, "Analog 2", SampleFormat::Float32LE});

    const ChannelPlan plan = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
    ASSERT_TRUE(plan.valid());
    EXPECT_EQ(plan.micInput, 0);
    EXPECT_EQ(plan.outputLeft, 0);
    EXPECT_EQ(plan.outputRight, 1);
}

TEST(AsioChannelMapTest, RejectsAmbiguousNames)
{
    std::vector<ChannelInfo> inputs;
    inputs.push_back({0, true, true, "Left", SampleFormat::Float32LE});
    inputs.push_back({1, true, true, "Right", SampleFormat::Float32LE});

    std::vector<ChannelInfo> outputs;
    outputs.push_back({0, false, true, "Left", SampleFormat::Float32LE});
    outputs.push_back({1, false, true, "Right", SampleFormat::Float32LE});

    const ChannelPlan plan = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
    EXPECT_FALSE(plan.valid());
}

TEST(AsioChannelMapTest, SingleInputResolvesWithUnverifiedDefault)
{
    std::vector<ChannelInfo> inputs;
    inputs.push_back({0, true, true, "Mic Line", SampleFormat::Float32LE});

    std::vector<ChannelInfo> outputs;
    outputs.push_back({0, false, true, "Output 1", SampleFormat::Float32LE});
    outputs.push_back({1, false, true, "Output 2", SampleFormat::Float32LE});

    const ChannelPlan allow = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, true);
    ASSERT_TRUE(allow.valid());
    EXPECT_EQ(allow.micInput, 0);

    const ChannelPlan reject = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
    EXPECT_FALSE(reject.valid());
}

TEST(AsioChannelMapTest, IncompleteOutputPairIsRejected)
{
    std::vector<ChannelInfo> inputs;
    inputs.push_back({0, true, true, "Analog 1", SampleFormat::Float32LE});

    std::vector<ChannelInfo> outputs;
    outputs.push_back({0, false, true, "Analog 1", SampleFormat::Float32LE});

    const ChannelPlan plan = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
    EXPECT_FALSE(plan.valid());
}

TEST(DeviceStateMachineTest, FullLifecycleTransitionChainIsLegal)
{
    DeviceStateMachine state;
    EXPECT_EQ(state.current(), DeviceState::Uninitialized);

    ASSERT_TRUE(state.transition(DeviceState::Uninitialized, DeviceState::Idle));
    ASSERT_TRUE(state.transition(DeviceState::Idle, DeviceState::Opening));
    ASSERT_TRUE(state.transition(DeviceState::Opening, DeviceState::Ready));
    ASSERT_TRUE(state.transition(DeviceState::Ready, DeviceState::Starting));
    ASSERT_TRUE(state.transition(DeviceState::Starting, DeviceState::Streaming));
    ASSERT_TRUE(state.transition(DeviceState::Streaming, DeviceState::Stopping));
    ASSERT_TRUE(state.transition(DeviceState::Stopping, DeviceState::Ready));
    EXPECT_EQ(state.current(), DeviceState::Ready);
}

TEST(DeviceStateMachineTest, IllegalTransitionsAreRejected)
{
    DeviceStateMachine state;
    ASSERT_TRUE(state.transition(DeviceState::Uninitialized, DeviceState::Idle));

    EXPECT_FALSE(state.transition(DeviceState::Idle, DeviceState::Streaming));
    EXPECT_FALSE(state.transition(DeviceState::Ready, DeviceState::Streaming));
    EXPECT_FALSE(state.transition(DeviceState::Stopping, DeviceState::Opening));
    EXPECT_EQ(state.current(), DeviceState::Idle);
}

TEST(DeviceStateMachineTest, TransitionsAreIdempotentAndAtomic)
{
    DeviceStateMachine state;
    ASSERT_TRUE(state.transition(DeviceState::Uninitialized, DeviceState::Idle));
    ASSERT_TRUE(state.transition(DeviceState::Idle, DeviceState::Opening));

    ASSERT_TRUE(state.transition(DeviceState::Opening, DeviceState::Ready));
    EXPECT_FALSE(state.transition(DeviceState::Opening, DeviceState::Ready)) << "a second CAS must fail";
    EXPECT_FALSE(state.transition(DeviceState::Opening, DeviceState::Opening)) << "self transitions are illegal";
    EXPECT_EQ(state.current(), DeviceState::Ready);
}

TEST(DeviceStateMachineTest, DisconnectAndNoDevicePaths)
{
    DeviceStateMachine state;
    state.transition(DeviceState::Uninitialized, DeviceState::Idle);

    ASSERT_TRUE(state.transition(DeviceState::Idle, DeviceState::NoDevice));
    ASSERT_TRUE(state.transition(DeviceState::NoDevice, DeviceState::Idle));

    state.transition(DeviceState::Idle, DeviceState::Opening);
    state.transition(DeviceState::Opening, DeviceState::Ready);
    state.transition(DeviceState::Ready, DeviceState::Starting);
    state.transition(DeviceState::Starting, DeviceState::Streaming);

    ASSERT_TRUE(state.transition(DeviceState::Streaming, DeviceState::Reconnecting));
    ASSERT_TRUE(state.transition(DeviceState::Reconnecting, DeviceState::Opening));
    ASSERT_TRUE(state.transition(DeviceState::Opening, DeviceState::Ready));
}