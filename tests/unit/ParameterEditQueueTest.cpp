#include "vst3/ParameterEditQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// SPSC bounded parameter queue used by VST-009: UI/controller thread pushes,
// audio callback drains. The unit focus here is FIFO order, wrap-around,
// bounded overflow/drop accounting, and a concurrent producer/consumer stress
// proving no functional corruption of the exposed contract. (MSVC has no TSan;
// the stress asserts the observable FIFO/count invariants under real threads.)

namespace
{

constexpr Steinberg::Vst::ParamID kId1 = 0x1122;
constexpr Steinberg::Vst::ParamID kId2 = 0x3344;

audient::vst3::ParameterEditQueue::Edit editFor(std::uint64_t seq)
{
    audient::vst3::ParameterEditQueue::Edit edit;
    edit.id = static_cast<Steinberg::Vst::ParamID>(seq % 1000u);
    // The full sequence number rides in the value (exact for integers in
    // double), so the concurrent test can assert FIFO order across threads.
    edit.value = static_cast<Steinberg::Vst::ParamValue>(seq);
    return edit;
}

} // namespace

TEST(ParameterEditQueueTest, PushDrainPreservesFifoOrder)
{
    audient::vst3::ParameterEditQueue queue;
    ASSERT_TRUE(queue.push(kId1, 0.1));
    ASSERT_TRUE(queue.push(kId2, 0.2));
    ASSERT_TRUE(queue.push(kId1, 0.3));

    EXPECT_EQ(queue.pending(), 3u);

    std::vector<audient::vst3::ParameterEditQueue::Edit> drained(3);
    EXPECT_EQ(queue.drain(drained.data(), drained.size()), 3u);
    EXPECT_EQ(drained[0].id, kId1);
    EXPECT_EQ(drained[0].value, 0.1);
    EXPECT_EQ(drained[1].id, kId2);
    EXPECT_EQ(drained[2].id, kId1);
    EXPECT_EQ(drained[2].value, 0.3);
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(queue.pushed(), 3u);
    EXPECT_EQ(queue.dropped(), 0u);
}

TEST(ParameterEditQueueTest, DrainIsLimitedByRequestedMaxAndDeferred)
{
    audient::vst3::ParameterEditQueue queue;
    ASSERT_TRUE(queue.push(kId1, 0.1));
    ASSERT_TRUE(queue.push(kId1, 0.2));
    ASSERT_TRUE(queue.push(kId2, 0.3));

    std::vector<audient::vst3::ParameterEditQueue::Edit> drained(2);
    EXPECT_EQ(queue.drain(drained.data(), drained.size()), 2u);
    EXPECT_EQ(queue.pending(), 1u);

    EXPECT_EQ(queue.drain(drained.data(), drained.size()), 1u);
    EXPECT_EQ(drained[0].value, 0.3);
    EXPECT_EQ(queue.pending(), 0u);
}

TEST(ParameterEditQueueTest, OverflowDropsNewEditsAndCountsThem)
{
    audient::vst3::ParameterEditQueue queue(4);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(queue.push(kId1, static_cast<Steinberg::Vst::ParamValue>(i) / 100.0)) << "fill to capacity";
    }
    EXPECT_EQ(queue.pending(), 4u);

    EXPECT_FALSE(queue.push(kId1, 0.9)) << "queue full: new edit must be dropped";
    EXPECT_FALSE(queue.push(kId2, 0.9)) << "still full after first drop";
    EXPECT_EQ(queue.dropped(), 2u);
    EXPECT_EQ(queue.pushed(), 4u) << "pushed counts only accepted edits";
    EXPECT_EQ(queue.pending(), 4u) << "dropped edits never enter the buffer";
}

TEST(ParameterEditQueueTest, WrapAroundKeepsFifoAndMonotonicPositions)
{
    audient::vst3::ParameterEditQueue queue(4);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(queue.push(kId1, static_cast<Steinberg::Vst::ParamValue>(i) / 100.0));
    }

    std::vector<audient::vst3::ParameterEditQueue::Edit> drained(4);
    EXPECT_EQ(queue.drain(drained.data(), drained.size()), 4u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(drained[i].value, static_cast<Steinberg::Vst::ParamValue>(i) / 100.0);
    }

    // Re-fill after the head rode all the way around; a ghost read of the old
    // tail must not appear.
    for (std::size_t i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(queue.push(kId2, static_cast<Steinberg::Vst::ParamValue>(i) / 50.0));
    }
    EXPECT_EQ(queue.drain(drained.data(), drained.size()), 4u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(drained[i].id, kId2) << "wrap-around must not read stale edits";
        EXPECT_EQ(drained[i].value, static_cast<Steinberg::Vst::ParamValue>(i) / 50.0);
    }
}

