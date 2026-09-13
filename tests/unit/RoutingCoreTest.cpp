#include "core/AllocationTracker.h"
#include "routing/RoutingCore.h"
#include "routing/RoutingTypes.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

#if defined(_MSC_VER) && !defined(NDEBUG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
std::uintptr_t currentThreadId()
{
    return static_cast<std::uintptr_t>(::GetCurrentThreadId());
}
} // namespace
#else
namespace
{
std::uintptr_t currentThreadId()
{
    return 0;
}
} // namespace
#endif

namespace core = audient::core;

// Phase 5 routing-core (Slice N): the two fixed v1 paths, logical bus IDs
// independent of any backend, and realtime cleanliness. Chain hooks are plain
// function pointers so VST3/WASAPI/virtual-ASIO adapters can bind later without
// rewriting the graph (project plan routing-core rescope 2026-09-03).

namespace
{

using audient::routing::BlockBinding;
using audient::routing::BusId;
using audient::routing::RoutingCore;

constexpr std::size_t kFrames = 64;
constexpr std::size_t kMaxBlock = 128;

float sumOf(const std::array<float, kFrames>& buffer)
{
    float total = 0.0f;
    for (const float sample : buffer)
    {
        total += sample;
    }
    return total;
}

float sumOf(const std::vector<float>& data)
{
    float total = 0.0f;
    for (const float sample : data)
    {
        total += sample;
    }
    return total;
}

struct BlockHost
{
    // Runtime slot 0 buffers keep the legacy single-channel names so existing
    // single-channel test bodies stay unchanged.
    std::array<float, kFrames> input{};
    std::array<float, kFrames> micRaw{};
    std::array<float, kFrames> micProcessed{};
    std::array<float, kFrames> micUplink{};
    std::array<float, kFrames> monitor{};
    // Runtime slot 1 buffers for the multi-channel cases.
    std::array<float, kFrames> input1{};
    std::array<float, kFrames> micRaw1{};
    std::array<float, kFrames> micProcessed1{};
    std::array<float, kFrames> monitor1{};
    // Downlink.
    std::array<float, kFrames> playbackLeft{};
    std::array<float, kFrames> playbackRight{};
    std::array<float, kFrames> processedL{};
    std::array<float, kFrames> processedR{};
    std::array<float, kFrames> physOutL{};
    std::array<float, kFrames> physOutR{};

    BlockBinding binding(std::size_t channels = 1, std::size_t uplinkChannel = 0)
    {
        BlockBinding d;
        d.frames = kFrames;
        d.inputChannels = channels;
        d.micUplinkChannel = uplinkChannel;
        d.inputSource[0] = input.data();
        d.inputRawTap[0] = micRaw.data();
        d.inputProcessed[0] = micProcessed.data();
        d.inputMonitorFeed[0] = monitor.data();
        if (channels > 1)
        {
            d.inputSource[1] = input1.data();
            d.inputRawTap[1] = micRaw1.data();
            d.inputProcessed[1] = micProcessed1.data();
            d.inputMonitorFeed[1] = monitor1.data();
        }
        d.micUplink = micUplink.data();
        d.playbackLeft = playbackLeft.data();
        d.playbackRight = playbackRight.data();
        d.outputProcessedLeft = processedL.data();
        d.outputProcessedRight = processedR.data();
        d.physicalOutputLeft = physOutL.data();
        d.physicalOutputRight = physOutR.data();
        return d;
    }
};

void fill(std::array<float, kFrames>& buffer, float value)
{
    buffer.fill(value);
}

// A trivial "input chain": gain the mono buffer.
void gainMono(const float* in, float* out, std::size_t frames, void* context)
{
    const float gain = *static_cast<const float*>(context);
    for (std::size_t i = 0; i < frames; ++i)
    {
        out[i] = in[i] * gain;
    }
}

// A trivial "output chain": gain stereo, keeping L/R distinct.
void gainStereo(const float* lin, const float* rin, float* lout, float* rout, std::size_t frames, void* context)
{
    const float gain = *static_cast<const float*>(context);
    for (std::size_t i = 0; i < frames; ++i)
    {
        lout[i] = lin[i] * gain;
        rout[i] = rin[i] * gain;
    }
}

// A bounded latency source for the routing model tests: returns the value the
// test stored (no plug-in involved; the routing core must not hard-code any).
std::uint32_t latencyQuery(void* context)
{
    return *static_cast<std::uint32_t*>(context);
}

} // namespace

