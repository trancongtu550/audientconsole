#include "pico/PicoRenderDevice.h"
#include "pico/PicoUsbWorker.h"
#include "pico/PicoVirtualMicSink.h"

#include "transport/TransportFormat.h"
#include "transport/VirtualCaptureTransport.h"
#include "virtual_audio/driver-protocol/FanoutCaptureSink.h"
#include "virtual_audio/driver-protocol/MockDriverCaptureSink.h"
#include "virtual_audio/driver-protocol/VirtualMicFeeder.h"
#include "virtual_audio/endpoint/SoftwareCaptureEndpoint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pico = audient::pico;
namespace va = audient::virtual_audio;
namespace core = audient::transport;

namespace
{

core::Format monoFormat()
{
    return {48000, 1, 64, core::SampleType::Float32};
}

// A hardware-free IPicoRenderDevice. `available` models the WASAPI free frame
// count handed to the worker; every submit() records the exact interleaved
// stereo the worker produced so tests can assert L==R reconstruction and that
// no block-size assumption exists.
class FakePicoRenderDevice final : public pico::IPicoRenderDevice
{
public:
    explicit FakePicoRenderDevice(unsigned available)
        : m_available(available)
    {
    }

    pico::PicoDeviceOpenResult open() override
    {
        if (!openOk)
        {
            return {false, std::string(), "fake: endpoint absent"};
        }
        m_isOpen = true;
        m_needsReopen = false;
        return {true, m_name, std::string()};
    }

    void close() override { m_isOpen = false; }
    bool isOpen() const override { return m_isOpen; }
    bool needsReopen() const override { return m_needsReopen; }
    bool waitForRenderReady(unsigned) override { return m_isOpen && !m_needsReopen; }
    void wake() override {}
    unsigned availableRenderFrames() override { return m_isOpen ? m_available : 0u; }

    bool submit(const float* stereo, std::size_t frames, bool* deviceLost) override
    {
        if (deviceLost != nullptr)
        {
            *deviceLost = false;
        }
        if (!m_isOpen)
        {
            return false;
        }
        if (submitLost)
        {
            if (deviceLost != nullptr)
            {
                *deviceLost = true;
            }
            return false;
        }
        captures.emplace_back(stereo, stereo + frames * 2u);
        ++submitCalls;
        return true;
    }

    unsigned sampleRate() const override { return 48000; }
    unsigned channels() const override { return 2; }
    const std::string& deviceName() const override { return m_name; }
    void threadShutdown() override { shutdownCalled = true; }

    bool openOk = true;
    bool submitLost = false;
    bool shutdownCalled = false;
    std::uint64_t submitCalls = 0;
    unsigned m_available = 64;

private:
    bool m_isOpen = false;
    bool m_needsReopen = false;
    std::string m_name = "Speakers (fake Audient Console Bridge)";
public:
    std::vector<std::vector<float>> captures;
};

pico::PicoWorkerConfig quietConfig()
{
    pico::PicoWorkerConfig config;
    config.log = false;
    return config;
}

} // namespace

TEST(PicoVirtualMicTransportTest, RendersExactDigitalSilenceWhenSinkEmpty)
{
    pico::PicoVirtualMicSink sink(monoFormat(), 1024);
    auto fake = std::make_unique<FakePicoRenderDevice>(64);
    FakePicoRenderDevice* raw = fake.get();
    pico::PicoUsbWorker worker(sink, std::move(fake), quietConfig());

    EXPECT_TRUE(worker.stepOnce());
    const pico::PicoUsbWorker::Status status = worker.status();
    EXPECT_TRUE(status.deviceOpen);
    ASSERT_EQ(raw->captures.size(), 1u);
    EXPECT_EQ(raw->captures[0].size(), 128u) << "64 stereo frames";
    for (const float sample : raw->captures[0])
    {
        EXPECT_EQ(sample, 0.0f) << "underrun must be exact digital silence";
    }
    EXPECT_EQ(status.renderedFrames, 64u);
    EXPECT_EQ(status.silenceFrames, 64u);
    EXPECT_EQ(status.underruns, 1u);
}

TEST(PicoVirtualMicTransportTest, InterleavesProcessedMonoToBothChannels)
{
    pico::PicoVirtualMicSink sink(monoFormat(), 1024);
    auto fake = std::make_unique<FakePicoRenderDevice>(64);
    FakePicoRenderDevice* raw = fake.get();
    pico::PicoUsbWorker worker(sink, std::move(fake), quietConfig());

    std::vector<float> mono(64);
    for (std::size_t i = 0; i < mono.size(); ++i)
    {
        mono[i] = static_cast<float>(i) / 128.0f;
    }
    ASSERT_TRUE(sink.writeMono(mono.data(), mono.size(), 1u, 0u));

    ASSERT_TRUE(worker.stepOnce());
    ASSERT_EQ(raw->captures.size(), 1u);
    ASSERT_EQ(raw->captures[0].size(), 128u);
    for (std::size_t i = 0; i < mono.size(); ++i)
    {
        // The firmware computes (left>>1)+(right>>1): L==R reconstructs mono.
        EXPECT_FLOAT_EQ(raw->captures[0][i * 2u], mono[i]);
        EXPECT_FLOAT_EQ(raw->captures[0][i * 2u + 1u], mono[i]);
    }
}

