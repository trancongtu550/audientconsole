#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/sdk/NativeAsioDriverProvider.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Real-hardware Phase 2 soak (incident notes §13/§14). Intentionally env-gated so the
// default ctest run stays quick: every suite reports SKIP unless the durable
// soak is requested. Run with:
//
//   AUDIENT_SOAK=1
//   AUDIENT_SOAK_CYCLES=100
//   AUDIENT_SOAK_BUFFER=64
//   AUDIENT_SOAK_SECONDS=1800
//   audient_console_soak_tests.exe --gtest_filter=Soak.*
//
// Only executes on a machine with the Audient ASIO driver registered.

namespace
{

std::string envOrDefault(const char* name, const char* fallback)
{
    std::string value;
    std::size_t length = 0;
    char* buffer = nullptr;
    if (_dupenv_s(&buffer, &length, name) == 0 && buffer != nullptr)
    {
        value.assign(buffer, length > 0 ? length - 1 : 0);
        std::free(buffer);
    }
    if (value.empty())
    {
        return fallback;
    }
    return value;
}

bool soakEnabled()
{
    return envOrDefault("AUDIENT_SOAK", "0") == "1";
}

long envLong(const char* name, long fallback)
{
    const std::string value = envOrDefault(name, "");
    if (value.empty())
    {
        return fallback;
    }
    return std::strtol(value.c_str(), nullptr, 10);
}

bool containsInsensitive(const std::string& haystack, const char* needle)
{
    std::string lowered = haystack;
    for (char& ch : lowered)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered.find(needle) != std::string::npos;
}

void writeZeros(const audient::asio::AsioCallbackInfo& info, void*)
{
    if (info.outputs == nullptr)
    {
        return;
    }
    for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
    {
        if (info.outputs[channel] != nullptr)
        {
            std::memset(info.outputs[channel], 0, static_cast<std::size_t>(info.sampleCount) * sizeof(float));
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

} // namespace

namespace audient::asio
{

TEST(Soak, DirectStartStopCycles)
{
    if (!soakEnabled())
    {
        GTEST_SKIP() << "soak disabled (set AUDIENT_SOAK=1 to run)";
    }

    const long cycles = envLong("AUDIENT_SOAK_CYCLES", 100);
    const long buffer = envLong("AUDIENT_SOAK_BUFFER", 64);
    ASSERT_GT(cycles, 0);
    ASSERT_GT(buffer, 0);

    NativeAsioDriverProvider provider;
    const std::vector<DriverIdentity> available = provider.available();
    DriverIdentity match;
    for (const DriverIdentity& candidate : available)
    {
        if (containsInsensitive(candidate.name, "audient"))
        {
            match = candidate;
            break;
        }
    }
    if (match.clsid.empty())
    {
        GTEST_SKIP() << "no Audient ASIO driver registered on this machine";
    }

    std::string error;
    for (long cycle = 0; cycle < cycles; ++cycle)
    {
        ASSERT_TRUE(provider.available().size() != 0) << "driver disappeared during cycle " << cycle;
        auto driver = provider.open(match, error);
        ASSERT_NE(driver, nullptr) << "cycle " << cycle << ": " << error;

        const DriverCapabilities caps = driver->capabilities();
        std::vector<ChannelInfo> inputs;
        std::vector<ChannelInfo> outputs;
        for (const ChannelInfo& channel : caps.channels)
        {
            (channel.isInput ? inputs : outputs).push_back(channel);
        }
        const ChannelPlan plan = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
        ASSERT_TRUE(plan.valid()) << "cycle " << cycle << ": channel plan invalid";

        ASSERT_TRUE(driver->initDriver(48000, error)) << "cycle " << cycle << ": " << error;
        ASSERT_TRUE(driver->prepareBuffers(buffer, {inputs[static_cast<std::size_t>(plan.micInput)]},
                                           {outputs[static_cast<std::size_t>(plan.outputLeft)],
                                            outputs[static_cast<std::size_t>(plan.outputRight)]},
                                           error))
            << "cycle " << cycle << ": " << error;

        std::atomic<long> calls{0};
        driver->setCallback([](const AsioCallbackInfo& info, void* context) {
            auto* counter = static_cast<std::atomic<long>*>(context);
            counter->fetch_add(1, std::memory_order_relaxed);
            if (info.outputs != nullptr)
            {
                for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
                {
                    if (info.outputs[channel] != nullptr)
                    {
                        std::memset(info.outputs[channel], 0,
                                    static_cast<std::size_t>(info.sampleCount) * sizeof(float));
                    }
                }
            }
        }, &calls, error);

        ASSERT_TRUE(driver->startStream(error)) << "cycle " << cycle << ": " << error;
        ASSERT_TRUE(waitUntil([&]() { return calls.load(std::memory_order_relaxed) > 0; }, std::chrono::milliseconds(6000)))
            << "cycle " << cycle << ": no callback delivered";
        ASSERT_TRUE(driver->stopStream(error)) << "cycle " << cycle << ": " << error;
        driver->disposeDriver();
    }
    std::printf("[soak] direct start/stop %ld cycles @ %ld samples: PASS\n", cycles, buffer);
}

TEST(Soak, BackendStartStopCycles)
{
    if (!soakEnabled())
    {
        GTEST_SKIP() << "soak disabled (set AUDIENT_SOAK=1 to run)";
    }

    const long cycles = envLong("AUDIENT_SOAK_CYCLES", 100);
    const long buffer = envLong("AUDIENT_SOAK_BUFFER", 64);
    ASSERT_GT(cycles, 0);
    ASSERT_GT(buffer, 0);

    for (long cycle = 0; cycle < cycles; ++cycle)
    {
        NativeAsioDriverProvider provider;
        AsioBackend backend(provider);
        std::string error;
        ASSERT_TRUE(backend.selectAudientDevice(error)) << "cycle " << cycle << ": " << error;

        std::vector<ChannelInfo> inputs;
        std::vector<ChannelInfo> outputs;
        for (const ChannelInfo& channel : backend.driverCapabilities().channels)
        {
            (channel.isInput ? inputs : outputs).push_back(channel);
        }
        const ChannelPlan plan = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
        ASSERT_TRUE(plan.valid()) << "cycle " << cycle << ": channel plan invalid";
        ASSERT_TRUE(backend.configure(48000, buffer, plan, error)) << "cycle " << cycle << ": " << error;

        backend.setStreamProcessor(writeZeros, nullptr);
        ASSERT_TRUE(backend.start(error)) << "cycle " << cycle << ": " << error;
        ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(6000)))
            << "cycle " << cycle << ": no callback delivered";
        ASSERT_TRUE(backend.stop(error)) << "cycle " << cycle << ": " << error;
    }
    std::printf("[soak] backend start/stop %ld cycles @ %ld samples: PASS\n", cycles, buffer);
}

TEST(Soak, StreamSilenceAtTargetBuffer)
{
    if (!soakEnabled())
    {
        GTEST_SKIP() << "soak disabled (set AUDIENT_SOAK=1 to run)";
    }

    const long buffer = envLong("AUDIENT_SOAK_BUFFER", 64);
    const long seconds = envLong("AUDIENT_SOAK_SECONDS", 1800);
    ASSERT_GT(buffer, 0);
    ASSERT_GT(seconds, 0);

    NativeAsioDriverProvider provider;
    AsioBackend backend(provider);
    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error)) << error;

    std::vector<ChannelInfo> inputs;
    std::vector<ChannelInfo> outputs;
    for (const ChannelInfo& channel : backend.driverCapabilities().channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }
    const ChannelPlan plan = AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, buffer, plan, error)) << error;

    backend.setStreamProcessor(writeZeros, nullptr);
    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(6000)))
        << "no callback delivered";

    const std::uint64_t before = backend.callbackCount();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const std::uint64_t after = backend.callbackCount();

    EXPECT_GT(after, before) << "callback count must keep advancing during the soak";
    EXPECT_EQ(backend.xrunCount(), 0u) << "zero xruns required during silence soak";
    EXPECT_EQ(backend.overloadCount(), 0u) << "zero overloads required during silence soak";
    std::printf("[soak] stream %lds @ %ld samples: callbacks before=%llu after=%llu xruns=%llu overloads=%llu\n",
                seconds, buffer, static_cast<unsigned long long>(before), static_cast<unsigned long long>(after),
                static_cast<unsigned long long>(backend.xrunCount()),
                static_cast<unsigned long long>(backend.overloadCount()));

    ASSERT_TRUE(backend.stop(error)) << error;
}

} // namespace audient::asio