TEST(RoutingTypesTest, BusIdsAreStableLogicalRoles)
{
    // Logical IDs are the backend-independent contract: names and channel
    // counts never vary with the transport.
    EXPECT_EQ(audient::routing::channelCount(BusId::PhysicalInput1), 1u);
    EXPECT_EQ(audient::routing::channelCount(BusId::MicUplink), 1u);
    EXPECT_EQ(audient::routing::channelCount(BusId::MicRaw), 1u);
    EXPECT_EQ(audient::routing::channelCount(BusId::MicProcessed), 1u);
    EXPECT_EQ(audient::routing::channelCount(BusId::PlaybackBus), 2u);
    EXPECT_EQ(audient::routing::channelCount(BusId::OutputProcessed), 2u);
    EXPECT_EQ(audient::routing::channelCount(BusId::PhysicalOutputLeft), 1u);
    EXPECT_EQ(audient::routing::channelCount(BusId::PhysicalOutputRight), 1u);

    EXPECT_STREQ(audient::routing::busName(BusId::PhysicalInput1), "Physical Input 1");
    EXPECT_STREQ(audient::routing::busName(BusId::MicProcessed), "Mic Processed");
    EXPECT_STREQ(audient::routing::busName(BusId::PlaybackBus), "Playback Bus");
    EXPECT_STREQ(audient::routing::busName(BusId::PhysicalOutputLeft), "Physical Output 1");
}

TEST(RoutingCoreTest, MicPathRawAndProcessedAreIndependent)
{
    RoutingCore core(kMaxBlock);
    float gain = 0.5f;
    core.setInputChain(&gainMono, &gain);

    BlockHost host;
    fill(host.input, 0.25f);
    BlockBinding binding = host.binding();
    core.process(binding);

    // MicRaw is the pre-chain tap; MicProcessed is the chain output; the
    // uplink carries only the PROCESSED mic.
    EXPECT_NEAR(host.micRaw[0], 0.25f, 1e-6f);
    EXPECT_NEAR(host.micProcessed[0], 0.25f * gain, 1e-6f);
    EXPECT_NEAR(host.micUplink[0], 0.25f * gain, 1e-6f) << "uplink must carry processed mic, never raw";
    EXPECT_NEAR(host.monitor[0], 0.25f * gain, 1e-6f) << "monitor tap = processed mic";
}

TEST(RoutingCoreTest, DownlinkPathPreservesLeftRightThroughOutputChain)
{
    RoutingCore core(kMaxBlock);
    float gain = 0.9f;
    core.setOutputChain(&gainStereo, &gain);

    BlockHost host;
    fill(host.playbackLeft, 0.2f);
    fill(host.playbackRight, -0.4f);
    BlockBinding binding = host.binding();
    core.process(binding);

    EXPECT_NEAR(host.processedL[0], 0.2f * gain, 1e-6f);
    EXPECT_NEAR(host.processedR[0], -0.4f * gain, 1e-6f);
    EXPECT_NEAR(host.physOutL[0], 0.2f * gain, 1e-6f) << "physical out 1 = output-chain L";
    EXPECT_NEAR(host.physOutR[0], -0.4f * gain, 1e-6f) << "physical out 2 = output-chain R";
    EXPECT_NE(host.physOutL[0], host.physOutR[0]) << "L/R mapping must survive the downlink path";
}

TEST(RoutingCoreTest, EmptyChainsActAsWire)
{
    RoutingCore core(kMaxBlock);
    // No hooks installed: both paths are audio wires.
    BlockHost host;
    fill(host.input, 0.3f);
    fill(host.playbackLeft, 0.1f);
    fill(host.playbackRight, 0.2f);
    BlockBinding binding = host.binding();
    core.process(binding);

    EXPECT_NEAR(host.micUplink[0], 0.3f, 1e-6f);
    EXPECT_NEAR(host.micRaw[0], 0.3f, 1e-6f);
    EXPECT_NEAR(host.physOutL[0], 0.1f, 1e-6f);
    EXPECT_NEAR(host.physOutR[0], 0.2f, 1e-6f);
}