TEST(PicoVirtualMicTransportTest, BlockAdaptationIsIndependentOfAsioBufferSize)
{
    for (unsigned blockFrames : {32u, 64u, 128u, 256u, 512u, 1024u})
    {
        pico::PicoVirtualMicSink sink(monoFormat(), 8192);
        auto fake = std::make_unique<FakePicoRenderDevice>(blockFrames);
        FakePicoRenderDevice* raw = fake.get();
        pico::PicoUsbWorker worker(sink, std::move(fake), quietConfig());

        const float value = 0.5f;
        std::vector<float> mono(blockFrames, value);
        ASSERT_TRUE(sink.writeMono(mono.data(), mono.size(), 1u, 0u))
            << "block=" << blockFrames;

        ASSERT_TRUE(worker.stepOnce()) << "block=" << blockFrames;
        ASSERT_EQ(raw->captures.size(), 1u) << "block=" << blockFrames;
        const std::vector<float>& out = raw->captures[0];
        ASSERT_EQ(out.size(), static_cast<std::size_t>(blockFrames) * 2u);
        for (float sample : out)
        {
            EXPECT_FLOAT_EQ(sample, value) << "block=" << blockFrames;
        }
    }
}

TEST(PicoVirtualMicTransportTest, BackpressureDropsNewAndNeverBlocks)
{
    pico::PicoVirtualMicSink sink(monoFormat(), 128); // exactly two 64-frame blocks
    auto fake = std::make_unique<FakePicoRenderDevice>(64);
    pico::PicoUsbWorker worker(sink, std::move(fake), quietConfig());

    std::vector<float> block(64, 0.25f);
    int accepted = 0;
    for (int i = 0; i < 10; ++i)
    {
        if (sink.writeMono(block.data(), block.size(), 1u, static_cast<std::uint64_t>(i)))
        {
            ++accepted;
        }
    }
    EXPECT_EQ(accepted, 2) << "the bounded ring accepts exactly its capacity";
    EXPECT_EQ(sink.availableFrames(), 128u);
    EXPECT_EQ(sink.snapshot().rejectedStall, 8u) << "every refused write is counted";

    // The worker still renders without blocking; the engine side was untouched.
    EXPECT_TRUE(worker.stepOnce());
}

TEST(PicoVirtualMicTransportTest, AbsentDeviceKeepsWorkerAliveWithoutAudio)
{
    pico::PicoVirtualMicSink sink(monoFormat(), 1024);
    auto fake = std::make_unique<FakePicoRenderDevice>(64);
    FakePicoRenderDevice* raw = fake.get();
    raw->openOk = false;
    pico::PicoUsbWorker worker(sink, std::move(fake), quietConfig());

    EXPECT_FALSE(worker.stepOnce());
    const pico::PicoUsbWorker::Status status = worker.status();
    EXPECT_FALSE(status.deviceOpen);
    EXPECT_EQ(status.failedOpens, 1u);
    EXPECT_EQ(status.opens, 0u);
    EXPECT_EQ(raw->captures.size(), 0u);
    EXPECT_FALSE(sink.connected()) << "no endpoint => transport marks disconnected";

    // Still retries (does not terminate) and never touches the engine.
    EXPECT_FALSE(worker.stepOnce());
    EXPECT_EQ(worker.status().failedOpens, 2u);
}

