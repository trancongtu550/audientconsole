#include "engine/ChannelControls.h"
#include "engine/SanitizeAudio.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

TEST(ChannelControlsTest, UnityGainIsTransparent)
{
    audient::engine::ChannelControls controls;
    controls.setRampSamples(0);

    std::vector<float> data = {0.5f, -0.25f, 0.0f, 1.0f, -1.0f};
    std::vector<float> reference = data;
    controls.processMono(data.data(), data.size());

    for (std::size_t i = 0; i < data.size(); ++i)
    {
        EXPECT_NEAR(data[i], reference[i], 1e-6f);
    }
}

TEST(ChannelControlsTest, TrimDbAppliesLinearGain)
{
    audient::engine::ChannelControls controls;
    controls.setRampSamples(0);
    controls.setTrimDb(-6.020599913279624f);
    std::vector<float> data(64, 1.0f);
    controls.processMono(data.data(), data.size());
    for (const float sample : data)
    {
        EXPECT_NEAR(sample, 0.5f, 1e-5f);
    }

    controls.setTrimDb(-12.041199826559248f);
    std::fill(data.begin(), data.end(), 1.0f);
    controls.processMono(data.data(), data.size());
    for (const float sample : data)
    {
        EXPECT_NEAR(sample, 0.25f, 1e-5f);
    }
}

TEST(ChannelControlsTest, PolarityInvertFlipsSign)
{
    audient::engine::ChannelControls controls;
    controls.setRampSamples(0);
    controls.setPolarityInvert(true);

    std::vector<float> data = {0.5f, -0.25f, 0.0f};
    controls.processMono(data.data(), data.size());
    EXPECT_NEAR(data[0], -0.5f, 1e-6f);
    EXPECT_NEAR(data[1], 0.25f, 1e-6f);
    EXPECT_NEAR(data[2], 0.0f, 1e-6f);
}

TEST(ChannelControlsTest, MuteRampsZeros)
{
    audient::engine::ChannelControls controls;
    controls.setRampSamples(0);
    controls.setMute(true);

    std::vector<float> data(64, 1.0f);
    controls.processMono(data.data(), data.size());
    for (const float sample : data)
    {
        EXPECT_NEAR(sample, 0.0f, 1e-6f);
    }
}

TEST(ChannelControlsTest, PanicMuteZeroesImmediately)
{
    audient::engine::ChannelControls controls;
    controls.setRampSamples(256);
    controls.setTrimDb(6.0f);
    controls.setMute(false);
    controls.processMono(std::vector<float>(256, 1.0f).data(), 256);

    std::vector<float> panic = {0.9f, -0.9f, 0.5f};
    controls.setPanicMute(true);
    controls.processMono(panic.data(), panic.size());
    for (const float sample : panic)
    {
        EXPECT_EQ(sample, 0.0f);
    }
}

TEST(ChannelControlsTest, GainChangeIsClickFree)
{
    audient::engine::ChannelControls controls;
    controls.setRampSamples(128);
    controls.setTrimDb(0.0f);
    controls.processMono(std::vector<float>(128, 1.0f).data(), 128);
    EXPECT_TRUE(controls.gainSettled());

    controls.setTrimDb(-12.041199826559248f);

    std::vector<float> data(256, 1.0f);
    controls.processMono(data.data(), data.size());

    float maxStep = 0.0f;
    for (std::size_t i = 1; i < data.size(); ++i)
    {
        maxStep = std::max(maxStep, std::fabs(data[i] - data[i - 1]));
    }

    EXPECT_LE(maxStep, 0.01f) << "a 0.75 linear-gain change across a 128-sample ramp must not click";
    EXPECT_NEAR(data.back(), 0.25f, 1e-4f);
}

TEST(SanitizeAudioTest, NanInfHandlingIsContained)
{
    audient::engine::SanitizeCounters counters;

    std::vector<float> data = {0.5f,
                               std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity(),
                               -0.75f};
    audient::engine::sanitizeBlock(data.data(), data.size(), counters);

    EXPECT_EQ(data[0], 0.5f);
    EXPECT_EQ(data[1], 0.0f);
    EXPECT_EQ(data[2], 1.0f);
    EXPECT_EQ(data[3], -1.0f);
    EXPECT_EQ(data[4], -0.75f);
    EXPECT_EQ(counters.nanReplaced, 1u);
    EXPECT_EQ(counters.infClamped, 2u);
}

TEST(SanitizeAudioTest, SanitizeValueMapsEdgeCases)
{
    EXPECT_EQ(audient::engine::sanitizeValue(std::numeric_limits<float>::quiet_NaN()), 0.0f);
    EXPECT_EQ(audient::engine::sanitizeValue(std::numeric_limits<float>::infinity()), 1.0f);
    EXPECT_EQ(audient::engine::sanitizeValue(-std::numeric_limits<float>::infinity()), -1.0f);
    EXPECT_EQ(audient::engine::sanitizeValue(-0.25f), -0.25f);
}