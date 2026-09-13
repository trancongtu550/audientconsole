#include "TestPassthroughPlugin.h"

#include "core/AllocationTracker.h"
#include "channel/ChannelIdentity.h"
#include "production/ChannelVstChain.h"
#include "routing/RoutingCore.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

// Phase B B3 — independent per-channel VST3 ownership and publication
// (production::ChannelVstChain). Proves the ChannelVstChain owner wires real
// per-channel chains into the RoutingCore per-slot seams with zero cross-channel
// sharing, stable SlotId identity across reorder, transactional mutations,
// explicit off-RT retirement, quiescent cleanup and a stable owner address.

namespace
{

constexpr std::size_t kMaxBlock = 256;
constexpr std::size_t kFrames = 64;

using audient::routing::RoutingCore;

audient::channel::ChannelIdentity analog(unsigned ordinal)
{
    return audient::channel::makeAnalogInput(ordinal);
}

std::filesystem::path testModulePath(const char* moduleFileName)
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    EXPECT_GT(length, 0u);
    std::filesystem::path exe(modulePath);
    return exe.parent_path() / moduleFileName;
}

const std::string& kPassthroughModulePath()
{
    static const std::string path = testModulePath(TEST_PASSTHROUGH_VST3_MODULE).string();
    return path;
}

// Owns the factory/host/processor so the HOST outlives the processor that the
// ChannelVstChain borrows/moves: declare BEFORE any owner in the test so owners
// are destroyed first (reverse destruction order) and this host is destroyed
// last.
struct PreparedProcessor
{
    audient::vst3_test::TestPassthroughFactory factory;
    audient::vst3::Vst3Host host;
    std::unique_ptr<audient::vst3::Vst3Processor> processor;
    audient::vst3_test::TestPassthroughComponent* comp = nullptr;

    PreparedProcessor(float gain, long latencySamples)
        : factory(gain)
    {
        host.attachFactory(&factory);
        processor = host.createEffectProcessor(host.classes()[0], 48000.0, static_cast<long>(kMaxBlock),
                                               audient::vst3::BusLayout::Mono);
        comp = audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
        if (latencySamples > 0)
        {
            comp->setTestLatency(static_cast<std::uint32_t>(latencySamples));
            (void)processor->refreshLatencySamples();
        }
    }
};

// Drives one mono channel through RoutingCore slot `slot`.
struct MonoDriver
{
    RoutingCore core{kMaxBlock};
    std::array<float, kFrames> inputA{};
    std::array<float, kFrames> inputB{};
    std::array<float, kFrames> rawA{};
    std::array<float, kFrames> rawB{};
    std::array<float, kFrames> procA{};
    std::array<float, kFrames> procB{};
    std::array<float, kFrames> up{};

    void wire(audient::production::ChannelVstChain& c0, audient::production::ChannelVstChain& c1)
    {
        core.setInputChain(0, &audient::vst3::Vst3Chain::processMono, &c0.chain());
        core.setInputChain(1, &audient::vst3::Vst3Chain::processMono, &c1.chain());
        core.setInputChainLatency(0, &audient::vst3::Vst3Chain::chainLatencyQuery, &c0.chain());
        core.setInputChainLatency(1, &audient::vst3::Vst3Chain::chainLatencyQuery, &c1.chain());
    }

    audient::routing::BlockBinding binding()
    {
        audient::routing::BlockBinding b;
        b.frames = kFrames;
        b.inputChannels = 2;
        b.inputSource[0] = inputA.data();
        b.inputRawTap[0] = rawA.data();
        b.inputProcessed[0] = procA.data();
        b.inputSource[1] = inputB.data();
        b.inputRawTap[1] = rawB.data();
        b.inputProcessed[1] = procB.data();
        b.micUplinkChannel = 0;
        b.micUplink = up.data();
        return b;
    }

    void fillA(float value)
    {
        inputA.fill(value);
    }

    void fillB(float value)
    {
        inputB.fill(value);
    }

    void run(std::size_t blocks)
    {
        audient::routing::BlockBinding b = binding();
        for (std::size_t i = 0; i < blocks; ++i)
        {
            core.process(b);
        }
    }
};