TEST(RoutingCoreTest, DownlinkNeverFeedsMicPath)
{
    // Structural isolation (AGENTS §4): a full-scale downlink with silence on
    // the physical input must leave the mic path silent even with the output
    // chain hook installed.
    RoutingCore core(kMaxBlock);
    float downlinkGain = 2.0f;
    core.setOutputChain(&gainStereo, &downlinkGain);

    BlockHost host;
    fill(host.playbackLeft, 1.0f);
    fill(host.playbackRight, -1.0f);
    host.input.fill(0.0f);
    BlockBinding binding = host.binding();
    core.process(binding);

    EXPECT_EQ(sumOf(host.micUplink), 0.0f) << "downlink must never leak into the mic uplink";
    EXPECT_EQ(sumOf(host.micRaw), 0.0f) << "downlink must never leak into mic raw";
    EXPECT_EQ(sumOf(host.micProcessed), 0.0f);
    EXPECT_NEAR(host.physOutL[0], 2.0f, 1e-6f) << "downlink alone drives the physical output";
}

TEST(RoutingCoreTest, NullSinksAreDroppedSafely)
{
    // Null sinks are legal (unwired outputs): the core must not dereference
    // them; processing continues for the wired buses.
    RoutingCore core(kMaxBlock);
    float gain = 0.5f;
    core.setInputChain(&gainMono, &gain);
    float downlinkGain = 0.8f;
    core.setOutputChain(&gainStereo, &downlinkGain);

    BlockHost host;
    fill(host.input, 0.4f);
    fill(host.playbackLeft, 0.1f);
    fill(host.playbackRight, 0.2f);
    BlockBinding binding = host.binding();
    binding.micUplink = nullptr;              // virtual mic not connected
    binding.physicalOutputRight = nullptr;    // right physical output not connected
    binding.inputMonitorFeed[0] = nullptr;
    core.process(binding);

    EXPECT_NEAR(host.micProcessed[0], 0.4f * gain, 1e-6f);
    EXPECT_NEAR(host.micRaw[0], 0.4f, 1e-6f);
    EXPECT_NEAR(host.physOutL[0], 0.1f * downlinkGain, 1e-6f) << "left physical output still routed";
    // No crash, no garbage write attempted.
}

TEST(RoutingCoreTest, NullDownlinkSourceDoesNotReplayStaleAudio)
{
    // A missing downlink source must not replay the previous block's buffered
    // audio to the physical outputs (AGENTS §7 never-replay-stale rule).
    RoutingCore core(kMaxBlock);

    BlockHost host;
    fill(host.playbackLeft, 0.9f);
    fill(host.playbackRight, -0.7f);
    BlockBinding binding = host.binding();
    core.process(binding);
    EXPECT_NEAR(host.physOutL[0], 0.9f, 1e-6f) << "wired block reaches physical output";

    // Now the playback source is dropped (null). Physical output must go silent,
    // NOT repeat the previous 0.9/-0.7 block.
    fill(host.physOutL, 0.0f);
    fill(host.physOutR, 0.0f);
    binding.playbackLeft = nullptr;
    binding.playbackRight = nullptr;
    core.process(binding);
    EXPECT_EQ(sumOf(host.physOutL), 0.0f) << "null downlink must not replay stale audio";
    EXPECT_EQ(sumOf(host.physOutR), 0.0f);

    // A single missing side on the wire path zeroes only that side.
    fill(host.playbackLeft, 0.5f);
    binding.playbackLeft = nullptr;
    binding.playbackRight = host.playbackRight.data();
    core.process(binding);
    EXPECT_EQ(sumOf(host.physOutL), 0.0f) << "missing left side drops to silence";
    EXPECT_NEAR(host.physOutR[0], -0.7f, 1e-6f) << "wired right side still routes";
}

