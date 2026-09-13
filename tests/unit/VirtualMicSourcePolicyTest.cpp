#include "channel/VirtualMicSource.h"

#include "engine/AudioRamp.h"
#include "routing/CaptureMixer.h"
#include "routing/RoutingTypes.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

using audient::channel::VirtualMicSource;
using audient::routing::CaptureMixer;

namespace
{
constexpr std::size_t kFrames = 64;

float maxAbs(const std::array<float, kFrames>& data)
{
    float peak = 0.0f;
    for (const float sample : data)
    {
        peak = std::max(peak, sample < 0.0f ? -sample : sample);
    }
    return peak;
}
} // namespace

TEST(VirtualMicSourcePolicyTest, DefaultEnablesInput1AndDisablesInput2)
{
    const auto sends = audient::channel::virtualMicSendsFor(VirtualMicSource::Input1, true);
    EXPECT_TRUE(sends.channel0.enabled);
    EXPECT_FALSE(sends.channel1.enabled);
}

TEST(VirtualMicSourcePolicyTest, Input2EnablesSlot1Only)
{
    const auto sends = audient::channel::virtualMicSendsFor(VirtualMicSource::Input2, true);
    EXPECT_FALSE(sends.channel0.enabled);
    EXPECT_TRUE(sends.channel1.enabled);
}

TEST(VirtualMicSourcePolicyTest, Input2FallsBackToInput1WhenOnlyOneInputExists)
{
    const auto sends = audient::channel::virtualMicSendsFor(VirtualMicSource::Input2, false);
    EXPECT_TRUE(sends.channel0.enabled);
    EXPECT_FALSE(sends.channel1.enabled);
}

TEST(VirtualMicSourcePolicyTest, ExactlyOneChannelIsEverEnabled)
{
    for (const bool dual : {false, true})
    {
        for (const VirtualMicSource source : {VirtualMicSource::Input1, VirtualMicSource::Input2})
        {
            const auto sends = audient::channel::virtualMicSendsFor(source, dual);
            const int enabled = (sends.channel0.enabled ? 1 : 0) + (sends.channel1.enabled ? 1 : 0);
            EXPECT_EQ(enabled, 1) << "dual=" << dual;
        }
    }
}

// The realtime seam the adapter uses: CaptureMixer::mixMonoRamped with the
// per-channel virtual-mic send ramps. A steady [1,0] must yield ONLY channel 0
// and a steady [0,1] must yield ONLY channel 1 — never a sum, and never the
// unselected channel.
TEST(VirtualMicSourcePolicyTest, RampedMixerSelectsExactlyOneChannelAndNeverSums)
{
    std::array<float, kFrames> channel0{};
    std::array<float, kFrames> channel1{};
    channel0.fill(0.3f);
    channel1.fill(0.7f);
    const float* sources[CaptureMixer::kCapacity] = {};
    sources[0] = channel0.data();
    sources[1] = channel1.data();

    // Input 1 selected.
    {
        std::array<audient::engine::AudioRamp, CaptureMixer::kCapacity> ramps;
        for (auto& ramp : ramps)
        {
            ramp.configure(kFrames);
            ramp.jumpTo(0.0f);
        }
        ramps[0].jumpTo(1.0f);
        std::array<float, kFrames> dest{};
        CaptureMixer::mixMonoRamped(kFrames, sources, ramps.data(), 2, dest.data());
        for (const float sample : dest)
        {
            EXPECT_NEAR(sample, 0.3f, 1e-6f) << "must be channel 0 only";
        }
        EXPECT_NEAR(maxAbs(dest), 0.3f, 1e-6f);
    }

    // Input 2 selected.
    {
        std::array<audient::engine::AudioRamp, CaptureMixer::kCapacity> ramps;
        for (auto& ramp : ramps)
        {
            ramp.configure(kFrames);
            ramp.jumpTo(0.0f);
        }
        ramps[1].jumpTo(1.0f);
        std::array<float, kFrames> dest{};
        CaptureMixer::mixMonoRamped(kFrames, sources, ramps.data(), 2, dest.data());
        for (const float sample : dest)
        {
            EXPECT_NEAR(sample, 0.7f, 1e-6f) << "must be channel 1 only";
        }
        EXPECT_NEAR(maxAbs(dest), 0.7f, 1e-6f);
    }
}
