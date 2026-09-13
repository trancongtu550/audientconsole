#include "engine/CallbackGuard.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

TEST(CallbackGuardTest, NormalCallbackReturnsTrue)
{
    bool ran = false;
    const bool result = audient::engine::invokeInCallback([&]() { ran = true; });
    EXPECT_TRUE(result);
    EXPECT_TRUE(ran);
}

TEST(CallbackGuardTest, ThrowingCallbackIsContainedAndCounted)
{
    const std::uint64_t before = audient::engine::callbackExceptionCount();

    const bool first = audient::engine::invokeInCallback([]() { throw std::runtime_error("boom"); });
    EXPECT_FALSE(first);
    EXPECT_EQ(audient::engine::callbackExceptionCount(), before + 1);

    const bool second = audient::engine::invokeInCallback([]() { throw "raw"; });
    EXPECT_FALSE(second);
    EXPECT_EQ(audient::engine::callbackExceptionCount(), before + 2);

    const bool after = audient::engine::invokeInCallback([]() {});
    EXPECT_TRUE(after);
}