void expectSynchronized(const audient::production::ChannelVstChain& chain)
{
    ASSERT_EQ(chain.slotCount(), chain.controlChannel().rack().size());
    for (std::size_t i = 0; i < chain.slotCount(); ++i)
    {
        const auto* rackSlot = chain.controlChannel().rack().slot(i);
        audient::production::SlotInfo info;
        ASSERT_NE(rackSlot, nullptr);
        ASSERT_TRUE(chain.slotInfoAt(i, info));
        EXPECT_EQ(info.id, rackSlot->id);
        EXPECT_EQ(info.name, rackSlot->name);
        EXPECT_EQ(info.bypass, rackSlot->bypass);
    }
}

} // namespace

static_assert(!std::is_copy_constructible<audient::production::ChannelVstChain>::value,
              "a wired ChannelVstChain must never be copied");
static_assert(!std::is_move_constructible<audient::production::ChannelVstChain>::value,
              "a wired ChannelVstChain must never be moved (stable RT context address)");

TEST(Vst3PerChannelChain, IndependentOwnershipAndDspIsolation)
{
    // A+B: two real per-channel chains, different gains, distinct signals.
    PreparedProcessor ch0Proc(2.0f, 0);
    PreparedProcessor ch1Proc(0.5f, 0);
    const void* const p0 = ch0Proc.processor.get();
    const void* const p1 = ch1Proc.processor.get();
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);
    EXPECT_NE(p0, p1) << "two distinct processors (never shared)";

    std::unique_ptr<audient::production::ChannelVstChain> c0 =
        std::make_unique<audient::production::ChannelVstChain>(analog(1));
    std::unique_ptr<audient::production::ChannelVstChain> c1 =
        std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);
    EXPECT_NE(&c0->chain(), &c1->chain()) << "independent Vst3Chain objects";

    ASSERT_EQ(c0->addPrepared(std::move(ch0Proc.processor), "ch0"), audient::production::ChainResult::Ok);
    ASSERT_EQ(c1->addPrepared(std::move(ch1Proc.processor), "ch1"), audient::production::ChainResult::Ok);

    MonoDriver driver;
    driver.wire(*c0, *c1);
    driver.fillA(0.25f);
    driver.fillB(0.5f);
    driver.run(1);

    EXPECT_NEAR(driver.procA[0], 0.25f * 2.0f, 1e-5f) << "channel 0 gain 2x";
    EXPECT_NEAR(driver.procB[0], 0.5f * 0.5f, 1e-5f) << "channel 1 gain 0.5x";
    EXPECT_NEAR(driver.rawA[0], 0.25f, 1e-6f);
    EXPECT_NEAR(driver.rawB[0], 0.5f, 1e-6f);
    EXPECT_NEAR(driver.up[0], 0.25f * 2.0f, 1e-5f) << "uplink follows channel 0 processed only";
}

TEST(Vst3PerChannelChain, StableSlotIdsAcrossReorderAndMutationIsolation)
{
    // C+M: stable SlotId identity across reorder + mutations on ch0 never touch
    // ch1's order/ids/latency.
    std::unique_ptr<audient::production::ChannelVstChain> c0 =
        std::make_unique<audient::production::ChannelVstChain>(analog(1));
    std::unique_ptr<audient::production::ChannelVstChain> c1 =
        std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);

    ASSERT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);
    expectSynchronized(*c0);
    ASSERT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);
    expectSynchronized(*c0);
    ASSERT_EQ(c1->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);
    expectSynchronized(*c1);

    const auto ids0 = c0->slotIds();
    ASSERT_EQ(ids0.size(), 2u);
    const audient::production::SlotId idA = ids0[0];
    const audient::production::SlotId idB = ids0[1];
    ASSERT_NE(idA, idB);
    audient::production::SlotInfo before;
    ASSERT_TRUE(c0->slotInfoById(idA, before));

    const auto ids1Before = c1->slotIds();
    const std::uint32_t lat1Before = c1->latencySamples();

    // Reorder idA from position 0 to position 1: id/processor association is
    // position-independent.
    ASSERT_EQ(c0->moveSlot(idA, 1), audient::production::ChainResult::Ok);
    const auto ids0After = c0->slotIds();
    ASSERT_EQ(ids0After.size(), 2u);
    EXPECT_EQ(ids0After[0], idB) << "idB now first";
    EXPECT_EQ(ids0After[1], idA) << "idA still present (stable), moved to position 1";
    expectSynchronized(*c0);
    audient::production::SlotInfo after;
    ASSERT_TRUE(c0->slotInfoById(idA, after));
    EXPECT_EQ(after.name, before.name) << "state follows the SlotId, not the position";

    // Channel 1 is untouched by the ch0 reorder.
    EXPECT_EQ(c1->slotIds(), ids1Before);
    EXPECT_EQ(c1->latencySamples(), lat1Before);
}