TEST(RoutingCoreTest, OversizedBlockIsIgnored)
{
    // frames > maxBlock is a hard no-op: prevents buffer overruns if a backend
    // ever hands a larger block than the core was configured for.
    RoutingCore core(64);
    float gain = 0.5f;
    core.setInputChain(&gainMono, &gain);

    std::vector<float> big(kMaxBlock, 0.5f);
    std::vector<float> out(kMaxBlock, -1.0f);
    BlockBinding binding;
    binding.frames = kMaxBlock; // > core maxBlock (64)
    binding.inputChannels = 1;
    binding.inputSource[0] = big.data();
    binding.micUplinkChannel = 0;
    binding.micUplink = out.data();
    core.process(binding);

    EXPECT_EQ(sumOf(out), -1.0f * kMaxBlock) << "oversized block must be left untouched";
}

TEST(RoutingCoreTest, ZeroFramesNoOp)
{
    RoutingCore core(kMaxBlock);
    float gain = 0.5f;
    core.setInputChain(&gainMono, &gain);
    BlockHost host;
    fill(host.input, 0.5f);
    BlockBinding binding = host.binding();
    binding.frames = 0;
    core.process(binding);
    EXPECT_EQ(host.micUplink[0], 0.0f) << "zero-frame block must not write";
}

TEST(RoutingCoreTest, CallbackBodyAllocatesZeroBytes)
{
    // Realtime rule: process() must never allocate on the calling thread.
    const bool supported = core::isAllocationTrackerEnabled() ||
                           core::installAllocationTracker() == core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker is active only when NDEBUG is unset";
    }

    RoutingCore core(kMaxBlock);
    // A wire input chain (identity) so process() touches the realtime path.
    float identityGain = 1.0f;
    core.setInputChain(&gainMono, &identityGain);
    core.setOutputChain(&gainStereo, &identityGain);

    BlockHost host;
    fill(host.input, 0.1f);
    fill(host.playbackLeft, 0.1f);
    fill(host.playbackRight, -0.1f);
    BlockBinding binding = host.binding();

    const std::uint64_t baseline = core::allocationCountOnRealtimeThread();
    core::markThreadRealtime(currentThreadId());
    for (int i = 0; i < 20000; ++i)
    {
        core.process(binding);
    }
    core::clearRealtimeMark();

    EXPECT_EQ(core::allocationCountOnRealtimeThread(), baseline) << "routing process must not allocate";
}

TEST(RoutingCoreTest, TwoInputSlotsProcessIndependently)
{
    // A/D: two runtime slots with independent sources, chains, taps, processed
    // buffers and monitor feeds. Channel 1 must never bleed into channel 0.
    RoutingCore core(kMaxBlock);
    float gain0 = 0.5f;
    float gain1 = 2.0f;
    core.setInputChain(0, &gainMono, &gain0);
    core.setInputChain(1, &gainMono, &gain1);

    BlockHost host;
    fill(host.input, 0.25f);  // channel 0
    fill(host.input1, -0.3f); // channel 1
    BlockBinding binding = host.binding(/*channels=*/2, /*uplink=*/0);
    core.process(binding);

    EXPECT_NEAR(host.micRaw[0], 0.25f, 1e-6f);
    EXPECT_NEAR(host.micProcessed[0], 0.25f * 0.5f, 1e-6f);
    EXPECT_NEAR(host.micRaw1[0], -0.3f, 1e-6f);
    EXPECT_NEAR(host.micProcessed1[0], -0.3f * 2.0f, 1e-6f);
    EXPECT_NEAR(host.monitor[0], 0.25f * 0.5f, 1e-6f) << "slot 0 monitor feed is independent";
    EXPECT_NEAR(host.monitor1[0], -0.6f, 1e-6f) << "slot 1 monitor feed is independent";
    EXPECT_NEAR(host.micUplink[0], 0.125f, 1e-6f) << "uplink carries channel 0 only (no Phase-C summing)";
}

TEST(RoutingCoreTest, MicUplinkChannelIsSelectablePerSlot)
{
    // The uplink source is binding DATA (micUplinkChannel), never baked into
    // "slot 0 == the mic" inside the engine.
    RoutingCore core(kMaxBlock);
    BlockHost host;
    fill(host.input, 0.25f);
    fill(host.input1, 0.5f);
    BlockBinding binding = host.binding(/*channels=*/2, /*uplink=*/1);
    core.process(binding);
    EXPECT_NEAR(host.micUplink[0], 0.5f, 1e-6f) << "uplink must carry the SELECTED slot, not slot 0";
}

