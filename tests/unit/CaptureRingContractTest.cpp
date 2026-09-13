#include "virtual_audio/driver-protocol/CaptureRingContract.h"
#include "virtual_audio/driver-protocol/SharedRingCaptureSink.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace cr = audient::capture_ring;
namespace va = audient::virtual_audio;

namespace
{

constexpr cr::U32 kBlock = 64u;
constexpr cr::U32 kCapacity = 512u;

// A freshness threshold larger than any test ring capacity: the Q1 policy only
// discards a backlog that EXCEEDS the threshold, so this disables the stale
// resync for tests that exercise plain full-block reads.
constexpr cr::U32 kNoStaleFrames = 0x7FFFFFFFu;

std::vector<std::uint8_t> makeRegion(cr::U32 capacityFrames = kCapacity)
{
    const std::uint64_t bytes = cr::CaptureRingRegionBytes(capacityFrames);
    return std::vector<std::uint8_t>(static_cast<std::size_t>(bytes), 0u);
}

cr::CaptureRingHeader* headerOf(std::vector<std::uint8_t>& region)
{
    return reinterpret_cast<cr::CaptureRingHeader*>(region.data());
}

void initRegion(std::vector<std::uint8_t>& region, cr::U32 capacityFrames = kCapacity)
{
    ASSERT_TRUE(cr::CaptureRingInit(headerOf(region), static_cast<cr::U64>(region.size()),
                                    capacityFrames, kBlock));
}

float peakAbs(const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (const float s : samples)
    {
        peak = std::max(peak, std::fabs(s));
    }
    return peak;
}

} // namespace

// --- Layout / validation ----------------------------------------------------

TEST(CaptureRingContractTest, LayoutSizesAreVersionedAndStable)
{
    EXPECT_EQ(cr::REGION_VERSION, 1u);
    EXPECT_EQ(cr::REGION_FORMAT_MONO_FLOAT32, 1u);
    // RegionBytes includes the header + capacity float samples.
    EXPECT_EQ(cr::CaptureRingRegionBytes(512), sizeof(cr::CaptureRingHeader) + 512u * 4u);
}

TEST(CaptureRingContractTest, InitThenValidateRoundTrip)
{
    auto region = makeRegion();
    initRegion(region);
    EXPECT_TRUE(cr::CaptureRingValidate(headerOf(region), region.size(),
                                        cr::REGION_FORMAT_MONO_FLOAT32,
                                        cr::REGION_SAMPLE_RATE));
    // Snapshot counters start at zero.
    cr::CaptureRingHeader* h = headerOf(region);
    EXPECT_EQ(h->producedSamples, 0u);
    EXPECT_EQ(h->overflowDrops, 0u);
    EXPECT_EQ(h->underruns, 0u);
    EXPECT_EQ(h->generationFlushes, 0u);
}

TEST(CaptureRingContractTest, ValidateRejectsBadMagicVersionSizeAndFormat)
{
    auto region = makeRegion();
    initRegion(region);
    cr::CaptureRingHeader* h = headerOf(region);

    h->magic = 0xDEADBEEFu;
    EXPECT_FALSE(cr::CaptureRingValidate(h, region.size(), 0u, cr::REGION_SAMPLE_RATE));
    h->magic = cr::REGION_MAGIC;

    h->version = 99u;
    EXPECT_FALSE(cr::CaptureRingValidate(h, region.size(), 0u, cr::REGION_SAMPLE_RATE));
    h->version = cr::REGION_VERSION;

    // Region too small for the declared capacity.
    EXPECT_FALSE(cr::CaptureRingValidate(h, cr::CaptureRingRegionBytes(kCapacity) - 4u, 0u,
                                         cr::REGION_SAMPLE_RATE));

    // Format tag mismatch (when a specific tag is required).
    h->formatTag = 99u;
    EXPECT_FALSE(cr::CaptureRingValidate(h, region.size(), cr::REGION_FORMAT_MONO_FLOAT32,
                                         cr::REGION_SAMPLE_RATE));
}

TEST(CaptureRingContractTest, ValidateRejectsNonPowerOfTwoCapacity)
{
    auto region = makeRegion(512);
    EXPECT_TRUE(cr::CaptureRingCapacityIsLegal(512, kBlock));
    EXPECT_FALSE(cr::CaptureRingCapacityIsLegal(300, kBlock));
    EXPECT_FALSE(cr::CaptureRingCapacityIsLegal(0, kBlock));
    EXPECT_FALSE(cr::CaptureRingCapacityIsLegal(512, 1024)); // capacity < block
}

// --- Ring producer/consumer core ---------------------------------------------