TEST(Vst3PerChannelChain, PerPluginAndWholeChainBypassIsolation)
{
    // D+E: bypassing channel 0 never affects channel 1's result.
    PreparedProcessor ch0Gain2(2.0f, 0);
    PreparedProcessor ch0GainHalf(0.5f, 0);
    PreparedProcessor ch1GainHalf(0.5f, 0);

    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);
    ASSERT_EQ(c0->addPrepared(std::move(ch0Gain2.processor), "g2"), audient::production::ChainResult::Ok);
    ASSERT_EQ(c0->addPrepared(std::move(ch0GainHalf.processor), "g0.5"), audient::production::ChainResult::Ok);
    ASSERT_EQ(c1->addPrepared(std::move(ch1GainHalf.processor), "g0.5"), audient::production::ChainResult::Ok);

    const auto ids0 = c0->slotIds();
    const audient::production::SlotId second = ids0[1];

    MonoDriver driver;
    driver.wire(*c0, *c1);
    driver.fillA(0.2f); // ch0: 0.2 -> *2 -> *0.5 = 0.2
    driver.fillB(0.8f); // ch1: 0.8 * 0.5 = 0.4
    driver.run(1);
    EXPECT_NEAR(driver.procA[0], 0.2f, 1e-5f);
    EXPECT_NEAR(driver.procB[0], 0.4f, 1e-5f);

    // Per-plugin bypass of the SECOND ch0 slot: ch0 becomes *2 only.
    ASSERT_EQ(c0->setSlotBypass(second, true), audient::production::ChainResult::Ok);
    expectSynchronized(*c0);
    driver.run(1);
    EXPECT_NEAR(driver.procA[0], 0.2f * 2.0f, 1e-5f) << "ch0 second slot bypassed";
    EXPECT_NEAR(driver.procB[0], 0.4f, 1e-5f) << "ch1 result unchanged";

    // Whole-chain bypass on ch0 only.
    c0->setWholeChainBypass(true);
    expectSynchronized(*c0);
    EXPECT_TRUE(c0->controlChannel().rack().wholeChainBypass());
    driver.run(1);
    EXPECT_NEAR(driver.procA[0], 0.2f, 1e-5f) << "ch0 whole-chain bypassed (wire)";
    EXPECT_NEAR(driver.procB[0], 0.4f, 1e-5f) << "ch1 remains fully processed";
    expectSynchronized(*c0);
    expectSynchronized(*c1);
}

