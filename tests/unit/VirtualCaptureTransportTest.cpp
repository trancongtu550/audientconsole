#include "transport/TransportFormat.h"
#include "transport/VirtualCaptureTransport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace core = audient::transport;
namespace engine = audient::engine;

namespace
{

core::Format monoFormat(std::size_t block = 64)
{
    return core::Format{48000, 1, static_cast<int>(block), core::SampleType::Float32};
}

} // namespace

TEST(VirtualCaptureTransportTest, RejectsNonMonoOrInvalidFormat)
{
    EXPECT_THROW(
        core::VirtualCaptureTransport({48000, 2, 64, core::SampleType::Float32}, 256), std::invalid_argument)
        << "the virtual mic is mono; a stereo format must be rejected";
    EXPECT_THROW(core::VirtualCaptureTransport({0, 1, 64, core::SampleType::Float32}, 256), std::invalid_argument);
    EXPECT_THROW(core::VirtualCaptureTransport({48000, 1, 0, core::SampleType::Float32}, 256), std::invalid_argument);
}

TEST(VirtualCaptureTransportTest, RejectsInvalidCapacity)
{
    EXPECT_THROW(core::VirtualCaptureTransport(monoFormat(), 0), std::invalid_argument);
    EXPECT_THROW(core::VirtualCaptureTransport(monoFormat(), 100), std::invalid_argument)
        << "capacity must be a power of two (SPSC ring contract)";
}

TEST(VirtualCaptureTransportTest, ReportsFormatAndCapacity)
{
    core::VirtualCaptureTransport capture(monoFormat(64), 4096);
    EXPECT_EQ(capture.format().sampleRateHz, 48000);
    EXPECT_EQ(capture.format().channels, 1);
    EXPECT_EQ(capture.capacityFrames(), 4096u);
    EXPECT_EQ(capture.availableFrames(), 0u);
}

TEST(VirtualCaptureTransportTest, MonoBlockRoundTripPreservesSamples)
{
    core::VirtualCaptureTransport capture(monoFormat(64), 1024);

    std::vector<float> block(64);
    for (std::size_t i = 0; i < block.size(); ++i)
    {
        block[i] = static_cast<float>(i) / 64.0f;
    }
    ASSERT_TRUE(capture.writeProcessedMic(block.data(), 64));
    EXPECT_EQ(capture.availableFrames(), 64u);
    EXPECT_EQ(capture.producerSequence(), 1u);

    std::vector<float> read(64, -1.0f);
    ASSERT_TRUE(capture.readProcessedMic(read.data(), 64));
    for (std::size_t i = 0; i < read.size(); ++i)
    {
        EXPECT_EQ(read[i], block[i]);
    }
    EXPECT_EQ(capture.consumerSequence(), 1u);
    EXPECT_EQ(capture.availableFrames(), 0u);
}

TEST(VirtualCaptureTransportTest, OverflowDropsAreCountedAndProducerNeverBlocks)
{
    core::VirtualCaptureTransport capture(monoFormat(64), 128);
    std::vector<float> block(64, 0.5f);

    ASSERT_TRUE(capture.writeProcessedMic(block.data(), 64));
    ASSERT_TRUE(capture.writeProcessedMic(block.data(), 64));
    EXPECT_FALSE(capture.writeProcessedMic(block.data(), 64)) << "a full ring must reject, never block";
    EXPECT_FALSE(capture.writeProcessedMic(block.data(), 64));

    const core::VirtualCaptureTransport::CaptureStatistics stats = capture.stats();
    EXPECT_EQ(stats.produced, 128u) << "produced counts only committed producer samples";
    EXPECT_EQ(stats.overflowDrops, 128u) << "rejected blocks are overflowDrops, never silent";
    EXPECT_EQ(capture.availableFrames(), 128u);
    EXPECT_EQ(capture.producerSequence(), 2u) << "rejected blocks must not advance the accepted sequence";
}

TEST(VirtualCaptureTransportTest, UnderrunReadFailsAndIsCounted)
{
    core::VirtualCaptureTransport capture(monoFormat(64), 512);
    std::vector<float> read(64, -1.0f);

    EXPECT_FALSE(capture.readProcessedMic(read.data(), 64));
    EXPECT_GE(capture.stats().underruns, 64u);
    EXPECT_EQ(capture.consumerSequence(), 0u);
}

TEST(VirtualCaptureTransportTest, ResetYieldsFreshEpochWithoutStaleReplay)
{
    core::VirtualCaptureTransport capture(monoFormat(64), 512);

    std::vector<float> block(64, 0.75f);
    ASSERT_TRUE(capture.writeProcessedMic(block.data(), 64));
    ASSERT_TRUE(capture.readProcessedMic(block.data(), 64));
    ASSERT_TRUE(capture.writeProcessedMic(block.data(), 64));

    capture.reset();
    EXPECT_EQ(capture.availableFrames(), 0u) << "reset must clear buffered capture data";
    EXPECT_EQ(capture.producerSequence(), 0u);
    EXPECT_EQ(capture.consumerSequence(), 0u);
    EXPECT_EQ(capture.stats().overflowDrops, 0u);
    EXPECT_EQ(capture.stats().staleCatchupDrops, 0u);
    EXPECT_EQ(capture.stats().underruns, 0u);

    std::vector<float> read(64, -1.0f);
    EXPECT_FALSE(capture.readProcessedMic(read.data(), 64)) << "old data must never be served after reset";

    // A fresh producer block resumes capture.
    std::vector<float> fresh(64, 0.25f);
    ASSERT_TRUE(capture.writeProcessedMic(fresh.data(), 64));
    ASSERT_TRUE(capture.readProcessedMic(read.data(), 64));
    EXPECT_EQ(read[0], 0.25f);
}