TEST(CaptureRingContractTest, ProducerRejectsWhenDisconnectedAndAcceptsWhenConnected)
{
    auto region = makeRegion();
    initRegion(region);
    cr::CaptureRingHeader* h = headerOf(region);

    std::vector<float> block(kBlock, 0.5f);
    // Flag clear => disconnect.
    EXPECT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, 0u),
              cr::WRITE_DISCONNECTED);
    h->flags |= cr::REGION_FLAG_CONNECTED;
    EXPECT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, 0u), cr::WRITE_OK);
    EXPECT_EQ(h->producedSamples, kBlock);
}

TEST(CaptureRingContractTest, DropNewRejectsWholeBlockAndCountsOverflow)
{
    auto region = makeRegion(256); // 4 blocks
    initRegion(region, 256);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    std::vector<float> block(kBlock, 0.25f);
    for (cr::U32 i = 0; i < 4; ++i)
    {
        EXPECT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, i), cr::WRITE_OK);
    }
    // 5th block must be REJECTED as a whole (never partially written).
    EXPECT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, 4u), cr::WRITE_STALLED);
    EXPECT_EQ(h->overflowDrops, kBlock); // exactly one block dropped
    EXPECT_EQ(h->producedSamples, 256u);
    EXPECT_EQ(h->producerSequence, 3u); // last ACCEPTED sequence
}

TEST(CaptureRingContractTest, FullBlockOrNothingConsumerRead)
{
    auto region = makeRegion();
    initRegion(region);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    std::vector<float> block(kBlock, 0.5f);
    ASSERT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, 0u), cr::WRITE_OK);

    // Reading more than buffered must fail WITHOUT advancing the read head.
    std::vector<float> out(128, -1.0f);
    EXPECT_EQ(cr::CaptureRingConsumerRead(h, out.data(), 128u, kNoStaleFrames), 0);
    EXPECT_EQ(h->consumedSamples, 0u);
    EXPECT_GT(h->underruns, 0u);
    EXPECT_EQ(h->readPos, 0u);

    // Reading exactly one block succeeds and advances exactly one block.
    out.assign(64, -1.0f);
    EXPECT_EQ(cr::CaptureRingConsumerRead(h, out.data(), kBlock, kNoStaleFrames), 1);
    EXPECT_EQ(h->consumedSamples, kBlock);
    EXPECT_NEAR(out[0], 0.5f, 1e-6f);
}

TEST(CaptureRingContractTest, WrapAroundAfterCapacity)
{
    auto region = makeRegion(256);
    initRegion(region, 256);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    std::vector<float> block(kBlock, 0.5f);
    std::vector<float> out(kBlock, -1.0f);

    // Fill + drain several full laps so writePos/readPos wrap the capacity.
    for (cr::U32 lap = 0; lap < 8; ++lap)
    {
        for (cr::U32 b = 0; b < 4; ++b)
        {
            ASSERT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, b), cr::WRITE_OK);
        }
        for (cr::U32 b = 0; b < 4; ++b)
        {
            ASSERT_EQ(cr::CaptureRingConsumerRead(h, out.data(), kBlock, kNoStaleFrames), 1);
            EXPECT_NEAR(out[0], 0.5f, 1e-6f);
        }
    }
    EXPECT_EQ(h->producedSamples, 8u * 256u);
    EXPECT_EQ(h->consumedSamples, 8u * 256u);
    EXPECT_EQ(h->overflowDrops, 0u);
    EXPECT_EQ(h->underruns, 0u);
}

TEST(CaptureRingContractTest, GenerationFlushDropsOldEpochBacklog)
{
    auto region = makeRegion(256);
    initRegion(region, 256);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    // Old epoch fills the ring (capacity 256 = 4 blocks) and is never read.
    std::vector<float> oldBlock(kBlock, 0.9f);
    for (cr::U32 i = 0; i < 4; ++i)
    {
        ASSERT_EQ(cr::CaptureRingProducerWrite(h, oldBlock.data(), kBlock, 1u, i), cr::WRITE_OK);
    }
    EXPECT_EQ(cr::CaptureRingAvailableFrames(h), 256u);
    EXPECT_EQ(h->generationFlushes, 1u); // first write of gen 1 opened the epoch

    // Reconnect epoch (generation 2): the flush discards the pre-reconnect
    // backlog (control plane; consumer is quiescent at attach).
    std::vector<float> newBlock(kBlock, 0.1f);
    for (cr::U32 i = 0; i < 4; ++i)
    {
        ASSERT_EQ(cr::CaptureRingProducerWrite(h, newBlock.data(), kBlock, 2u, i), cr::WRITE_OK);
    }

    EXPECT_GE(h->generationFlushes, 2u);
    std::vector<float> out(kBlock, -1.0f);
    int read = 0;
    while (cr::CaptureRingConsumerRead(h, out.data(), kBlock, kNoStaleFrames) == 1)
    {
        EXPECT_LE(peakAbs(out), 0.11f) << "old-epoch (0.9) audio must never be served after reconnect";
        ++read;
    }
    EXPECT_EQ(read, 4);
    EXPECT_EQ(h->overflowDrops, 0u);
}

