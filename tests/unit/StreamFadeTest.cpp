#include "asio/StreamFade.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace
{

constexpr float kMaxStep = 0.02f;

void expectNoClickAboveThreshold(const std::vector<float>& envelope)
{
    ASSERT_FALSE(envelope.empty());
    float previous = envelope.front();
    for (const float sample : envelope)
    {
        ASSERT_LE(std::fabs(sample - previous), kMaxStep) << "fade must step monotonically within the click threshold";
        previous = sample;
    }
}

} // namespace

TEST(StreamFadeTest, StartsMutedAndRampsInToFull)
{
    audient::asio::StreamFade fade;
    fade.configure(256);
    fade.requestFadeIn();

    std::vector<float> envelope;
    envelope.reserve(512);
    for (std::size_t i = 0; i < 512; ++i)
    {
        envelope.push_back(fade.next());
    }

    EXPECT_NEAR(envelope.front(), 0.0f, 1e-5f) << "fade-in must begin at silence";
    EXPECT_NEAR(envelope.back(), 1.0f, 1e-5f) << "fade-in must reach unity";
    expectNoClickAboveThreshold(envelope);
    EXPECT_FALSE(fade.isMuted());
    EXPECT_TRUE(fade.isFull());
}

TEST(StreamFadeTest, RampsOutToSilence)
{
    audient::asio::StreamFade fade;
    fade.configure(128);
    fade.requestFadeIn();
    for (std::size_t i = 0; i < 512; ++i)
    {
        fade.next();
    }
    ASSERT_TRUE(fade.isFull());

    fade.requestFadeOut();
    std::vector<float> envelope;
    envelope.reserve(256);
    for (std::size_t i = 0; i < 256; ++i)
    {
        envelope.push_back(fade.next());
    }

    EXPECT_NEAR(envelope.front(), 1.0f, 1e-5f) << "fade-out must begin at full level";
    EXPECT_NEAR(envelope.back(), 0.0f, 1e-5f) << "fade-out must reach silence";
    expectNoClickAboveThreshold(envelope);
    EXPECT_TRUE(fade.isMuted());
    EXPECT_FALSE(fade.isFull());
}

TEST(StreamFadeTest, MuteRequestRampsFromFullToZero)
{
    audient::asio::StreamFade fade;
    fade.configure(64);
    fade.requestFadeIn();
    for (std::size_t i = 0; i < 256; ++i)
    {
        fade.next();
    }

    fade.requestMute();
    std::vector<float> envelope;
    envelope.reserve(128);
    for (std::size_t i = 0; i < 128; ++i)
    {
        envelope.push_back(fade.next());
    }

    EXPECT_NEAR(envelope.front(), 1.0f, 1e-5f);
    EXPECT_NEAR(envelope.back(), 0.0f, 1e-5f);
    expectNoClickAboveThreshold(envelope);
    EXPECT_TRUE(fade.isMuted());
}

TEST(StreamFadeTest, ZeroLengthConfigureIsTrivialMute)
{
    audient::asio::StreamFade fade;
    fade.configure(0);
    fade.requestFadeIn();
    EXPECT_EQ(fade.next(), 0.0f) << "a zero-length fade must stay muted";
    EXPECT_TRUE(fade.isMuted());
}