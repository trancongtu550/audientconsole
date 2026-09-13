#include "transport/TransportFormat.h"
#include "transport/TransportLinks.h"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace core = audient::transport;
namespace engine = audient::engine;

TEST(TransportFormatTest, EqualityAndDescription)
{
    const core::Format stereo{48000, 2, 64, core::SampleType::Float32};
    const core::Format same{48000, 2, 64, core::SampleType::Float32};
    const core::Format mono{48000, 1, 64, core::SampleType::Float32};

    EXPECT_EQ(stereo, same);
    EXPECT_NE(stereo, mono);
    EXPECT_FALSE(stereo.describe().empty());
    EXPECT_TRUE(core::isCompatibleBlock(stereo, 64));
    EXPECT_FALSE(core::isCompatibleBlock(stereo, 128));
}

TEST(TransportFormatTest, InvalidFormatsAreRejected)
{
    core::Format badChannels{48000, 4, 64, core::SampleType::Float32};
    EXPECT_THROW(core::UplinkTransport(badChannels, 256), std::invalid_argument);

    core::Format zeroRate{0, 1, 64, core::SampleType::Float32};
    EXPECT_THROW(core::DownlinkTransport(zeroRate, 256), std::invalid_argument);
}

TEST(UplinkTransportTest, OverflowDropsAreCounted)
{
    core::UplinkTransport uplink({48000, 1, 64, core::SampleType::Float32}, 128);
    std::vector<float> block(64, 0.5f);

    EXPECT_TRUE(uplink.writeBlock(block.data(), 64));
    EXPECT_TRUE(uplink.writeBlock(block.data(), 64));
    EXPECT_FALSE(uplink.writeBlock(block.data(), 64));
    EXPECT_FALSE(uplink.writeBlock(block.data(), 64));

    const engine::LockFreeRingBuffer::Snapshot stats = uplink.stats();
    EXPECT_EQ(stats.dropped, 128u);
    EXPECT_EQ(stats.produced, 256u);
    EXPECT_EQ(uplink.availableFrames(), 128u);
}

TEST(DownlinkTransportTest, UnderrunFramesAreCounted)
{
    core::DownlinkTransport downlink({48000, 2, 64, core::SampleType::Float32}, 512);
    std::vector<float> read(128);

    EXPECT_FALSE(downlink.readBlockInterleaved(read.data(), 64));
    const engine::LockFreeRingBuffer::Snapshot stats = downlink.stats();
    EXPECT_EQ(stats.underrunFrames, 128u);
}

TEST(DownlinkTransportTest, StereoRoundTripPreservesOrder)
{
    core::DownlinkTransport downlink({48000, 2, 64, core::SampleType::Float32}, 512);

    std::vector<float> write(128);
    for (std::size_t i = 0; i < write.size(); ++i)
    {
        write[i] = static_cast<float>(i);
    }
    ASSERT_TRUE(downlink.writeBlock(write.data(), 64));
    ASSERT_TRUE(downlink.writeBlock(write.data(), 64));

    std::vector<float> read(128, -1.0f);
    ASSERT_TRUE(downlink.readBlockInterleaved(read.data(), 64));
    ASSERT_TRUE(downlink.readBlockInterleaved(read.data(), 64));

    for (std::size_t i = 0; i < read.size(); ++i)
    {
        EXPECT_EQ(read[i], write[i]);
    }
}

TEST(TransportTest, ConcurrentProducerConsumerAccountsEverySample)
{
    constexpr std::uint64_t kTotal = 400000;
    constexpr std::size_t kBlock = 64;

    core::UplinkTransport uplink({48000, 1, kBlock, core::SampleType::Float32}, 4096);
    std::atomic<bool> startFlag{false};
    std::atomic<std::uint64_t> maxStep{0};
    std::atomic<std::uint64_t> gapCount{0};

    std::thread producer([&]() {
        std::vector<float> block(kBlock);
        std::uint64_t index = 0;
        while (!startFlag.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (index < kTotal)
        {
            for (std::size_t i = 0; i < kBlock && index + i < kTotal; ++i)
            {
                block[i] = static_cast<float>(index + i);
            }
            const std::size_t frames = (index + kBlock <= kTotal) ? kBlock : static_cast<std::size_t>(kTotal - index);
            if (uplink.writeBlock(block.data(), frames))
            {
                index += frames;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    std::thread consumer([&]() {
        std::vector<float> block(kBlock);
        std::uint64_t lastValue = 0;
        bool haveLast = false;
        while (!startFlag.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (true)
        {
            if (uplink.readBlockInterleaved(block.data(), kBlock))
            {
                for (std::size_t i = 0; i < kBlock; ++i)
                {
                    const std::uint64_t value = static_cast<std::uint64_t>(block[i]);
                    if (haveLast)
                    {
                        const std::uint64_t step = value - lastValue;
                        if (step != 1)
                        {
                            gapCount.fetch_add(1, std::memory_order_relaxed);
                        }
                        maxStep.store(std::max(maxStep.load(std::memory_order_relaxed), step), std::memory_order_relaxed);
                    }
                    haveLast = true;
                    lastValue = value;
                }
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const engine::LockFreeRingBuffer::Snapshot state = uplink.stats();
            const std::uint64_t written = state.produced - state.dropped;
            if (written >= kTotal && uplink.availableFrames() == 0)
            {
                break;
            }
        }
        if (!haveLast || lastValue + 1 != kTotal)
        {
            gapCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    startFlag.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    const engine::LockFreeRingBuffer::Snapshot stats = uplink.stats();
    EXPECT_EQ(stats.consumed, kTotal) << "every accepted sample must be consumed";
    EXPECT_EQ(stats.produced, stats.consumed + stats.dropped) << "attempts must be accounted";
    EXPECT_EQ(maxStep.load(), 1u);
    EXPECT_EQ(gapCount.load(), 0u);
}