#include "asio/AsioRecoveryController.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace
{

// Fake recoverable stream: the test controls the callback count, whether a
// fresh open succeeds, and counts open/close calls.
class FakeStream final : public audient::asio::IRecoverableAsioStream
{
public:
    std::uint64_t count = 100;
    bool canOpen = true;
    int opens = 0;
    int closes = 0;

    std::uint64_t callbackCount() override { return count; }

    bool openAndStart(std::string& /*error*/) override
    {
        ++opens;
        return canOpen;
    }

    void closeStream() override { ++closes; }
};

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

class AsioRecoveryControllerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        clockBase = Clock::now();
        fake = std::make_unique<FakeStream>();
        controller = std::make_unique<audient::asio::AsioRecoveryController>(*fake);
    }

    Clock::time_point at(Ms offset) const { return clockBase + offset; }

    Clock::time_point clockBase{};
    std::unique_ptr<FakeStream> fake;
    std::unique_ptr<audient::asio::AsioRecoveryController> controller;
};

// A stall of >= stallMs (150 ms) on the control thread declares device loss.
TEST_F(AsioRecoveryControllerTest, StallDetectionTriggersLoss)
{
    controller->markStreaming(at(Ms(0)));
    controller->tick(at(Ms(0)));  // establishes the baseline count
    controller->tick(at(Ms(20))); // starts the idle timer here
    controller->tick(at(Ms(160))); // idle 140 ms < 150 ms: still streaming
    EXPECT_EQ(audient::asio::PhysDeviceState::Streaming, controller->state());
    EXPECT_EQ(0u, fake->closes);

    controller->tick(at(Ms(180))); // idle 160 ms >= 150 ms: loss
    EXPECT_EQ(audient::asio::PhysDeviceState::Lost, controller->state());
    EXPECT_EQ(1u, controller->lostCount());
    EXPECT_EQ(1, fake->closes); // dead stream closed on the control thread
    EXPECT_TRUE(controller->consumeEvents().lost);
}

// Advancing callbacks must never trip the detector.
TEST_F(AsioRecoveryControllerTest, AdvancingCallbacksNeverTrip)
{
    controller->markStreaming(at(Ms(0)));
    for (int i = 0; i < 100; ++i)
    {
        fake->count = 100u + static_cast<std::uint64_t>(i);
        controller->tick(at(Ms(20) * i));
    }
    EXPECT_EQ(audient::asio::PhysDeviceState::Streaming, controller->state());
    EXPECT_EQ(0u, controller->lostCount());
    EXPECT_EQ(0, fake->closes);
}

// After loss, a successful fresh open within retryMs resumes streaming.
TEST_F(AsioRecoveryControllerTest, RecoveryReopensFreshStreamAndResumes)
{
    controller->markStreaming(at(Ms(0)));
    controller->tick(at(Ms(0)));
    controller->simulateLoss(at(Ms(50)));
    EXPECT_EQ(audient::asio::PhysDeviceState::Lost, controller->state());
    EXPECT_EQ(1, fake->closes);

    controller->tick(at(Ms(300)));  // 250 ms < 750 ms: no attempt yet
    EXPECT_EQ(0, fake->opens);
    controller->tick(at(Ms(900)));  // 850 ms >= 750 ms: reopen -> fresh stream
    EXPECT_EQ(1, fake->opens);
    EXPECT_EQ(audient::asio::PhysDeviceState::Streaming, controller->state());
    EXPECT_EQ(1u, controller->recoveryCount());
    EXPECT_TRUE(controller->consumeEvents().recovered);
}

// Failed opens retry at retryMs cadence without busy-spinning.
TEST_F(AsioRecoveryControllerTest, FailedOpensRetryOnlyAtCadence)
{
    fake->canOpen = false;
    controller->markStreaming(at(Ms(0)));
    controller->tick(at(Ms(0)));
    controller->simulateLoss(at(Ms(0)));

    controller->tick(at(Ms(750))); // first attempt fails
    EXPECT_EQ(1, fake->opens);
    EXPECT_EQ(1u, controller->failedAttempts());
    EXPECT_EQ(audient::asio::PhysDeviceState::Lost, controller->state());

    controller->tick(at(Ms(800))); // only 50 ms later: no busy-spin
    EXPECT_EQ(1, fake->opens);

    controller->tick(at(Ms(1500))); // 750 ms after the failed attempt: retry
    EXPECT_EQ(2, fake->opens);
    EXPECT_EQ(2u, controller->failedAttempts());
    EXPECT_EQ(audient::asio::PhysDeviceState::Lost, controller->state());
}

// simulateLoss is the diagnostic path: immediate transition, no callback stall.
TEST_F(AsioRecoveryControllerTest, SimulatedLossImmediate)
{
    controller->markStreaming(at(Ms(0)));
    controller->simulateLoss(at(Ms(0)));
    EXPECT_EQ(audient::asio::PhysDeviceState::Lost, controller->state());
    EXPECT_EQ(1u, controller->lostCount());
    EXPECT_EQ(1, fake->closes);
}

// requestStop() halts all detection and recovery activity.
TEST_F(AsioRecoveryControllerTest, StopHaltsActivity)
{
    controller->markStreaming(at(Ms(0)));
    controller->simulateLoss(at(Ms(0)));
    controller->requestStop();
    EXPECT_TRUE(controller->stopRequested());

    controller->tick(at(Ms(900))); // >= retryMs but stop requested
    EXPECT_EQ(0, fake->opens);
    EXPECT_EQ(audient::asio::PhysDeviceState::Lost, controller->state());
}

} // namespace