TEST(Vst3PerChannelChain, PerChannelLatencyIsIndependentAndReplaces)
{
    // G: ch0 = 64, ch1 = 192; replace ch1 with a 32-sample chain -> ch1 32,
    // ch0 stays 64; per-slot routing latency reflects each channel.
    PreparedProcessor ch0Lat(2.0f, 64);
    PreparedProcessor ch1LatHigh(0.5f, 192);
    PreparedProcessor ch1LatLow(0.5f, 32);

    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);
    ASSERT_EQ(c0->addPrepared(std::move(ch0Lat.processor), "lat64"), audient::production::ChainResult::Ok);
    ASSERT_EQ(c1->addPrepared(std::move(ch1LatHigh.processor), "lat192"), audient::production::ChainResult::Ok);

    MonoDriver driver;
    driver.wire(*c0, *c1);
    EXPECT_EQ(c0->latencySamples(), 64u);
    EXPECT_EQ(c1->latencySamples(), 192u);
    EXPECT_EQ(driver.core.inputChannelProcessedLatency(0), 64u) << "slot 0 latency = chain 0";
    EXPECT_EQ(driver.core.inputChannelProcessedLatency(1), 192u) << "slot 1 latency = chain 1";

    // Replace ch1's single plug-in with the 32-sample one (retire old, add new).
    const audient::production::SlotId id1 = c1->slotIds()[0];
    ASSERT_EQ(c1->removeSlot(id1), audient::production::ChainResult::Ok);
    expectSynchronized(*c1);
    ASSERT_EQ(c1->addPrepared(std::move(ch1LatLow.processor), "lat32"), audient::production::ChainResult::Ok);
    expectSynchronized(*c1);
    driver.run(5); // advance chain counters so the retirement can be serviced
    c1->serviceRetirements();

    EXPECT_EQ(c1->latencySamples(), 32u) << "ch1 replaced chain latency is 32";
    EXPECT_EQ(c0->latencySamples(), 64u) << "ch0 latency unchanged by the ch1 replace";
    EXPECT_EQ(driver.core.inputChannelProcessedLatency(0), 64u);
    EXPECT_EQ(driver.core.inputChannelProcessedLatency(1), 32u);
}

TEST(Vst3PerChannelChain, EmptyChannelIsWireAndNeverAffectsProcessedChannel)
{
    // K: an empty channel is a clean wire-through and leaves the processed
    // channel untouched.
    PreparedProcessor ch0Gain2(2.0f, 0);

    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);
    ASSERT_EQ(c0->addPrepared(std::move(ch0Gain2.processor), "g2"), audient::production::ChainResult::Ok);
    EXPECT_TRUE(c1->empty());

    MonoDriver driver;
    driver.wire(*c0, *c1);
    driver.fillA(0.3f);
    driver.fillB(0.7f);
    driver.run(1);
    EXPECT_NEAR(driver.procA[0], 0.6f, 1e-5f) << "channel 0 processed normally";
    EXPECT_NEAR(driver.procB[0], 0.7f, 1e-5f) << "empty channel 1 is a wire";
}

TEST(Vst3PerChannelChain, ProductCapIsFourPerChannelIndependent)
{
    // F: 1-4 accepted, 5th rejected cleanly per channel; filling ch0 never
    // consumes ch1 capacity.
    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);

    for (std::size_t i = 0; i < audient::production::ChannelVstChain::kMaxSlots; ++i)
    {
        EXPECT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok)
            << "slots 1..4 accepted";
    }
    EXPECT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Full)
        << "5th slot rejected cleanly";
    EXPECT_EQ(c0->slotCount(), audient::production::ChannelVstChain::kMaxSlots);
    EXPECT_EQ(c0->controlChannel().rack().size(), audient::production::ChannelVstChain::kMaxSlots);
    expectSynchronized(*c0);

    EXPECT_TRUE(c1->empty()) << "ch1 capacity is independent of ch0";
    EXPECT_EQ(c1->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok)
        << "ch1 still accepts a slot while ch0 is full";
}

TEST(Vst3PerChannelChain, TransactionalRollbackOnLoadFailure)
{
    // N: a failed load/prepare leaves rack, heavy bundles and the published
    // chain unchanged; other channels are untouched.
    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);

    ASSERT_EQ(c0->addPrepared(nullptr, "null"), audient::production::ChainResult::Invalid);
    EXPECT_TRUE(c0->empty());
    EXPECT_EQ(c0->latencySamples(), 0u);

    const audient::production::ChainResult bad =
        c0->loadSlot("C:/definitely/not/a/vst3/plugin.vst3");
    EXPECT_EQ(bad, audient::production::ChainResult::Invalid);
    EXPECT_TRUE(c0->empty()) << "rack unchanged after a load failure";
    EXPECT_EQ(c0->controlChannel().rack().size(), 0u);
    EXPECT_EQ(c0->latencySamples(), 0u) << "published snapshot unchanged";
    expectSynchronized(*c0);
    EXPECT_TRUE(c1->empty()) << "other channel untouched";

    EXPECT_FALSE(c0->lastError().empty());
}