TEST(RoutingCoreTest, MissingSecondInputSlotLeavesFirstSlotUnaffected)
{
    // G: a missing second input must never alias slot 0 into slot 1's sink and
    // never disturb the wired slot.
    RoutingCore core(kMaxBlock);
    float gain = 0.5f;
    core.setInputChain(0, &gainMono, &gain);

    BlockHost host;
    fill(host.input, 0.2f);
    BlockBinding binding = host.binding(/*channels=*/2, /*uplink=*/0);
    binding.inputSource[1] = nullptr; // Input 2 absent this block
    core.process(binding);

    EXPECT_NEAR(host.micProcessed[0], 0.1f, 1e-6f) << "slot 0 still processed";
    EXPECT_NEAR(host.micUplink[0], 0.1f, 1e-6f) << "uplink still carries slot 0";
    EXPECT_EQ(sumOf(host.micProcessed1), 0.0f) << "core must never alias slot 0 into the missing slot";
}

TEST(RoutingCoreTest, TwoSlotCallbackAllocatesZeroBytes)
{
    // F: steady-state allocation must stay zero with multiple live channels.
    const bool supported = core::isAllocationTrackerEnabled() ||
                           core::installAllocationTracker() == core::TrackerState::Installed;
    if (!supported)
    {
        GTEST_SKIP() << "allocation tracker is active only when NDEBUG is unset";
    }

    RoutingCore core(kMaxBlock);
    float identityGain = 1.0f;
    core.setInputChain(0, &gainMono, &identityGain);
    core.setInputChain(1, &gainMono, &identityGain);

    BlockHost host;
    fill(host.input, 0.1f);
    fill(host.input1, -0.2f);
    BlockBinding binding = host.binding(/*channels=*/2, /*uplink=*/0);

    const std::uint64_t baseline = core::allocationCountOnRealtimeThread();
    core::markThreadRealtime(currentThreadId());
    for (int i = 0; i < 20000; ++i)
    {
        core.process(binding);
    }
    core::clearRealtimeMark();

    EXPECT_EQ(core::allocationCountOnRealtimeThread(), baseline) << "two-slot routing process must not allocate";
}

TEST(RoutingLatencyTest, PerChannelLatencySlotsAreIndependent)
{
    // #7: each runtime slot owns an independent latency seam. A wire/null chain
    // contributes zero route latency; changing one slot's latency never alters
    // another slot's nor the uplink LatencySnapshot (which tracks slot 0).
    RoutingCore core(kMaxBlock);
    EXPECT_EQ(core.inputChannelProcessedLatency(0), 0u) << "no source/chain: processed latency is 0";

    core.setSourceLatency(BusId::PhysicalInput1, 40u);
    std::uint32_t chain0 = 128u;
    std::uint32_t chain1 = 7u;
    core.setInputChainLatency(0, &latencyQuery, &chain0);
    core.setInputChainLatency(1, &latencyQuery, &chain1);

    EXPECT_EQ(core.inputChannelProcessedLatency(0), 40u + 128u);
    EXPECT_EQ(core.inputChannelProcessedLatency(1), 40u + 7u);
    EXPECT_EQ(core.latencySnapshot().micProcessed, 40u + 128u) << "uplink snapshot = slot 0";

    chain1 = 500u;
    EXPECT_EQ(core.inputChannelProcessedLatency(1), 40u + 500u);
    EXPECT_EQ(core.inputChannelProcessedLatency(0), 40u + 128u) << "slot 1 change must not alter slot 0";
    EXPECT_EQ(core.latencySnapshot().micProcessed, 40u + 128u);

    chain0 = 0u; // uplink chain became a wire: slot reports source latency only
    EXPECT_EQ(core.inputChannelProcessedLatency(0), 40u);
}

// --- VST-015 latency model (Slice O) ----------------------------------------

