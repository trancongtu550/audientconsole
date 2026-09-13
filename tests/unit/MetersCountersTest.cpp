#include "engine/Counters.h"
#include "engine/Meters.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

TEST(MetersTest, KnownAmplitudeSeedIsTrackedAsPeak)
{
    audient::engine::PeakRmsMeter meter;
    std::vector<float> mono(48000, 0.1f);
    mono[0] = 0.5f;
    meter.feed(mono.data(), mono.size(), 1);

    EXPECT_NEAR(meter.peakAbs(0), 0.5f, 1e-6f);
    const float expectedDb = 20.0f * std::log10f(0.5f);
    EXPECT_NEAR(meter.peakDb(0), expectedDb, 0.01f);

    meter.feed(mono.data(), mono.size(), 1);
    EXPECT_NEAR(meter.peakAbs(0), 0.5f, 1e-6f) << "peak hold must keep the maximum";
}

TEST(MetersTest, SineRmsTracksTheory)
{
    audient::engine::PeakRmsMeter meter;
    constexpr std::size_t kFrames = 48000;
    constexpr double kAmplitude = 0.4;
    std::vector<float> mono(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        mono[i] = static_cast<float>(kAmplitude * std::sin(2.0 * 3.14159265358979323846 * i / kFrames * 10.0));
    }
    meter.feed(mono.data(), mono.size(), 1);

    const double expectedRms = kAmplitude / std::sqrt(2.0);
    EXPECT_NEAR(meter.rmsAbs(0), expectedRms, 1e-3);
}

TEST(MetersTest, ClippingLatchAndReset)
{
    audient::engine::PeakRmsMeter meter;
    std::vector<float> loud = {1.2f, -1.1f, 0.5f};
    meter.feed(loud.data(), loud.size(), 1);

    EXPECT_TRUE(meter.clipping(0));
    EXPECT_NEAR(meter.peakDb(0), 20.0f * std::log10f(1.2f), 0.05f);

    meter.resetPeakHold();
    EXPECT_FALSE(meter.clipping(0));
    EXPECT_EQ(meter.peakAbs(0), 0.0f);
}

TEST(CountersTest, EngineCountersSnapshotMatches)
{
    audient::engine::EngineCounters counters;
    counters.recordCallback();
    counters.recordCallback();
    counters.recordXrun();
    counters.recordOverload();
    counters.recordSequenceStep();
    counters.recordSequenceStep();
    counters.recordSequenceStep();
    counters.recordSanitized(12);

    const audient::engine::EngineCounters::Snapshot snapshot = counters.snapshot();
    EXPECT_EQ(snapshot.callbacks, 2u);
    EXPECT_EQ(snapshot.xruns, 1u);
    EXPECT_EQ(snapshot.overloads, 1u);
    EXPECT_EQ(snapshot.bufferSequence, 3u);
    EXPECT_EQ(snapshot.sanitizedSamples, 12u);
}

TEST(CountersTest, HistogramRecordsAndSummarizes)
{
    audient::engine::CallbackTimingHistogram histogram;
    for (int i = 0; i < 100; ++i)
    {
        histogram.record(0.05);
    }
    histogram.record(0.25);
    histogram.record(1.5);

    EXPECT_EQ(histogram.count(), 102u);
    EXPECT_EQ(histogram.lastMs(), 1.5);
    EXPECT_EQ(histogram.maxMs(), 1.5);
    EXPECT_GT(histogram.meanMs(), 0.0);
    EXPECT_NEAR(histogram.percentileMs(0.5), 0.05, 0.05);

    std::uint64_t counted = 0;
    for (std::size_t i = 0; i < audient::engine::CallbackTimingHistogram::kBucketCount; ++i)
    {
        counted += histogram.bucketCount(i);
    }
    EXPECT_EQ(counted, 102u);
}

TEST(MetersTest, ConcurrentReaderSeesFiniteValues)
{
    audient::engine::PeakRmsMeter meter;
    std::vector<float> block(128, 0.3f);
    std::atomic<bool> stop{false};

    std::thread writer([&]() {
        while (!stop.load(std::memory_order_acquire))
        {
            meter.feed(block.data(), 64, 2);
        }
    });

    for (int i = 0; i < 200000; ++i)
    {
        const float peak = meter.peakAbs(0);
        const float rms = meter.rmsAbs(1);
        ASSERT_TRUE(std::isfinite(peak));
        ASSERT_TRUE(std::isfinite(rms));
    }

    stop.store(true, std::memory_order_release);
    writer.join();
}