#include "transport/TransportFormat.h"
#include "transport/VirtualCaptureTransport.h"
#include "pico/PicoVirtualMicSink.h"
#include "virtual_audio/driver-protocol/IDriverCaptureSink.h"
#include "virtual_audio/driver-protocol/MockDriverCaptureSink.h"
#include "virtual_audio/driver-protocol/SharedRingCaptureSink.h"
#include "virtual_audio/driver-protocol/VirtualMicFeeder.h"
#include "virtual_audio/endpoint/SoftwareCaptureEndpoint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>
#include <vector>

namespace core = audient::transport;
namespace va = audient::virtual_audio;
namespace pico = audient::pico;

namespace
{

core::VirtualCaptureTransport makeEndpointTransport(std::size_t capacity = 4096)
{
    return core::VirtualCaptureTransport(
        {48000, 1, 64, core::SampleType::Float32}, capacity);
}

// Each feeder-facing sink under test is constructed the same way and exposes
// the same surface (writeMono/readMono/setConnected/snapshot/capacityFrames/
// availableFrames). Returning by value relies on C++17 guaranteed elision.
template <typename SinkT>
SinkT makeSink(std::size_t capacityFrames)
{
    return SinkT({48000, 1, 64, core::SampleType::Float32}, capacityFrames);
}

float maxAbs(const std::vector<float>& data)
{
    float peak = 0.0f;
    for (const float sample : data)
    {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

// Write `count` constant blocks into the endpoint transport.
void writeBlocks(core::VirtualCaptureTransport& transport, float value, int count)
{
    std::vector<float> block(64, value);
    for (int i = 0; i < count; ++i)
    {
        ASSERT_TRUE(transport.writeProcessedMic(block.data(), 64));
    }
}

} // namespace

// This suite runs the FULL Q3 feeder protocol against every IDriverCaptureSink
// implementation in the tree. MockDriverCaptureSink is the reference test
// double; SharedRingCaptureSink is the Q5-A3 concrete transport sink that must
// be a behavioral drop-in for it (the same bounded, fresh, generation-flush
// semantics) so the existing feeder contract is preserved when the concrete
// transport is used.
template <typename SinkT>
class VirtualMicFeederProtocolTest : public ::testing::Test
{
};

using SinkImplementations = ::testing::Types<va::MockDriverCaptureSink, va::SharedRingCaptureSink, pico::PicoVirtualMicSink>;
TYPED_TEST_SUITE(VirtualMicFeederProtocolTest, SinkImplementations);

TYPED_TEST(VirtualMicFeederProtocolTest, NormalFlowDeliversFreshProcessedMic)
{
    using Sink = TypeParam;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(1024);
    va::VirtualMicFeeder feeder(endpoint, 64);

    feeder.attachSink(&sink);
    writeBlocks(transport, 0.125f, 1);
    ASSERT_TRUE(feeder.tick(256));

    const va::VirtualMicFeeder::Snapshot snap = feeder.snapshot();
    EXPECT_EQ(snap.generation, 1u);
    EXPECT_EQ(snap.sequence, 1u);
    EXPECT_EQ(snap.pushedBlocks, 1u);
    EXPECT_EQ(snap.rejectedBlocks, 0u);

    std::vector<float> out(64, -1.0f);
    ASSERT_TRUE(sink.readMono(out.data(), 64, 256));
    EXPECT_NEAR(out[0], 0.125f, 1e-6f);
    EXPECT_EQ(sink.snapshot().acceptedWrites, 1u);
    EXPECT_EQ(sink.snapshot().lastAcceptedSequence, 0u);
}

TYPED_TEST(VirtualMicFeederProtocolTest, EngineAbsenceNeverInventsSilenceForTheSink)
{
    using Sink = TypeParam;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(1024);
    va::VirtualMicFeeder feeder(endpoint, 64);

    feeder.attachSink(&sink);
    // No engine blocks: the feeder must NOT synthesize/push silence.
    EXPECT_FALSE(feeder.tick(256));
    const va::VirtualMicFeeder::Snapshot snap = feeder.snapshot();
    EXPECT_EQ(snap.engineGaps, 1u);
    EXPECT_EQ(snap.pushedBlocks, 0u);
    EXPECT_EQ(sink.snapshot().acceptedWrites, 0u);

    std::vector<float> out(64, -1.0f);
    EXPECT_FALSE(sink.readMono(out.data(), 64, 256)) << "an empty driver sink serves digital silence";
}

TYPED_TEST(VirtualMicFeederProtocolTest, SinkStallIsBoundedAndNeverBlocksTheFeeder)
{
    using Sink = TypeParam;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(256); // only 4 blocks
    va::VirtualMicFeeder feeder(endpoint, 64);

    feeder.attachSink(&sink);
    // Real cadence: one fresh engine block per tick, so the endpoint stays
    // within the freshness threshold and only the SINK is saturated.
    for (int i = 0; i < 20; ++i)
    {
        writeBlocks(transport, 0.125f, 1);
        (void)feeder.tick(256); // must never block
    }

    EXPECT_EQ(feeder.snapshot().pushedBlocks, 4u) << "exactly capacity (4 blocks) fits while the client stalls";
    EXPECT_EQ(sink.availableFrames(), 256u) << "a stalled client bounds the sink at capacity";
    EXPECT_EQ(sink.snapshot().producedSamples, 256u) << "only capacity is committed while the client stalls";
    EXPECT_GT(feeder.snapshot().rejectedBlocks, 0u) << "accepted drops are rejected, never silent";
    EXPECT_GT(sink.snapshot().rejectedStall, 0u);
}

TYPED_TEST(VirtualMicFeederProtocolTest, DisconnectRejectsAndReconnectResumesFresh)
{
    using Sink = TypeParam;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(1024);
    va::VirtualMicFeeder feeder(endpoint, 64);

    // Phase A (generation 1): 0.125 audio flows and is consumed.
    feeder.attachSink(&sink);
    writeBlocks(transport, 0.125f, 4);
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(feeder.tick(256));
    }
    std::vector<float> out(64, -1.0f);
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(sink.readMono(out.data(), 64, 256));
        EXPECT_NEAR(out[0], 0.125f, 1e-6f);
    }