TEST(RoutingLatencyTest, DestinationEqualsSourcePlusRouteProcessing)
{
    // The core goal: destination bus latency = source latency + route
    // processing latency, computed from chain latency hooks (no hard-coded
    // plug-in value).
    RoutingCore core(kMaxBlock);
    std::uint32_t inputChainLat = 128u;
    std::uint32_t outputChainLat = 64u;
    core.setSourceLatency(BusId::PhysicalInput1, 32u);
    core.setSourceLatency(BusId::PlaybackBus, 16u);
    core.setInputChainLatency(&latencyQuery, &inputChainLat);
    core.setOutputChainLatency(&latencyQuery, &outputChainLat);

    const audient::routing::LatencySnapshot model = core.latencySnapshot();

    // Mic path: raw is a wire tap (source only); processed = source + input
    // chain; uplink = processed (wire).
    EXPECT_EQ(model.physicalInput, 32u);
    EXPECT_EQ(model.micRaw, 32u) << "raw tap adds no processing latency";
    EXPECT_EQ(model.micProcessed, 32u + 128u);
    EXPECT_EQ(model.micUplink, 32u + 128u) << "uplink carries processed mic only";

    // Downlink path: output processed = playback + output chain; physical
    // output = processed (wire).
    EXPECT_EQ(model.playbackBus, 16u);
    EXPECT_EQ(model.outputProcessed, 16u + 64u);
    EXPECT_EQ(model.physicalOutput, 16u + 64u);
}

TEST(RoutingLatencyTest, LatencyHooksAreIndependentAndOptional)
{
    RoutingCore core(kMaxBlock);

    // No hooks configured: all route processing latency is zero; only source
    // latencies are reported.
    EXPECT_EQ(core.latencySnapshot().micProcessed, 0u);
    EXPECT_EQ(core.latencySnapshot().outputProcessed, 0u);

    // Source latency alone (no chain latency) is preserved through wires.
    core.setSourceLatency(BusId::PhysicalInput1, 48u);
    core.setSourceLatency(BusId::PlaybackBus, 8u);
    const audient::routing::LatencySnapshot wireModel = core.latencySnapshot();
    EXPECT_EQ(wireModel.micRaw, 48u);
    EXPECT_EQ(wireModel.micProcessed, 48u) << "no input chain: processed == source";
    EXPECT_EQ(wireModel.outputProcessed, 8u) << "no output chain: processed == source";

    // Only the input chain latency is fed: the downlink must stay source-only.
    std::uint32_t inputChainLat = 200u;
    core.setInputChainLatency(&latencyQuery, &inputChainLat);
    const audient::routing::LatencySnapshot halfModel = core.latencySnapshot();
    EXPECT_EQ(halfModel.micProcessed, 48u + 200u);
    EXPECT_EQ(halfModel.outputProcessed, 8u) << "output chain latency hook not set -> 0";
}

TEST(RoutingLatencyTest, LatencyQueryReflectsChainTotalLatencyChanges)
{
    // The latency hook is a live query: changing the chain total (e.g. slot
    // add/bypass/remove) changes the model without reconfiguring the routing
    // core. This mirrors the VST-016/VST-015 integration where the query calls
    // Vst3Chain::totalLatencySamples.
    RoutingCore core(kMaxBlock);
    core.setSourceLatency(BusId::PhysicalInput1, 100u);

    std::uint32_t chainTotal = 30u;
    core.setInputChainLatency(&latencyQuery, &chainTotal);
    EXPECT_EQ(core.latencySnapshot().micProcessed, 130u);

    chainTotal = 75u; // chain state changed (latency grew)
    EXPECT_EQ(core.latencySnapshot().micProcessed, 175u) << "latency must be re-queried, not cached";

    chainTotal = 0u; // whole-chain bypass removes the path
    EXPECT_EQ(core.latencySnapshot().micProcessed, 100u);
}

TEST(RoutingLatencyTest, NonSourceBusLatencyIsIgnored)
{
    RoutingCore core(kMaxBlock);
    // setSourceLatency only accepts PhysicalInput1 / PlaybackBus; other ids are
    // ignored so the model cannot derive a bogus source.
    core.setSourceLatency(BusId::MicProcessed, 999u);
    core.setSourceLatency(BusId::PhysicalOutputLeft, 999u);
    EXPECT_EQ(core.latencySnapshot().micProcessed, 0u);
    EXPECT_EQ(core.latencySnapshot().physicalOutput, 0u);
}