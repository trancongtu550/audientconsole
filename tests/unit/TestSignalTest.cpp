#include "engine/SyntheticDownlink.h"
#include "engine/TestSignal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

TEST(TestSignalTest, DiagnosticBuildSynthesizesRequestedLevel)
{
    audient::engine::TestSignalSource signal;
    signal.configure(1000.0, -18.0, 48000, 0.0);

#if defined(NDEBUG)
    GTEST_SKIP() << "test signal is disabled in Release builds by design";
#else
    ASSERT_TRUE(signal.enabled());
#endif

    std::vector<float> mono(48000);
    signal.fillMono(mono.data(), mono.size());

    const float expectedAmplitude = std::pow(10.0f, -18.0f / 20.0f);
    EXPECT_NEAR(mono[0], expectedAmplitude, 1e-4f);
    const float peak = *std::max_element(mono.begin(), mono.end());
    EXPECT_NEAR(peak, expectedAmplitude, 1e-4f);

    const float peakDb = 20.0f * std::log10f(peak + 1e-12f);
    EXPECT_NEAR(peakDb, -18.0f, 0.05f);
}

TEST(TestSignalTest, DiagnosticBuildPhaseAdvances)
{
#if defined(NDEBUG)
    GTEST_SKIP() << "test signal is disabled in Release builds by design";
#else
    audient::engine::TestSignalSource signal;
    signal.configure(1000.0, -18.0, 48000, 0.0);

    std::vector<float> frame(48000);
    signal.fillMono(frame.data(), frame.size());
    EXPECT_NEAR(signal.currentPhaseRadians(), 2.0 * 3.14159265358979323846 * 1000.0, 1e-3);
#endif
}

TEST(TestSignalTest, StereoFillCarriesBothChannels)
{
#if defined(NDEBUG)
    GTEST_SKIP() << "test signal is disabled in Release builds by design";
#else
    audient::engine::TestSignalSource signal;
    signal.configure(440.0, -20.0, 48000, 0.0);

    std::vector<float> left(4800);
    std::vector<float> right(4800);
    signal.fillStereo(left.data(), right.data(), left.size());

    EXPECT_NEAR(left[0], right[0], 1e-6f);
    EXPECT_NE(left[0], 0.0f);
    EXPECT_NEAR(left[0], std::pow(10.0f, -20.0f / 20.0f), 1e-4f);
#endif
}

TEST(TestSignalTest, ReleaseBuildEmitsSilence)
{
#if !defined(NDEBUG)
    GTEST_SKIP() << "Release-only behavior; this build has the signal path enabled";
#else
    audient::engine::TestSignalSource signal;
    signal.configure(1000.0, -18.0, 48000, 0.0);

    EXPECT_FALSE(signal.enabled());
    std::vector<float> mono(48000, 1.0f);
    signal.fillMono(mono.data(), mono.size());
    for (const float sample : mono)
    {
        EXPECT_EQ(sample, 0.0f);
    }
#endif
}

TEST(SyntheticDownlinkTest, DistinctLeftRightAtRequestedLevel)
{
#if defined(NDEBUG)
    GTEST_SKIP() << "synthetic downlink is disabled in Release builds by design";
#else
    audient::engine::SyntheticDownlink downlink;
    downlink.configure(440.0, 880.0, -20.0, 48000);

    ASSERT_TRUE(downlink.enabled());
    std::vector<float> left(48000);
    std::vector<float> right(48000);
    downlink.fill(left.data(), right.data(), left.size());

    const float expectedPeak = std::pow(10.0f, -20.0f / 20.0f);
    EXPECT_NEAR(*std::max_element(left.begin(), left.end()), expectedPeak, 1e-4f);
    EXPECT_NEAR(*std::max_element(right.begin(), right.end()), expectedPeak, 1e-4f);

    EXPECT_GT(std::fabs(left[0] - right[0]), 0.05f) << "left/right downlink channels must be distinct";
#endif
}

TEST(SyntheticDownlinkTest, ReleaseBuildEmitsSilence)
{
#if !defined(NDEBUG)
    GTEST_SKIP() << "Release-only behavior; this build has the synthetic source enabled";
#else
    audient::engine::SyntheticDownlink downlink;
    downlink.configure(440.0, 880.0, -20.0, 48000);

    EXPECT_FALSE(downlink.enabled());
    std::vector<float> left(48000, 1.0f);
    std::vector<float> right(48000, 1.0f);
    downlink.fill(left.data(), right.data(), left.size());
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        EXPECT_EQ(left[i], 0.0f);
        EXPECT_EQ(right[i], 0.0f);
    }
#endif
}