TEST(VirtualCaptureTransportTest, FreshReadWithinThresholdServesNormalBacklog)
{
    core::VirtualCaptureTransport capture(monoFormat(64), 1024);

    std::vector<float> block(64, 0.5f);
    ASSERT_TRUE(capture.writeProcessedMic(block.data(), 64));
    ASSERT_TRUE(capture.writeProcessedMic(block.data(), 64));

    std::vector<float> read(64, -1.0f);
    ASSERT_TRUE(capture.readProcessedMicFresh(read.data(), 64, 256))
        << "a backlog within the freshness threshold is normal in-flight latency";
    for (const float sample : read)
    {
        EXPECT_EQ(sample, 0.5f);
    }
    EXPECT_EQ(capture.stats().staleCatchupDrops, 0u);
    EXPECT_EQ(capture.stats().underruns, 0u);
}

TEST(VirtualCaptureTransportTest, FreshReadDiscardsStalledBacklogAndServesOnlyPostRecoveryAudio)
{
    core::VirtualCaptureTransport capture(monoFormat(64), 1024);
    constexpr std::size_t kThreshold = 128;

    // Consumer stalls: producer keeps writing "stall-era" audio (must never be
    // served after recovery).
    std::vector<float> stale(64, 1.0f);
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(capture.writeProcessedMic(stale.data(), 64));
    }

    // First fresh read after recovery: the whole stale backlog (512 frames) is
    // discarded and counted; nothing is served.
    std::vector<float> read(64, -1.0f);
    EXPECT_FALSE(capture.readProcessedMicFresh(read.data(), 64, kThreshold)) << "no pre-recovery audio is served";
    EXPECT_EQ(capture.stats().staleCatchupDrops, 512u);
    EXPECT_GE(capture.stats().underruns, 64u);
    EXPECT_EQ(capture.availableFrames(), 0u);

    // Fresh audio produced after recovery is served normally (written and read
    // interleaved so the in-flight backlog never re-exceeds the threshold).
    std::vector<float> fresh(64, 0.25f);
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(capture.writeProcessedMic(fresh.data(), 64));
        ASSERT_TRUE(capture.readProcessedMicFresh(read.data(), 64, kThreshold));
        for (const float sample : read)
        {
            EXPECT_EQ(sample, 0.25f) << "only post-recovery audio may be served";
        }
    }
    EXPECT_EQ(capture.stats().staleCatchupDrops, 512u) << "catch-up discards are counted once";
}

TEST(VirtualCaptureTransportTest, OverflowStaleCatchupAndUnderrunCountersAreDistinct)
{
    // Stall long enough to overflow: producer rejects (overflowDrops), the
    // buffered backlog is stale, recovery discards it (staleCatchupDrops), and
    // the empty read underruns. All three counters independent, nothing silent.
    core::VirtualCaptureTransport capture(monoFormat(64), 256);
    constexpr std::size_t kThreshold = 64;

    std::vector<float> block(64, 0.9f);
    for (int i = 0; i < 10; ++i) // 640 frames attempted, ring holds 256
    {
        (void)capture.writeProcessedMic(block.data(), 64);
    }

    const core::VirtualCaptureTransport::CaptureStatistics afterStall = capture.stats();
    EXPECT_EQ(afterStall.produced, 256u) << "only capacity fits while the consumer is stalled";
    EXPECT_EQ(afterStall.overflowDrops, 384u) << "producer drops are counted, never silent";
    EXPECT_EQ(capture.availableFrames(), 256u);

    std::vector<float> read(64, -1.0f);
    EXPECT_FALSE(capture.readProcessedMicFresh(read.data(), 64, kThreshold)) << "stale backlog is not served";

    const core::VirtualCaptureTransport::CaptureStatistics after = capture.stats();
    EXPECT_EQ(after.overflowDrops, 384u);
    EXPECT_EQ(after.staleCatchupDrops, 256u) << "consumer resync discards the stale backlog";
    EXPECT_GE(after.underruns, 64u);
    EXPECT_EQ(after.produced, after.consumed) << "backlog was flushed to the write frontier";
    EXPECT_EQ(capture.availableFrames(), 0u);
}

TEST(VirtualCaptureTransportTest, ConcurrentProducerConsumerAccountsEveryAcceptedSample)
{
    constexpr std::uint64_t kTotal = 400000;
    constexpr std::size_t kBlock = 64;

    core::VirtualCaptureTransport capture(monoFormat(kBlock), 4096);
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
            if (capture.writeProcessedMic(block.data(), frames))
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
            if (capture.readProcessedMic(block.data(), kBlock))
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
            if (capture.stats().produced >= kTotal && capture.availableFrames() == 0)
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

    const auto stats = capture.stats();
    EXPECT_EQ(stats.produced, kTotal) << "every accepted sample must be produced (committed)";
    EXPECT_EQ(stats.consumed, kTotal) << "every accepted sample must be consumed";
    EXPECT_EQ(stats.produced, stats.consumed);
    EXPECT_EQ(capture.producerSequence() * kBlock, kTotal) << "the accepted-block sequence equals written samples";
    EXPECT_EQ(capture.consumerSequence() * kBlock, stats.consumed) << "the consumed-block sequence equals read samples";
    EXPECT_EQ(maxStep.load(), 1u);
    EXPECT_EQ(gapCount.load(), 0u);
}