TEST(PicoVirtualMicTransportTest, DeviceLossThenReconnectFlushesStaleAndResumes)
{
    pico::PicoVirtualMicSink sink(monoFormat(), 1024);
    auto fake = std::make_unique<FakePicoRenderDevice>(64);
    FakePicoRenderDevice* raw = fake.get();
    pico::PicoUsbWorker worker(sink, std::move(fake), quietConfig());

    ASSERT_TRUE(worker.stepOnce()); // open
    std::vector<float> a(64, 0.25f);
    ASSERT_TRUE(sink.writeMono(a.data(), a.size(), 1u, 0u));
    ASSERT_TRUE(worker.stepOnce());
    ASSERT_EQ(raw->captures.size(), 2u);
    EXPECT_FLOAT_EQ(raw->captures.back()[0], 0.25f);

    // Endpoint disappears mid-render.
    raw->submitLost = true;
    EXPECT_FALSE(worker.stepOnce());
    EXPECT_EQ(worker.status().deviceLost, 1u);
    EXPECT_FALSE(sink.connected());

    // Endpoint returns: worker re-discovers, flushes the stale epoch and resumes.
    raw->submitLost = false;
    ASSERT_TRUE(worker.stepOnce());
    EXPECT_TRUE(worker.status().deviceOpen);
    EXPECT_EQ(worker.status().opens, 2u);
    EXPECT_EQ(worker.status().reconnects, 1u);

    std::vector<float> b(64, 0.75f);
    ASSERT_TRUE(sink.writeMono(b.data(), b.size(), 1u, 1u));
    ASSERT_TRUE(worker.stepOnce());
    const std::vector<float>& last = raw->captures.back();
    for (float sample : last)
    {
        EXPECT_FLOAT_EQ(sample, 0.75f) << "no pre-reconnect (0.25) PCM may be replayed";
    }
}

TEST(PicoVirtualMicTransportTest, ShutdownOwnershipJoinsWorkerAndShutsDownDevice)
{
    pico::PicoVirtualMicSink sink(monoFormat(), 1024);
    auto fake = std::make_unique<FakePicoRenderDevice>(64);
    FakePicoRenderDevice* raw = fake.get();
    pico::PicoUsbWorker worker(sink, std::move(fake), quietConfig());

    worker.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(worker.running());
    worker.stop();
    EXPECT_FALSE(worker.running());
    EXPECT_TRUE(raw->shutdownCalled) << "worker thread balances device COM on exit";
    worker.stop(); // idempotent
}

TEST(PicoVirtualMicTransportTest, FeederToFanoutToPicoSeamCarriesOnlyProcessedMono)
{
    core::VirtualCaptureTransport transport(monoFormat(), 4096);
    va::SoftwareCaptureEndpoint endpoint(transport);
    va::VirtualMicFeeder feeder(endpoint, 64);

    va::MockDriverCaptureSink driverSink(monoFormat(), 1024);
    pico::PicoVirtualMicSink picoSink(monoFormat(), 1024);
    va::FanoutCaptureSink fanout;
    fanout.add(&driverSink);
    fanout.add(&picoSink);
    feeder.attachSink(&fanout, 0u);

    // CH0 processed mono published by the engine.
    std::vector<float> block(64, 0.3f);
    ASSERT_TRUE(transport.writeProcessedMic(block.data(), block.size()));
    ASSERT_TRUE(feeder.tick(256));

    auto fake = std::make_unique<FakePicoRenderDevice>(64);
    FakePicoRenderDevice* raw = fake.get();
    pico::PicoUsbWorker worker(picoSink, std::move(fake), quietConfig());
    ASSERT_TRUE(worker.stepOnce());

    ASSERT_EQ(raw->captures.size(), 1u);
    for (float sample : raw->captures[0])
    {
        EXPECT_FLOAT_EQ(sample, 0.3f);
    }
    // The driver path received the same block independently.
    std::vector<float> driverOut(64, -1.0f);
    ASSERT_TRUE(driverSink.readMono(driverOut.data(), 64, 256));
    EXPECT_FLOAT_EQ(driverOut[0], 0.3f);
}

TEST(PicoVirtualMicTransportTest, FanoutAcceptsWhenAnySinkAcceptsAndIsolatesSinks)
{
    va::MockDriverCaptureSink driverSink(monoFormat(), 1024);
    pico::PicoVirtualMicSink picoSink(monoFormat(), 1024);
    va::FanoutCaptureSink fanout;
    fanout.add(&driverSink);
    fanout.add(&picoSink);

    std::vector<float> block(64, 0.5f);

    // Pico absent: the driver path must still accept; the Pico rejects (counted).
    picoSink.setConnected(false);
    EXPECT_TRUE(fanout.writeMono(block.data(), block.size(), 1u, 0u));
    EXPECT_EQ(driverSink.snapshot().acceptedWrites, 1u);
    EXPECT_EQ(picoSink.snapshot().rejectedDisconnected, 1u);
    EXPECT_TRUE(fanout.connected());

    // Driver absent: the Pico path still accepts (isolation is symmetric).
    driverSink.setConnected(false);
    picoSink.setConnected(true);
    EXPECT_TRUE(fanout.writeMono(block.data(), block.size(), 1u, 1u));
    EXPECT_EQ(picoSink.snapshot().acceptedWrites, 1u);

    // Both absent: the block is rejected and counted, never blocked.
    picoSink.setConnected(false);
    EXPECT_FALSE(fanout.writeMono(block.data(), block.size(), 1u, 2u));
    EXPECT_FALSE(fanout.connected());
    EXPECT_EQ(fanout.snapshot().allRejected, 1u);
}
