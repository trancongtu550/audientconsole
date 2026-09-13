#include "engine/RingBuffer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace core = audient::engine;

namespace
{
float seqValue(std::uint64_t index)
{
    return static_cast<float>(static_cast<std::int64_t>(index));
}
} // namespace

TEST(RingBufferTest, WrapAroundPreservesOrder)
{
    core::LockFreeRingBuffer buffer(1024);

    std::vector<float> written;
    std::vector<float> reference;
    std::vector<float> readBack(64);

    std::uint64_t produced = 0;
    for (int block = 0; block < 60; ++block)
    {
        std::vector<float> blockData(64);
        for (int i = 0; i < 64; ++i)
        {
            blockData[i] = seqValue(produced + static_cast<std::uint64_t>(i));
        }
        written.insert(written.end(), blockData.begin(), blockData.end());
        EXPECT_EQ(buffer.writeBlock(blockData.data(), 64), core::RingBufferStatus::Ok);
        while (buffer.available() >= 64)
        {
            EXPECT_EQ(buffer.readBlock(readBack.data(), 64), core::RingBufferStatus::Ok);
            reference.insert(reference.end(), readBack.begin(), readBack.end());
        }
        produced += 64;
    }

    while (buffer.available() >= 64)
    {
        EXPECT_EQ(buffer.readBlock(readBack.data(), 64), core::RingBufferStatus::Ok);
        reference.insert(reference.end(), readBack.begin(), readBack.end());
    }

    ASSERT_EQ(reference.size(), written.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        EXPECT_EQ(reference[i], written[i]);
    }

    const core::LockFreeRingBuffer::Snapshot stats = buffer.snapshot();
    EXPECT_EQ(stats.produced, produced);
    EXPECT_EQ(stats.consumed, written.size());
    EXPECT_EQ(stats.dropped, 0u);
}

TEST(RingBufferTest, OverflowDropsWholeBlockAndCounts)
{
    core::LockFreeRingBuffer buffer(256);
    std::vector<float> block(128, 1.0f);
    std::vector<float> fullRead(256, 0.0f);

    EXPECT_EQ(buffer.writeBlock(block.data(), 128), core::RingBufferStatus::Ok);
    EXPECT_EQ(buffer.writeBlock(block.data(), 128), core::RingBufferStatus::Ok);
    EXPECT_EQ(buffer.writeBlock(block.data(), 128), core::RingBufferStatus::Full);
    EXPECT_EQ(buffer.writeBlock(block.data(), 128), core::RingBufferStatus::Full);

    auto stats = buffer.snapshot();
    EXPECT_EQ(stats.dropped, 256u);
    EXPECT_EQ(stats.produced, 512u);
    EXPECT_EQ(buffer.available(), 256u);

    EXPECT_EQ(buffer.readBlock(fullRead.data(), 256), core::RingBufferStatus::Ok);
    const core::RingBufferStatus shortRead = buffer.readBlock(fullRead.data(), 64);
    EXPECT_EQ(shortRead, core::RingBufferStatus::Empty);

    stats = buffer.snapshot();
    EXPECT_EQ(stats.underrunFrames, 64u);
}

TEST(RingBufferTest, ResetClearsCounters)
{
    core::LockFreeRingBuffer buffer(64);
    std::vector<float> block(32, 1.0f);
    buffer.writeBlock(block.data(), 32);
    buffer.writeBlock(block.data(), 16);
    buffer.readBlock(block.data(), 8);
    buffer.reset();

    const core::LockFreeRingBuffer::Snapshot stats = buffer.snapshot();
    EXPECT_EQ(stats.produced, 0u);
    EXPECT_EQ(stats.consumed, 0u);
    EXPECT_EQ(stats.dropped, 0u);
    EXPECT_EQ(stats.underrunFrames, 0u);
    EXPECT_EQ(buffer.available(), 0u);
}

TEST(RingBufferTest, ConcurrentProducerConsumerAccountsEverySample)
{
    constexpr std::uint64_t kTotal = 600000;
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kCapacity = 4096;

    core::LockFreeRingBuffer buffer(kCapacity);
    std::atomic<bool> startFlag{false};
    std::atomic<std::uint64_t> gapCount{0};
    std::atomic<std::uint64_t> maxStep{0};

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
                block[i] = seqValue(index + i);
            }
            const std::size_t frames = (index + kBlock <= kTotal) ? kBlock : static_cast<std::size_t>(kTotal - index);
            if (buffer.writeBlock(block.data(), frames) == core::RingBufferStatus::Ok)
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
            const core::RingBufferStatus status = buffer.readBlock(block.data(), kBlock);
            if (status == core::RingBufferStatus::Ok)
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
            const core::LockFreeRingBuffer::Snapshot state = buffer.snapshot();
            const std::uint64_t written = state.produced - state.dropped;
            if (written >= kTotal && buffer.available() == 0)
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

    const core::LockFreeRingBuffer::Snapshot stats = buffer.snapshot();
    EXPECT_EQ(stats.consumed, kTotal) << "every accepted sample must eventually be consumed";
    EXPECT_EQ(stats.produced, stats.consumed + stats.dropped) << "attempted samples must be fully accounted";
    EXPECT_EQ(buffer.available(), 0u) << "buffer must be drained when the producer is finished";
    EXPECT_EQ(maxStep.load(), 1u) << "consumed samples must be strictly consecutive (no gaps or duplicates)";
    EXPECT_EQ(gapCount.load(), 0u) << "a non-unit step indicates tearing or a lost block in the consumed stream";
}

TEST(RingBufferTest, NonPowerOfTwoCapacityIsRejected)
{
    EXPECT_THROW(core::LockFreeRingBuffer(100), std::invalid_argument);
    EXPECT_THROW(core::LockFreeRingBuffer(0), std::invalid_argument);
    EXPECT_NO_THROW(core::LockFreeRingBuffer(4096));
}