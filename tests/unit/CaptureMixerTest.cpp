#include "core/AllocationTracker.h"
#include "engine/AudioRamp.h"
#include "routing/CaptureMixer.h"
#include "routing/RoutingTypes.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

#if defined(_MSC_VER) && !defined(NDEBUG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace
{
std::uintptr_t currentThreadId()
{
    return static_cast<std::uintptr_t>(::GetCurrentThreadId());
}
} // namespace
#else
namespace
{
std::uintptr_t currentThreadId()
{
    return 0;
}
} // namespace
#endif

using audient::routing::CaptureMixer;

namespace
{
constexpr std::size_t kFrames = 64;

void fill(std::array<float, kFrames>& b, float v)
{
    b.fill(v);
}
} // namespace

TEST(CaptureMixerTest, SingleSourceUnity_Mic1Slot0Only)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.25f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 1.0f;
    dest.fill(9.0f);
    CaptureMixer::mixMono(kFrames, srcs, gains, 1, dest.data());
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(dest[i], 0.25f, 1e-6f) << i;
    }
}

TEST(CaptureMixerTest, Fallback_Mic1EnabledMic2Disabled_Mic2Absent)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> slot1{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.3f);
    fill(slot1, 0.7f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = slot1.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 1.0f;
    gains[1] = 0.0f;
    CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(dest[i], 0.3f, 1e-6f);
    }
}

TEST(CaptureMixerTest, Mic2Slot1Only)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> slot1{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.1f);
    fill(slot1, -0.4f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = slot1.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 0.0f;
    gains[1] = 1.0f;
    CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(dest[i], -0.4f, 1e-6f);
    }
}

TEST(CaptureMixerTest, BothMicsLinearSumming_Transparent)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> slot1{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.2f);
    fill(slot1, 0.3f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = slot1.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 1.0f;
    gains[1] = 1.0f;
    CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(dest[i], 0.5f, 1e-6f);
    }
}

TEST(CaptureMixerTest, HeadroomPreserved_NoClampInsideMixer)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> slot1{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.8f);
    fill(slot1, 0.9f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = slot1.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 1.0f;
    gains[1] = 1.0f;
    CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    EXPECT_NEAR(dest[0], 1.7f, 1e-6f) << "mixer must NOT clamp; headroom preserved (downstream policy owns clamping)";
}

TEST(CaptureMixerTest, PerChannelGain_ScaleMic2Half)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> slot1{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.6f);
    fill(slot1, 0.4f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = slot1.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 1.0f;
    gains[1] = 0.5f;
    CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    EXPECT_NEAR(dest[0], 0.6f + 0.2f, 1e-6f);
}

TEST(CaptureMixerTest, MutedChannelContributesZero)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> slot1{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.9f);
    fill(slot1, 0.9f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = slot1.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 0.0f;
    gains[1] = 0.0f;
    CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(dest[i], 0.0f, 1e-6f);
    }
}

TEST(CaptureMixerTest, NullSourceIsSilence)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> dest{};
    fill(slot0, 0.5f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = nullptr;
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 1.0f;
    gains[1] = 1.0f;
    CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    EXPECT_NEAR(dest[0], 0.5f, 1e-6f);
}

TEST(CaptureMixerTest, RampedMix_UsesAudioRamp_NoPow)
{
    std::array<float, kFrames> slot0{};
    std::array<float, kFrames> slot1{};
    std::array<float, kFrames> dest{};
    fill(slot0, 1.0f);
    fill(slot1, 1.0f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = slot0.data();
    srcs[1] = slot1.data();
    std::array<audient::engine::AudioRamp, CaptureMixer::kCapacity> ramps;
    for (auto& r : ramps)
    {
        r.configure(kFrames);
        r.jumpTo(0.0f);
    }
    ramps[0].jumpTo(1.0f);
    ramps[1].jumpTo(1.0f);
    CaptureMixer::mixMonoRamped(kFrames, srcs, ramps.data(), 2, dest.data());
    EXPECT_NEAR(dest[0], 2.0f, 1e-6f);
    EXPECT_NEAR(dest[kFrames - 1], 2.0f, 1e-6f);
}

TEST(CaptureMixerTest, ZeroAllocation_MixMono)
{
    const bool supported = audient::core::isAllocationTrackerEnabled() ||
                           audient::core::installAllocationTracker() == audient::core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker only in Debug";
    }
    std::array<float, kFrames> s0{};
    std::array<float, kFrames> s1{};
    std::array<float, kFrames> dest{};
    fill(s0, 0.1f);
    fill(s1, -0.2f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = s0.data();
    srcs[1] = s1.data();
    float gains[CaptureMixer::kCapacity] = {};
    gains[0] = 1.0f;
    gains[1] = 1.0f;
    const std::uint64_t baseline = audient::core::allocationCountOnRealtimeThread();
    audient::core::markThreadRealtime(currentThreadId());
    for (int i = 0; i < 20000; ++i)
    {
        CaptureMixer::mixMono(kFrames, srcs, gains, 2, dest.data());
    }
    audient::core::clearRealtimeMark();
    EXPECT_EQ(audient::core::allocationCountOnRealtimeThread(), baseline);
}

TEST(CaptureMixerTest, ZeroAllocation_MixMonoRamped)
{
    const bool supported = audient::core::isAllocationTrackerEnabled() ||
                           audient::core::installAllocationTracker() == audient::core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker only in Debug";
    }
    std::array<float, kFrames> s0{};
    std::array<float, kFrames> s1{};
    std::array<float, kFrames> dest{};
    fill(s0, 0.1f);
    fill(s1, -0.2f);
    const float* srcs[CaptureMixer::kCapacity] = {};
    srcs[0] = s0.data();
    srcs[1] = s1.data();
    std::array<audient::engine::AudioRamp, CaptureMixer::kCapacity> ramps;
    for (auto& r : ramps)
    {
        r.configure(kFrames);
        r.jumpTo(0.0f);
    }
    ramps[0].jumpTo(1.0f);
    ramps[1].jumpTo(0.5f);
    const std::uint64_t baseline = audient::core::allocationCountOnRealtimeThread();
    audient::core::markThreadRealtime(currentThreadId());
    for (int i = 0; i < 20000; ++i)
    {
        for (auto& r : ramps)
        {
            r.jumpTo(ramps[0].settled() ? 1.0f : 0.5f);
        }
        CaptureMixer::mixMonoRamped(kFrames, srcs, ramps.data(), 2, dest.data());
    }
    audient::core::clearRealtimeMark();
    EXPECT_EQ(audient::core::allocationCountOnRealtimeThread(), baseline);
}
