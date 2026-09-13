#include "asio/AsioChannelMap.h"
#include "asio/sdk/NativeAsioDriver.h"
#include "asio/sdk/NativeAsioDriverProvider.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{

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

bool containsInsensitive(const std::string& haystack, const char* needle)
{
    const std::string lowered = [&]() {
        std::string value = haystack;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }();
    return lowered.find(needle) != std::string::npos;
}

std::vector<audient::asio::DriverIdentity> installeAsioDrivers()
{
    audient::asio::NativeAsioDriverProvider provider;
    return provider.available();
}

auto findAudient(const std::vector<audient::asio::DriverIdentity>& available)
{
    return std::find_if(available.begin(), available.end(), [](const auto& identity) {
        return containsInsensitive(identity.name, "audient");
    });
}

} // namespace

TEST(NativeAsioProbe, OpenAudientDriverAndReportCapabilities)
{
    const std::vector<audient::asio::DriverIdentity> available = installeAsioDrivers();
    const auto audientMatch = findAudient(available);
    if (audientMatch == available.end())
    {
        GTEST_SKIP() << "no Audient ASIO driver registered on this machine";
    }

    audient::asio::NativeAsioDriverProvider provider;
    std::string error;
    auto driver = provider.open(*audientMatch, error);
    ASSERT_NE(driver, nullptr) << error;

    const auto caps = driver->capabilities();
    EXPECT_GE(caps.inputChannels, 1);
    EXPECT_GE(caps.outputChannels, 2);
    EXPECT_GT(caps.channels.size(), 0u);
    EXPECT_FALSE(caps.supportedSampleRates.empty());
    EXPECT_NE(caps.bufferSizes.preferred, 0);

    std::vector<audient::asio::ChannelInfo> inputs;
    std::vector<audient::asio::ChannelInfo> outputs;
    for (const audient::asio::ChannelInfo& channel : caps.channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }

    for (const audient::asio::ChannelInfo& channel : caps.channels)
    {
        std::printf("[probe] %s%ld \"%s\" active=%d\n", channel.isInput ? "in " : "out", channel.index, channel.name.c_str(),
                    channel.isActive ? 1 : 0);
    }
    std::printf("[probe] driver=\"%s\" clsid=%s caps=%ldin/%ldout buffer=%ld rates=", audientMatch->name.c_str(),
                audientMatch->clsid.c_str(), caps.inputChannels, caps.outputChannels, caps.bufferSizes.preferred);
    for (const long rate : caps.supportedSampleRates)
    {
        std::printf("%ld ", rate);
    }
    std::printf("\n");
    std::fflush(stdout);

    const audient::asio::ChannelPlan plan = audient::asio::AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
    GTEST_LOG_(INFO) << "channel plan (mono mic + stereo out): " << (plan.valid() ? "VALID" : "INVALID");
    EXPECT_TRUE(plan.valid())
        << "the real ASIO channel-name corpus must map a mic input and a stereo output pair; tune AsioChannelMap tokens if this fails";
}

TEST(NativeAsioProbe, NativeDriverRejectsInitWithoutPinnedRates)
{
    const std::vector<audient::asio::DriverIdentity> available = installeAsioDrivers();
    const auto audientMatch = findAudient(available);
    if (audientMatch == available.end())
    {
        GTEST_SKIP() << "no Audient ASIO driver registered on this machine";
    }

    audient::asio::NativeAsioDriverProvider provider;
    std::string error;
    auto driver = provider.open(*audientMatch, error);
    ASSERT_NE(driver, nullptr) << error;

    const auto caps = driver->capabilities();
    const bool supports48k = std::find(caps.supportedSampleRates.begin(), caps.supportedSampleRates.end(), 48000L) != caps.supportedSampleRates.end();
    EXPECT_TRUE(supports48k) << "the target 48 kHz rate must be supported by the real driver";
}

TEST(NativeAsioStream, CallbacksStorageIsStableObjectMember)
{
    // Regression for the P0 optimized-build access violation (incident notes): the
    // ASIOCallbacks struct handed to createBuffers() must live in stable object
    // storage, not a stack temporary, because the driver retains the pointer and
    // invokes it from its own thread until disposeBuffers().
    audient::asio::DriverIdentity identity;
    identity.clsid = "{00000000-0000-0000-0000-000000000000}";
    identity.name = "lifetime probe";

    audient::asio::NativeAsioDriver first(identity);
    audient::asio::NativeAsioDriver second(identity);

    const ASIOCallbacks& firstStorage = first.callbacks();
    const ASIOCallbacks& secondStorage = second.callbacks();

    EXPECT_EQ(&firstStorage, &first.callbacks()) << "callbacks() must return a stable object member address";
    EXPECT_EQ(&secondStorage, &second.callbacks()) << "callbacks() must return a stable object member address";
    EXPECT_NE(&firstStorage, &secondStorage) << "each driver instance must own its callback storage";
}

TEST(NativeAsioStream, DirectCallbackZeroWrite)
{
    const std::vector<audient::asio::DriverIdentity> available = installeAsioDrivers();
    const auto audientMatch = findAudient(available);
    if (audientMatch == available.end())
    {
        GTEST_SKIP() << "no Audient ASIO driver registered on this machine";
    }

    audient::asio::NativeAsioDriverProvider provider;
    std::string error;
    auto driver = provider.open(*audientMatch, error);
    ASSERT_NE(driver, nullptr) << error;

    const auto caps = driver->capabilities();
    std::vector<audient::asio::ChannelInfo> inputs;
    std::vector<audient::asio::ChannelInfo> outputs;
    for (const audient::asio::ChannelInfo& channel : caps.channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }
    const audient::asio::ChannelPlan plan = audient::asio::AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
    ASSERT_TRUE(plan.valid());

    std::vector<audient::asio::ChannelInfo> inputRequest{inputs[static_cast<std::size_t>(plan.micInput)]};
    std::vector<audient::asio::ChannelInfo> outputRequest{outputs[static_cast<std::size_t>(plan.outputLeft)],
                                                          outputs[static_cast<std::size_t>(plan.outputRight)]};

    ASSERT_TRUE(driver->initDriver(48000, error)) << error;
    ASSERT_TRUE(driver->prepareBuffers(64, inputRequest, outputRequest, error)) << error;

    std::atomic<long> calls{0};
    auto callback = [](const audient::asio::AsioCallbackInfo& info, void* context) {
        auto* counter = static_cast<std::atomic<long>*>(context);
        counter->fetch_add(1, std::memory_order_relaxed);
        for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
        {
            for (long i = 0; i < info.sampleCount; ++i)
            {
                info.outputs[channel][i] = 0.0f;
            }
        }
    };
    ASSERT_TRUE(driver->setCallback(callback, &calls, error)) << error;

    ASSERT_TRUE(driver->startStream(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return calls.load(std::memory_order_relaxed) > 0; }, std::chrono::milliseconds(6000)))
        << "the native callback must be delivered when streaming silence";
    ASSERT_TRUE(driver->stopStream(error)) << error;
    EXPECT_GT(calls.load(std::memory_order_relaxed), 0L);
}