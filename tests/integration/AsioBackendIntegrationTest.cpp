#include "SimulatedAsioDriver.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioDeviceState.h"
#include "engine/CallbackGuard.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace audient::asio;

namespace
{

void copyMonoInputToOutputs(const AsioCallbackInfo& info, void*)
{
    const float* input = info.inputChannels > 0 ? info.inputs[0] : nullptr;
    for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
    {
        float* output = info.outputs[channel];
        for (long i = 0; i < info.sampleCount; ++i)
        {
            output[i] = input != nullptr ? input[i] : 0.0f;
        }
    }
}

void writeHalfScale(const AsioCallbackInfo& info, void*)
{
    for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
    {
        float* output = info.outputs[channel];
        for (long i = 0; i < info.sampleCount; ++i)
        {
            output[i] = 0.5f;
        }
    }
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

ChannelPlan planFromCapabilities(const DriverCapabilities& caps)
{
    std::vector<ChannelInfo> inputs;
    std::vector<ChannelInfo> outputs;
    for (const ChannelInfo& channel : caps.channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }
    return AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
}

} // namespace

TEST(AsioLifecycle, FullLifecycleWithSimulatedDevice)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;
    EXPECT_EQ(backend.state(), DeviceState::Ready);

    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    backend.setStreamProcessor(copyMonoInputToOutputs, nullptr);
    ASSERT_TRUE(backend.start(error)) << error;

    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    EXPECT_GT(backend.callbackCount(), 0u);
    EXPECT_TRUE(std::isfinite(backend.lastCallbackMs()));

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    EXPECT_EQ(driver->lastSampleCount(), 64);

    const std::vector<float> out0 = driver->lastOutput(0);
    const std::vector<float> out1 = driver->lastOutput(1);
    ASSERT_FALSE(out0.empty());
    ASSERT_FALSE(out1.empty());
    EXPECT_NEAR(out0[0], 0.25f, 1e-6f) << "outputs must carry the mono mic input (0.25)";
    EXPECT_NEAR(out1[0], 0.25f, 1e-6f) << "both output channels receive the same mono mic input";

    ASSERT_TRUE(backend.stop(error));
    EXPECT_EQ(backend.state(), DeviceState::Ready);
}

TEST(AsioLifecycle, ConfigureAt128Samples)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 128, plan, error)) << error;
    backend.setStreamProcessor(writeHalfScale, nullptr);
    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    EXPECT_EQ(driver->lastSampleCount(), 128);

    const std::vector<float> out = driver->lastOutput(0);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.size(), 128u);
    for (const float sample : out)
    {
        EXPECT_EQ(sample, 0.5f);
    }

    ASSERT_TRUE(backend.stop(error));
}

TEST(AsioLifecycle, NoDeviceSafeMode)
{
    SimulatedAsioProvider provider;
    provider.setPresent(false);

    AsioBackend backend(provider);
    std::string error;
    EXPECT_FALSE(backend.selectAudientDevice(error));
    EXPECT_EQ(backend.state(), DeviceState::NoDevice);
    EXPECT_FALSE(error.empty());
}

TEST(AsioLifecycle, PanicStopsStreamAndClearsCleanly)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error));
    backend.setStreamProcessor(writeHalfScale, nullptr);
    ASSERT_TRUE(backend.start(error));
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));

    backend.panic();
    EXPECT_EQ(backend.state(), DeviceState::Ready);

    backend.clearPanic();
    ASSERT_TRUE(backend.start(error));
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(backend.stop(error));
}

TEST(AsioLifecycle, DisconnectEntersSafeModeAndReconnectRestores)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error));
    backend.setStreamProcessor(writeHalfScale, nullptr);
    ASSERT_TRUE(backend.start(error));
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    driver->simulateDisconnect();
    backend.handleDisconnect();

    EXPECT_EQ(backend.state(), DeviceState::Reconnecting);
    EXPECT_GE(backend.xrunCount(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const std::uint64_t settledCallbacks = backend.callbackCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(backend.callbackCount(), settledCallbacks) << "no callbacks may run while disconnected";

    driver->simulateReconnect();
    backend.handleReconnect();
    EXPECT_EQ(backend.state(), DeviceState::Ready);

    ASSERT_TRUE(backend.configure(48000, 64, plan, error));
    ASSERT_TRUE(backend.start(error));
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > settledCallbacks; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(backend.stop(error));
}

namespace
{

struct ThrowingCallbackContext
{
    std::atomic<std::uint32_t> throwFor{0};
    std::atomic<std::uint32_t> calls{0};
};

void throwForFirstN(const AsioCallbackInfo& info, void* rawContext)
{
    auto* context = static_cast<ThrowingCallbackContext*>(rawContext);
    const std::uint32_t call = context->calls.fetch_add(1, std::memory_order_relaxed);
    if (call < context->throwFor.load(std::memory_order_relaxed))
    {
        throw std::runtime_error("simulated callback overload");
    }
    for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
    {
        float* output = info.outputs[channel];
        for (long i = 0; i < info.sampleCount; ++i)
        {
            output[i] = 0.5f;
        }
    }
}

} // namespace

TEST(AsioLifecycle, CallbackOverloadIsCountedAndRecovers)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error));

    const std::uint64_t exceptionsBefore = audient::engine::callbackExceptionCount();
    ThrowingCallbackContext context;
    context.throwFor.store(3u, std::memory_order_relaxed);
    backend.setStreamProcessor(throwForFirstN, &context);

    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 8; }, std::chrono::milliseconds(3000)));

    EXPECT_GE(backend.xrunCount(), 1u) << "throwing callbacks must be counted as overload xruns";
    EXPECT_GE(backend.overloadCount(), 1u) << "throwing callbacks must be counted as overloads";
    EXPECT_GE(audient::engine::callbackExceptionCount(), exceptionsBefore + 1u)
        << "the callback exception boundary must count the escaped exception";

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> out = driver->lastOutput(0);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out[0], 0.5f) << "the stream must recover to normal output after the overload";

    ASSERT_TRUE(backend.stop(error));
}

TEST(AsioLifecycle, ResetStopsStreamAndPreservesReadyState)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error));
    backend.setStreamProcessor(writeHalfScale, nullptr);
    ASSERT_TRUE(backend.start(error));
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));

    ASSERT_TRUE(backend.requestReset(error)) << error;
    EXPECT_EQ(backend.state(), DeviceState::Ready);

    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    EXPECT_FALSE(driver->isStreaming());

    const std::uint64_t settled = backend.callbackCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(backend.callbackCount(), settled) << "no callbacks may run after reset";

    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > settled; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(backend.stop(error));
}

TEST(AsioLifecycle, SampleRateChangeIsRejectedWhileStreamingAndAppliesWhenStopped)
{
    SimulatedAsioProvider provider;
    AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error));
    backend.setStreamProcessor(writeHalfScale, nullptr);

    ASSERT_TRUE(backend.start(error));
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    EXPECT_FALSE(backend.changeSampleRate(88200, error)) << "sample-rate change must be rejected while streaming";
    ASSERT_TRUE(backend.stop(error));

    ASSERT_TRUE(backend.changeSampleRate(44100, error)) << error;
    EXPECT_EQ(backend.state(), DeviceState::Ready);

    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    ASSERT_TRUE(backend.stop(error));
    EXPECT_FALSE(driver->isStreaming());
}