    // Disconnect: the engine keeps producing; the sink rejects (counted).
    sink.setConnected(false);
    writeBlocks(transport, 0.2f, 4);
    for (int i = 0; i < 4; ++i)
    {
        (void)feeder.tick(256);
    }
    EXPECT_EQ(feeder.snapshot().rejectedBlocks, 4u);
    EXPECT_EQ(sink.snapshot().rejectedDisconnected, 4u);

    // Reconnect = fresh epoch (generation 2): only post-reconnect audio flows.
    sink.setConnected(true);
    feeder.attachSink(&sink);
    EXPECT_EQ(feeder.generation(), 2u);
    writeBlocks(transport, 0.25f, 4);
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(feeder.tick(256));
    }
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(sink.readMono(out.data(), 64, 256));
        EXPECT_LE(maxAbs(out), 0.251f) << "no pre-reconnect (0.125 / 0.20) audio may be served";
        EXPECT_GE(out[0], 0.249f);
    }
    EXPECT_GE(sink.snapshot().generationFlushes, 1u);
}

TYPED_TEST(VirtualMicFeederProtocolTest, GenerationFlushDiscardsOldEpochBacklog)
{
    using Sink = TypeParam;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(512);
    va::VirtualMicFeeder feeder(endpoint, 64);

    // Generation 1: a 0.5 backlog fills the sink and is NEVER read by a client.
    feeder.attachSink(&sink);
    for (int i = 0; i < 20; ++i)
    {
        writeBlocks(transport, 0.5f, 1);
        (void)feeder.tick(256);
    }
    EXPECT_GT(sink.availableFrames(), 0u);

    // Reattach (generation 2): the sink flushes the old-epoch backlog.
    feeder.attachSink(&sink);
    for (int i = 0; i < 4; ++i)
    {
        writeBlocks(transport, 0.1f, 1);
        ASSERT_TRUE(feeder.tick(256));
    }

    std::vector<float> out(64, -1.0f);
    int read = 0;
    while (sink.readMono(out.data(), 64, 256))
    {
        EXPECT_LE(maxAbs(out), 0.101f) << "old-epoch (0.5) audio must never be served after reconnect";
        ++read;
    }
    EXPECT_EQ(read, 4);
    EXPECT_EQ(sink.availableFrames(), 0u);
    EXPECT_GE(sink.snapshot().generationFlushes, 1u);
}

TYPED_TEST(VirtualMicFeederProtocolTest, SequenceCountsAttemptsAndRecordsAccepts)
{
    using Sink = TypeParam;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(128); // 2 blocks
    va::VirtualMicFeeder feeder(endpoint, 64);

    feeder.attachSink(&sink);
    // 3 fresh blocks with a sink that accepts only the first 2.
    writeBlocks(transport, 0.5f, 3);
    for (int i = 0; i < 3; ++i)
    {
        (void)feeder.tick(256);
    }
    EXPECT_EQ(feeder.snapshot().sequence, 3u) << "the per-attach attempt index is monotonic";
    EXPECT_EQ(sink.snapshot().lastAcceptedSequence, 1u) << "only the accepted block records its sequence";
    EXPECT_EQ(sink.snapshot().rejectedStall, 1u);
}

TYPED_TEST(VirtualMicFeederProtocolTest, NoSinkTickIsANoOp)
{
    using Sink = TypeParam;
    (void)sizeof(Sink);
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    va::VirtualMicFeeder feeder(endpoint, 64);

    EXPECT_FALSE(feeder.tick(256));
    const va::VirtualMicFeeder::Snapshot snap = feeder.snapshot();
    EXPECT_EQ(snap.noSinkTicks, 1u);
    EXPECT_EQ(snap.pulls, 0u);
}