TEST(Vst3PerChannelChain, RetirementServedWithoutAnotherMutation)
{
    // O: remove a slot, advance grace blocks, call serviceRetirements() (no
    // other add/remove/reorder) -> the retired bundle is destroyed.
    PreparedProcessor ch0Proc(2.0f, 0);

    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);
    ASSERT_EQ(c0->addPrepared(std::move(ch0Proc.processor), "p"), audient::production::ChainResult::Ok);

    MonoDriver driver;
    driver.wire(*c0, *c1); // c1 is empty (wire) but its chain counter still advances
    driver.fillA(0.1f);
    driver.fillB(0.1f);
    driver.run(20);

    const audient::production::SlotId id = c0->slotIds()[0];
    ASSERT_EQ(c0->removeSlot(id), audient::production::ChainResult::Ok);
    EXPECT_EQ(c0->pendingRetirementCount(), 1u);
    EXPECT_EQ(c0->slotCount(), 0u);

    // Advance the block counter past the 2-block grace WITHOUT any mutation.
    driver.run(10);
    EXPECT_EQ(c0->pendingRetirementCount(), 1u) << "bundle still pending before serviceRetirements";
    c0->serviceRetirements();
    EXPECT_EQ(c0->pendingRetirementCount(), 0u) << "serviceRetirements destroyed the retired bundle";
    EXPECT_EQ(c0->slotCount(), 0u);
    EXPECT_EQ(c0->latencySamples(), 0u);
}

TEST(Vst3PerChannelChain, QuiescentCleanupAfterDetachDoesNotHang)
{
    // P: retire slots, stop processing (no further callbacks), then the explicit
    // quiescent cleanup / destructor completes safely without waiting for blocks.
    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    c0->configure(kMaxBlock);
    ASSERT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);
    ASSERT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);

    // Retire one slot but do NOT advance any block counter (stream is stopped).
    const audient::production::SlotId id = c0->slotIds()[0];
    ASSERT_EQ(c0->removeSlot(id), audient::production::ChainResult::Ok);
    EXPECT_EQ(c0->pendingRetirementCount(), 1u);
    EXPECT_EQ(c0->hasPendingRetirements(), true);

    // No process() calls happen from here on. Quiescent cleanup must not wait.
    c0->quiescentShutdown();
    EXPECT_EQ(c0->pendingRetirementCount(), 0u);
    EXPECT_EQ(c0->slotCount(), 0u);
    EXPECT_EQ(c0->controlChannel().rack().size(), 0u);
    EXPECT_FALSE(c0->hasPendingRetirements());
    expectSynchronized(*c0);

    // Destructor runs again on the already-shut-down owner: idempotent, no hang.
    c0->quiescentShutdown();
    c0.reset();
}

TEST(Vst3PerChannelChain, PublicationIsolationWhileOtherChannelProcesses)
{
    // H: repeatedly publish ch0 chain replacements while ch1 keeps processing;
    // ch1's result stays unchanged.
    PreparedProcessor ch0Gain2(2.0f, 0);
    PreparedProcessor ch1GainHalf(0.5f, 0);

    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);
    ASSERT_EQ(c0->addPrepared(std::move(ch0Gain2.processor), "ch0"), audient::production::ChainResult::Ok);
    ASSERT_EQ(c1->addPrepared(std::move(ch1GainHalf.processor), "ch1"), audient::production::ChainResult::Ok);

    const auto ch1Ids = c1->slotIds();
    const std::uint32_t ch1Lat = c1->latencySamples();

    MonoDriver driver;
    driver.wire(*c0, *c1);
    driver.fillA(0.25f);
    driver.fillB(0.5f);

    float ch1Result = 0.0f;
    audient::routing::BlockBinding b = driver.binding();
    for (std::size_t i = 0; i < 400; ++i)
    {
        driver.core.process(b);
        if (i % 50 == 0)
        {
            // Republish ch0 repeatedly (bypass toggle) while ch1 processes.
            c0->setWholeChainBypass((i / 50) % 2 == 0);
        }
        ch1Result = driver.procB[0];
        EXPECT_NEAR(ch1Result, 0.25f, 1e-5f) << "ch1 result never changes while ch0 republishes";
    }
    EXPECT_EQ(c1->slotIds(), ch1Ids);
    EXPECT_EQ(c1->latencySamples(), ch1Lat);
    expectSynchronized(*c0);
    expectSynchronized(*c1);
}