TEST(CaptureRingContractTest, ConsumerStaleBacklogIsDiscardedWholeBlocks)
{
    auto region = makeRegion(512);
    initRegion(region, 512);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    // Fill a stale backlog of 0.5 that exceeds the consumer's threshold.
    std::vector<float> staleBlock(kBlock, 0.5f);
    for (cr::U32 i = 0; i < 8; ++i)
    {
        ASSERT_EQ(cr::CaptureRingProducerWrite(h, staleBlock.data(), kBlock, 1u, i), cr::WRITE_OK);
    }

    std::vector<float> out(kBlock, -1.0f);
    // Threshold of 2 blocks: the 8-block backlog is stale and must be dropped.
    EXPECT_EQ(cr::CaptureRingConsumerRead(h, out.data(), kBlock, 2u * kBlock), 0);
    EXPECT_GT(h->staleCatchupDrops, 0u);

    // After resync the consumer serves only audio produced afterwards.
    std::vector<float> freshBlock(kBlock, 0.25f);
    ASSERT_EQ(cr::CaptureRingProducerWrite(h, freshBlock.data(), kBlock, 1u, 8u), cr::WRITE_OK);
    EXPECT_EQ(cr::CaptureRingConsumerRead(h, out.data(), kBlock, 2u * kBlock), 1);
    EXPECT_NEAR(out[0], 0.25f, 1e-6f);
}

// --- Mono -> L/R DMA fill ----------------------------------------------------

TEST(CaptureRingContractTest, FillStereoDuplicatesMonoToLAndR)
{
    auto region = makeRegion();
    initRegion(region);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    std::vector<float> block(kBlock);
    for (cr::U32 i = 0; i < kBlock; ++i)
    {
        block[i] = static_cast<float>(i) / static_cast<float>(kBlock);
    }
    ASSERT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, 0u), cr::WRITE_OK);

    std::vector<float> stereo(kBlock * 2u, -1.0f);
    EXPECT_EQ(cr::CaptureRingConsumerFillStereo(h, stereo.data(), kBlock, kNoStaleFrames), 1);
    for (cr::U32 i = 0; i < kBlock; ++i)
    {
        EXPECT_NEAR(stereo[2u * i], block[i], 1e-6f) << "L channel";
        EXPECT_NEAR(stereo[2u * i + 1u], block[i], 1e-6f) << "R channel";
    }
}

TEST(CaptureRingContractTest, FillStereoServesExactSilenceOnUnderrun)
{
    auto region = makeRegion();
    initRegion(region);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    // Ring empty: the DMA fill must be all-zero digital silence, never stale.
    std::vector<float> stereo(kBlock * 2u, 7.0f);
    EXPECT_EQ(cr::CaptureRingConsumerFillStereo(h, stereo.data(), kBlock, kNoStaleFrames), 0);
    for (const float s : stereo)
    {
        EXPECT_EQ(s, 0.0f) << "underrun fill must be exact digital silence";
    }
    EXPECT_GT(h->underruns, 0u);
}

TEST(CaptureRingContractTest, FillStereoWithoutRegionIsSilence)
{
    std::vector<float> stereo(kBlock * 2u, 3.0f);
    EXPECT_EQ(cr::CaptureRingConsumerFillStereo(nullptr, stereo.data(), kBlock, 0u), 0);
    for (const float s : stereo)
    {
        EXPECT_EQ(s, 0.0f);
    }
}

// --- Mono float32 -> PCM32 L/R DMA fill (Q5-A3B boundary format) -------------

TEST(CaptureRingContractTest, FloatToPcm32ClampsNanInfAndRange)
{
    // NaN -> silence.
    EXPECT_EQ(cr::CaptureRingFloatToPcm32(std::numeric_limits<float>::quiet_NaN()), 0);
    // +Inf / -Inf clamp to the PCM32 extremes, not UB/out-of-range.
    EXPECT_GE(cr::CaptureRingFloatToPcm32(std::numeric_limits<float>::infinity()), 0);
    EXPECT_LE(cr::CaptureRingFloatToPcm32(-std::numeric_limits<float>::infinity()), 0);
    // Large magnitudes clamp exactly to the safe scale.
    EXPECT_EQ(cr::CaptureRingFloatToPcm32(2.0f), cr::CaptureRingFloatToPcm32(1.0f));
    EXPECT_EQ(cr::CaptureRingFloatToPcm32(-2.0f), cr::CaptureRingFloatToPcm32(-1.0f));
    // Mid range: monotonic, sign preserved, magnitude within int32.
    const int v05 = cr::CaptureRingFloatToPcm32(0.5f);
    const int v10 = cr::CaptureRingFloatToPcm32(1.0f);
    const int vN05 = cr::CaptureRingFloatToPcm32(-0.5f);
    EXPECT_GT(v10, v05);
    EXPECT_LT(vN05, 0);
    EXPECT_EQ(vN05, -v05);
    EXPECT_LE(v10, 2147483647);
    EXPECT_GE(vN05, -2147483647);
    EXPECT_EQ(cr::CaptureRingFloatToPcm32(0.0f), 0);
}