TEST(ParameterEditQueueTest, ResetClearsPendingAndCounters)
{
    audient::vst3::ParameterEditQueue queue(4);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(queue.push(kId1, 0.5));
    }
    EXPECT_FALSE(queue.push(kId1, 0.5));
    std::vector<audient::vst3::ParameterEditQueue::Edit> drained(4);
    EXPECT_EQ(queue.drain(drained.data(), 2), 2u);

    queue.reset();
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(queue.pushed(), 0u);
    EXPECT_EQ(queue.dropped(), 0u);

    ASSERT_TRUE(queue.push(kId2, 0.25));
    EXPECT_EQ(queue.drain(drained.data(), 4), 1u);
    EXPECT_EQ(drained[0].id, kId2);
}

TEST(ParameterEditQueueTest, NullOrZeroDrainIsSafeNoOp)
{
    audient::vst3::ParameterEditQueue queue;
    ASSERT_TRUE(queue.push(kId1, 0.5));
    EXPECT_EQ(queue.drain(nullptr, 10), 0u);
    EXPECT_EQ(queue.pending(), 1u);

    std::vector<audient::vst3::ParameterEditQueue::Edit> drained(1);
    EXPECT_EQ(queue.drain(drained.data(), 0), 0u);
    EXPECT_EQ(queue.pending(), 1u);

    EXPECT_EQ(queue.drain(drained.data(), 1), 1u);
    EXPECT_EQ(queue.pending(), 0u);
}

TEST(ParameterEditQueueTest, ConcurrentProducerConsumerStressPreservesEveryEdit)
{
    constexpr std::uint64_t kTotal = 10'000;
    audient::vst3::ParameterEditQueue queue(256);

    std::atomic<bool> producerDone{false};
    std::thread producer([&]() {
        for (std::uint64_t i = 0; i < kTotal; ++i)
        {
            const audient::vst3::ParameterEditQueue::Edit edit = editFor(i);
            while (!queue.push(edit.id, edit.value))
            {
                // Back off only on the boundary policy of the ring; with the
                // consumer draining continuously this may transiently happen.
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::vector<audient::vst3::ParameterEditQueue::Edit> drained(32);
    std::uint64_t received = 0;
    std::int64_t lastSeq = -1; // first consumed edit (seq 0) must equal lastSeq+1
    bool orderPreserved = true;
    while (!producerDone.load(std::memory_order_acquire) || queue.pending() > 0)
    {
        const std::size_t n = queue.drain(drained.data(), drained.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::uint64_t seq = static_cast<std::uint64_t>(drained[i].value);
            // Edits carry the full sequence number in `value`, so FIFO order is
            // asserted exactly without float reconstruction.
            if (static_cast<std::int64_t>(seq) != lastSeq + 1)
            {
                orderPreserved = false;
            }
            lastSeq = static_cast<std::int64_t>(seq);
            ++received;
        }
    }
    producer.join();

    // As long as a producer retries after a transient full, the queue may have
    // counted push-attempt failures (dropped > 0) even though every edit was
    // eventually accepted. The deterministic drop contract is covered by the
    // bounded-buffer test; here we prove FIFO order and exactly-once delivery.
    EXPECT_TRUE(orderPreserved) << "FIFO order must hold across threads";
    EXPECT_EQ(received, kTotal) << "every accepted edit must be consumed exactly once";
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(queue.pushed(), kTotal);
}

TEST(ParameterEditQueueTest, DefaultCapacityIs256)
{
    audient::vst3::ParameterEditQueue queue;
    EXPECT_EQ(queue.capacity(), audient::vst3::ParameterEditQueue::kDefaultCapacity);
    EXPECT_EQ(audient::vst3::ParameterEditQueue::kDefaultCapacity, 256u);
}