TEST(Vst3PerChannelChain, ProcessorStateFollowsStableSlotIdAfterReorder)
{
    // M (state half): component state operations bind to the SlotId, not to the
    // vector position; after reorder the same SlotId still reads its own state.
    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    c0->configure(kMaxBlock);
    ASSERT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);
    ASSERT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);

    const auto ids = c0->slotIds();
    const audient::production::SlotId idA = ids[0];
    const audient::production::SlotId idB = ids[1];

    std::vector<std::uint8_t> blobA;
    ASSERT_TRUE(c0->saveComponentState(idA, blobA));

    // Reorder idA to position 1 (idB is now at position 0).
    ASSERT_EQ(c0->moveSlot(idA, 1), audient::production::ChainResult::Ok);
    EXPECT_EQ(c0->slotIdAt(0), idB);
    expectSynchronized(*c0);

    std::vector<std::uint8_t> blobAAgain;
    ASSERT_TRUE(c0->saveComponentState(idA, blobAAgain)) << "state read binds to SlotId, not index";
    EXPECT_EQ(blobA, blobAAgain) << "deterministic state for the same SlotId after reorder";
}

TEST(Vst3PerChannelChain, TwoChannelCallbacksAllocateZeroBytes)
{
    // J: both real per-channel chain seams active -> zero steady-state callback
    // allocation.
    const bool supported = audient::core::isAllocationTrackerEnabled() ||
                           audient::core::installAllocationTracker() == audient::core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker is active only when NDEBUG is unset";
    }

    PreparedProcessor ch0Proc(2.0f, 0);
    PreparedProcessor ch1Proc(0.5f, 0);
    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    auto c1 = std::make_unique<audient::production::ChannelVstChain>(analog(2));
    c0->configure(kMaxBlock);
    c1->configure(kMaxBlock);
    ASSERT_EQ(c0->addPrepared(std::move(ch0Proc.processor), "ch0"), audient::production::ChainResult::Ok);
    ASSERT_EQ(c1->addPrepared(std::move(ch1Proc.processor), "ch1"), audient::production::ChainResult::Ok);

    MonoDriver driver;
    driver.wire(*c0, *c1);
    driver.fillA(0.1f);
    driver.fillB(0.2f);
    audient::routing::BlockBinding b = driver.binding();

    const std::uint64_t baseline = audient::core::allocationCountOnRealtimeThread();
    audient::core::markThreadRealtime(static_cast<std::uintptr_t>(::GetCurrentThreadId()));
    for (int i = 0; i < 20000; ++i)
    {
        driver.core.process(b);
    }
    audient::core::clearRealtimeMark();
    EXPECT_EQ(audient::core::allocationCountOnRealtimeThread(), baseline)
        << "two-channel real VST chain processing must not allocate";
}

TEST(Vst3PerChannelChain, StableOwnerAddressWhileWired)
{
    // Q: the chain context address installed into RoutingCore is constant across
    // mutations, reorders and retirements until detach.
    PreparedProcessor ch0Gain2(2.0f, 0);
    auto c0 = std::make_unique<audient::production::ChannelVstChain>(analog(1));
    c0->configure(kMaxBlock);
    ASSERT_EQ(c0->addPrepared(std::move(ch0Gain2.processor), "p1"), audient::production::ChainResult::Ok);
    ASSERT_EQ(c0->loadSlot(kPassthroughModulePath()), audient::production::ChainResult::Ok);

    audient::vst3::Vst3Chain* const wired = &c0->chain();
    for (int i = 0; i < 50; ++i)
    {
        (void)c0->slotIds();
        if (i % 2 == 0)
        {
            c0->setWholeChainBypass(i % 4 == 0);
        }
        c0->serviceRetirements();
        EXPECT_EQ(&c0->chain(), wired) << "chain address is stable while wired";
    }
}