TEST(CaptureRingContractTest, FillPcm32StereoDuplicatesMonoToClampedLR)
{
    auto region = makeRegion();
    initRegion(region);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    std::vector<float> block(kBlock);
    for (cr::U32 i = 0; i < kBlock; ++i)
    {
        block[i] = static_cast<float>(i) / static_cast<float>(kBlock) - 0.5f;
    }
    // Include an out-of-range sample the fill must clamp, not blow up.
    block[kBlock / 2] = 1.5f;
    ASSERT_EQ(cr::CaptureRingProducerWrite(h, block.data(), kBlock, 1u, 0u), cr::WRITE_OK);

    std::vector<int> pcm(kBlock * 2u, -7);
    EXPECT_EQ(cr::CaptureRingConsumerFillPcm32Stereo(h, pcm.data(), kBlock, kNoStaleFrames), 1);
    for (cr::U32 i = 0; i < kBlock; ++i)
    {
        const int expected = cr::CaptureRingFloatToPcm32(block[i]);
        EXPECT_EQ(pcm[2u * i], expected) << "L channel frame " << i;
        EXPECT_EQ(pcm[2u * i + 1u], expected) << "R channel frame " << i;
    }
    // The over-range sample clamps (L == R == clamped value, no UB).
    EXPECT_EQ(pcm[kBlock], pcm[kBlock + 1u]);
}

TEST(CaptureRingContractTest, FillPcm32StereoExactSilenceOnUnderrunAndNoRegion)
{
    auto region = makeRegion();
    initRegion(region);
    cr::CaptureRingHeader* h = headerOf(region);
    h->flags |= cr::REGION_FLAG_CONNECTED;

    std::vector<int> pcm(kBlock * 2u, 7);
    EXPECT_EQ(cr::CaptureRingConsumerFillPcm32Stereo(h, pcm.data(), kBlock, kNoStaleFrames), 0);
    for (const int v : pcm)
    {
        EXPECT_EQ(v, 0) << "underrun PCM32 fill must be exact digital silence";
    }
    EXPECT_GT(h->underruns, 0u);

    std::vector<int> pcmNoRegion(kBlock * 2u, 7);
    EXPECT_EQ(cr::CaptureRingConsumerFillPcm32Stereo(nullptr, pcmNoRegion.data(), kBlock, 0u), 0);
    for (const int v : pcmNoRegion)
    {
        EXPECT_EQ(v, 0);
    }
}

// --- SharedRingCaptureSink over an adopted region ------------------------------

TEST(SharedRingCaptureSinkAdoptedRegionTest, ConstructedOverAnInitializedRegion)
{
    auto region = makeRegion();
    initRegion(region);
    va::SharedRingCaptureSink sink(
        {48000, 1, static_cast<int>(kBlock), audient::transport::SampleType::Float32},
        region.data(), static_cast<std::uint64_t>(region.size()));

    EXPECT_TRUE(sink.connected());
    EXPECT_EQ(sink.capacityFrames(), kCapacity);

    // Adopted sink writes into the SAME memory a raw consumer can read.
    std::vector<float> block(kBlock, 0.5f);
    ASSERT_TRUE(sink.writeMono(block.data(), kBlock, 1u, 0u));
    EXPECT_EQ(cr::CaptureRingConsumerRead(headerOf(region), block.data(), kBlock, kNoStaleFrames), 1);
    EXPECT_NEAR(block[0], 0.5f, 1e-6f);
}

TEST(SharedRingCaptureSinkAdoptedRegionTest, RejectsMismatchedRegion)
{
    // Wrong format tag / sample rate must be rejected at construction.
    auto region = makeRegion();
    initRegion(region);
    headerOf(region)->sampleRateHz = 44100u;
    EXPECT_THROW(va::SharedRingCaptureSink(
                     {48000, 1, static_cast<int>(kBlock), audient::transport::SampleType::Float32},
                     region.data(), static_cast<std::uint64_t>(region.size())),
                 std::invalid_argument);
}
