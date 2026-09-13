#include "vst3/Vst3Chain.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

// Unit coverage for Vst3Chain snapshot/reap/bypass bookkeeping that does not
// need a real plug-in (processor slots stay null/unused here; real signal
// behavior with licensed plug-ins is covered by the gated integration test).
// Realtime contract focus: publish/reap/settled never touch the audio path
// beyond a relaxed counter, and bypass/empty snapshots are pure passthrough.

namespace
{

std::vector<float> makeBlock(std::size_t frames)
{
    std::vector<float> block(frames, 0.0f);
    for (std::size_t i = 0; i < frames; ++i)
    {
        block[i] = 0.5f * std::sinf(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
    }
    return block;
}

} // namespace

TEST(Vst3ChainTest, DefaultEmptyChainIsPassthroughAndSettled)
{
    audient::vst3::Vst3Chain chain;
    EXPECT_TRUE(chain.settled());
    EXPECT_EQ(chain.blockCounter(), 0u);

    const std::vector<float> input = makeBlock(32);
    std::vector<float> output(input.size(), -1.0f);
    audient::vst3::Vst3Chain::processMono(input.data(), output.data(), input.size(), &chain);

    // Empty snapshots pass the buffer through untouched but never write to an
    // output that does not alias the input (the graph always calls in-place).
    EXPECT_EQ(chain.blockCounter(), 1u);
    for (std::size_t i = 0; i < output.size(); ++i)
    {
        EXPECT_EQ(output[i], -1.0f) << "empty chain must not write the output buffer";
    }
    EXPECT_TRUE(chain.settled());
}

TEST(Vst3ChainTest, PublishRejectsMoreThanMaxSlots)
{
    audient::vst3::Vst3Chain chain;
    std::vector<audient::vst3::Vst3Chain::Slot> tooMany;
    for (std::size_t i = 0; i < audient::vst3::Vst3Chain::kMaxSlots + 1; ++i)
    {
        tooMany.push_back({});
    }
    // Rejected: returns false regardless of the counter and must not retire anything.
    std::vector<float> block = makeBlock(16);
    audient::vst3::Vst3Chain::processMono(block.data(), block.data(), block.size(), &chain);
    EXPECT_FALSE(chain.publish(std::move(tooMany), false)) << "over-limit publish must be rejected";
    EXPECT_TRUE(chain.settled()) << "a rejected publish must leave the chain settled";

    // A valid publish returns true, reports the current counter floor, and
    // defers reclamation.
    std::uint64_t floor = 0;
    EXPECT_TRUE(chain.publish({}, false, &floor));
    EXPECT_EQ(floor, 1u);
    EXPECT_FALSE(chain.settled());
    for (std::uint64_t i = 0; i < audient::vst3::Vst3Chain::kGraceBlocks; ++i)
    {
        audient::vst3::Vst3Chain::processMono(block.data(), block.data(), block.size(), &chain);
    }
    chain.reap();
    EXPECT_TRUE(chain.settled());
}

TEST(Vst3ChainTest, RetiredSnapshotIsReclaimedAfterGrace)
{
    audient::vst3::Vst3Chain chain;
    std::vector<float> block = makeBlock(16);
    // Advance a couple of blocks first so the returned floor is unambiguous.
    audient::vst3::Vst3Chain::processMono(block.data(), block.data(), block.size(), &chain);
    audient::vst3::Vst3Chain::processMono(block.data(), block.data(), block.size(), &chain);
    std::uint64_t floor = 0;
    ASSERT_TRUE(chain.publish({}, false, &floor));
    EXPECT_EQ(floor, 2u);

    // Freshly published snapshot is retired and waiting for grace.
    EXPECT_FALSE(chain.settled());

    // One block is not enough; the grace window is kGraceBlocks.
    for (std::uint64_t i = 0; i < audient::vst3::Vst3Chain::kGraceBlocks - 1; ++i)
    {
        audient::vst3::Vst3Chain::processMono(block.data(), block.data(), block.size(), &chain);
    }
    EXPECT_FALSE(chain.settled());

    // After the full grace has elapsed, reap() frees the retired snapshot.
    audient::vst3::Vst3Chain::processMono(block.data(), block.data(), block.size(), &chain);
    chain.reap();
    EXPECT_TRUE(chain.settled());
    EXPECT_GE(chain.blockCounter(), 2u + audient::vst3::Vst3Chain::kGraceBlocks);
}

TEST(Vst3ChainTest, WholeChainBypassLeavesBufferUntouched)
{
    audient::vst3::Vst3Chain chain;
    audient::vst3::Vst3Chain::Slot slot;
    slot.processor = nullptr; // null slot, but whole-chain bypass wins either way
    slot.bypass = false;
    ASSERT_TRUE(chain.publish({slot}, true));

    const std::vector<float> input = makeBlock(32);
    std::vector<float> output(input.size(), -2.0f);
    audient::vst3::Vst3Chain::processMono(input.data(), output.data(), input.size(), &chain);
    for (std::size_t i = 0; i < output.size(); ++i)
    {
        EXPECT_EQ(output[i], -2.0f) << "whole-chain bypass must not write the buffer";
    }
}

TEST(Vst3ChainTest, NullOrBypassedSlotDoesNotMatterForMonotonicCounter)
{
    audient::vst3::Vst3Chain chain;
    audient::vst3::Vst3Chain::Slot slot;
    slot.processor = nullptr;
    ASSERT_TRUE(chain.publish({slot}, false));

    std::vector<float> block = makeBlock(8);
    audient::vst3::Vst3Chain::processMono(block.data(), block.data(), block.size(), &chain);
    EXPECT_EQ(chain.blockCounter(), 1u);
    chain.reap();

    // Stereo path keeps the same monotonic counter and passthrough contract.
    std::vector<float> left = makeBlock(8);
    std::vector<float> right = makeBlock(8);
    std::vector<float> leftOut(8, -3.0f);
    std::vector<float> rightOut(8, -3.0f);
    audient::vst3::Vst3Chain::processStereo(left.data(), right.data(), leftOut.data(), rightOut.data(), left.size(), &chain);
    EXPECT_EQ(chain.blockCounter(), 2u);
    for (std::size_t i = 0; i < leftOut.size(); ++i)
    {
        EXPECT_EQ(leftOut[i], -3.0f);
        EXPECT_EQ(rightOut[i], -3.0f);
    }
}

// VST-016: totalLatencySamples reads the published snapshot's ACTIVE slots.
// These unit cases use null processors (no plug-in needed) and cover the
// empty/bypass/default paths; real-latency aggregation is covered by the
// integration tests.
TEST(Vst3ChainTest, TotalLatencyIsZeroForEmptyOrWholeBypassedChain)
{
    audient::vst3::Vst3Chain chain;
    EXPECT_EQ(chain.totalLatencySamples(), 0u) << "default (empty) snapshot has no latency";

    audient::vst3::Vst3Chain::Slot slot; // null processor
    ASSERT_TRUE(chain.publish({slot}, true)); // whole-chain bypass
    EXPECT_EQ(chain.totalLatencySamples(), 0u) << "whole-chain bypass contributes zero";

    ASSERT_TRUE(chain.publish({slot}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 0u) << "null slot contributes zero";
}

TEST(Vst3ChainTest, TotalLatencyIgnoresBypassedAndNullSlots)
{
    audient::vst3::Vst3Chain chain;
    audient::vst3::Vst3Chain::Slot nullSlot;  // processor == nullptr
    audient::vst3::Vst3Chain::Slot bypassSlot; // null processor but bypass flag set
    bypassSlot.processor = nullptr;
    bypassSlot.bypass = true;
    ASSERT_TRUE(chain.publish({nullSlot, bypassSlot}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 0u) << "null and bypassed slots contribute nothing";
}