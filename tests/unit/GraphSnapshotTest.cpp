#include "engine/GraphConfigSnapshot.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace
{

struct TestConfig
{
    std::uint64_t left = 0;
    std::uint64_t right = 0;
    std::uint64_t checksum = 0;
};

} // namespace

TEST(GraphSnapshotTest, ConcurrentPublishNeverProducesTornRead)
{
    audient::engine::SeqLockSnapshot<TestConfig> snapshot;

    TestConfig first;
    first.left = 1;
    first.right = 2;
    first.checksum = 3;
    snapshot.publish(first);

    std::atomic<std::uint64_t> tornReads{0};
    std::atomic<bool> sawFirst{false};
    std::atomic<bool> sawSecond{false};

    std::thread reader([&]() {
        bool hadFirst = false;
        bool hadSecond = false;

        while (!hadFirst || !hadSecond)
        {
            const TestConfig config = snapshot.read();
            if (config.left + config.right != config.checksum)
            {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            }
            if (!hadFirst && config.left == 1)
            {
                hadFirst = true;
                sawFirst.store(true, std::memory_order_release);
            }
            if (!hadSecond && config.left == 10)
            {
                hadSecond = true;
                sawSecond.store(true, std::memory_order_release);
            }
            if (config.left != 1 && config.left != 10)
            {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            }
        }

        for (int i = 0; i < 2'000'000; ++i)
        {
            const TestConfig config = snapshot.read();
            if (config.left + config.right != config.checksum)
            {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            }
            if (config.left != 1 && config.left != 10)
            {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    while (!sawFirst.load(std::memory_order_relaxed) || !sawSecond.load(std::memory_order_relaxed))
    {
        TestConfig next = first;
        if (sawFirst.load(std::memory_order_relaxed) && !sawSecond.load(std::memory_order_relaxed))
        {
            next = first;
            next.left = 10;
            next.right = 20;
            next.checksum = 30;
        }
        snapshot.publish(next);
        std::this_thread::yield();
    }

    TestConfig second;
    second.left = 10;
    second.right = 20;
    second.checksum = 30;
    for (int publish = 0; publish < 2000; ++publish)
    {
        snapshot.publish((publish % 2) == 0 ? first : second);
    }

    reader.join();

    EXPECT_EQ(tornReads.load(), 0u) << "seqlock must never expose a mixed (torn) configuration";
    EXPECT_TRUE(sawFirst.load());
    EXPECT_TRUE(sawSecond.load());
}

TEST(GraphSnapshotTest, NoConcurrentWriterReadsLatest)
{
    audient::engine::SeqLockSnapshot<TestConfig> snapshot;

    TestConfig first;
    first.left = 7;
    first.right = 8;
    first.checksum = 15;
    snapshot.publish(first);

    const TestConfig seen1 = snapshot.read();
    EXPECT_EQ(seen1.left, 7u);

    TestConfig second;
    second.left = 100;
    second.right = 200;
    second.checksum = 300;
    snapshot.publish(second);

    const TestConfig seen2 = snapshot.read();
    EXPECT_EQ(seen2.left, 100u);
    EXPECT_EQ(seen2.right, 200u);
    EXPECT_EQ(seen2.checksum, 300u);
}