TYPED_TEST(VirtualMicFeederProtocolTest, ShutdownIsSafe)
{
    using Sink = TypeParam;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(1024);

    // Stop before start: no-op.
    {
        va::VirtualMicFeeder idle(endpoint, 64);
        idle.stop();
        EXPECT_FALSE(idle.running());
    }

    // Start -> requestStop -> stop joins cleanly; stop is idempotent.
    {
        va::VirtualMicFeeder feeder(endpoint, 64);
        feeder.attachSink(&sink);
        feeder.start(std::chrono::milliseconds(1));
        writeBlocks(transport, 0.125f, 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        feeder.requestStop();
        feeder.stop();
        EXPECT_FALSE(feeder.running());
        feeder.stop(); // idempotent
    }

    // Double start is guarded.
    {
        va::VirtualMicFeeder feeder(endpoint, 64);
        feeder.start(std::chrono::milliseconds(1));
        feeder.start(std::chrono::milliseconds(1));
        feeder.requestStop();
        feeder.stop();
    }

    // Destructor joins a still-running worker.
    {
        va::VirtualMicFeeder feeder(endpoint, 64);
        feeder.attachSink(&sink);
        feeder.start(std::chrono::milliseconds(1));
        writeBlocks(transport, 0.5f, 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        feeder.requestStop();
        // destructor runs stop()
    }
}

// A4: the app must align the feeder epoch with the driver's CONNECT generation so
// the kernel-initialized ring generation is NOT clobbered by the first producer
// write, and FLUSH/reconnect re-alignment must preserve fresh-epoch + no-stale
// semantics. This mirrors the project plan "INTERNAL SEAM" (Q5-A3B) that previously
// let the feeder's own 1,2,... epoch overwrite the kernel's per-CONNECT epoch.
TEST(VirtualMicFeederProtocolTest, ControlPlaneGenerationAlignmentPreservesKernelEpoch)
{
    using Sink = va::SharedRingCaptureSink;
    core::VirtualCaptureTransport transport = makeEndpointTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);
    Sink sink = makeSink<Sink>(1024);
    va::VirtualMicFeeder feeder(endpoint, 64);

    // Phase 0: the DRIVER CONNECTed and initialized a fresh region (kernel
    // epoch 42, CONNECTED set) before the app ever writes. The app adopts the
    // kernel generation on attach.
    const std::uint64_t kConnectGen = 42u;
    sink.region()->generation = kConnectGen;
    sink.region()->flags |= audient::capture_ring::REGION_FLAG_CONNECTED;

    feeder.attachSink(&sink, kConnectGen);
    EXPECT_EQ(feeder.generation(), kConnectGen);

    // Normal streaming: aligned writes must NOT reset the kernel epoch (no
    // spurious generationFlush, no clobber back to the feeder's own counter).
    writeBlocks(transport, 0.125f, 4);
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(feeder.tick(256));
    }
    EXPECT_EQ(sink.region()->generation, kConnectGen) << "aligned writes must preserve the kernel epoch";
    EXPECT_EQ(sink.region()->producedSamples, 4u * 64u);
    EXPECT_EQ(sink.region()->generationFlushes, 0u) << "no spurious epoch reset when app and driver agree";

    std::vector<float> out(64, -1.0f);
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(sink.readMono(out.data(), 64, 256));
        EXPECT_NEAR(out[0], 0.125f, 1e-6f);
    }

    // Phase 1: the driver FLUSHes while connected - it re-initializes the region
    // to a fresh empty epoch at generation 43. The app adopts the new generation
    // via setControlPlaneGeneration (no re-attach needed). Because app and driver
    // agree, the producer does NOT re-flush the driver's fresh epoch.
    const std::uint64_t kFlushGen = 43u;
    ASSERT_TRUE(audient::capture_ring::CaptureRingInit(
        sink.region(),
        audient::capture_ring::CaptureRingRegionBytes(sink.region()->capacityFrames),
        static_cast<audient::capture_ring::U32>(sink.region()->capacityFrames),
        static_cast<audient::capture_ring::U32>(sink.region()->blockFrames)) == 1);
    sink.region()->generation = kFlushGen;
    sink.region()->flags |= audient::capture_ring::REGION_FLAG_CONNECTED;

    feeder.setControlPlaneGeneration(kFlushGen);
    writeBlocks(transport, 0.25f, 2);
    for (int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(feeder.tick(256));
    }
    EXPECT_EQ(sink.region()->generation, kFlushGen) << "FLUSH re-alignment must keep the driver epoch";
    EXPECT_EQ(sink.region()->generationFlushes, 0u) << "producer must not re-flush the driver's fresh epoch";
    EXPECT_EQ(sink.region()->producedSamples, 2u * 64u) << "post-FLUSH audio fills the fresh epoch only";
    for (int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(sink.readMono(out.data(), 64, 256));
        EXPECT_NEAR(out[0], 0.25f, 1e-6f) << "no pre-FLUSH (0.125) audio may be served after the fresh epoch";
    }
    EXPECT_EQ(sink.snapshot().underruns, 0u) << "the fresh epoch is filled, not underrun";
}
