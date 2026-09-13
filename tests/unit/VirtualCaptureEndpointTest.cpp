#include "transport/TransportFormat.h"
#include "transport/VirtualCaptureTransport.h"
#include "virtual_audio/endpoint/SoftwareCaptureEndpoint.h"
#include "virtual_audio/endpoint/VirtualCaptureEndpoint.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <vector>

namespace core = audient::transport;
namespace va = audient::virtual_audio;

namespace
{

core::VirtualCaptureTransport makeTransport(std::size_t capacity = 4096, std::size_t block = 64)
{
    return core::VirtualCaptureTransport(
        {48000, 1, static_cast<int>(block), core::SampleType::Float32}, capacity);
}

float maxAbs(const std::vector<float>& data)
{
    float peak = 0.0f;
    for (const float sample : data)
    {
        const float magnitude = sample >= 0.0f ? sample : -sample;
        peak = magnitude > peak ? magnitude : peak;
    }
    return peak;
}

} // namespace

TEST(VirtualCaptureEndpointTest, DeliversFreshProcessedMic)
{
    core::VirtualCaptureTransport transport = makeTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);

    std::vector<float> block(64, 0.125f);
    ASSERT_TRUE(transport.writeProcessedMic(block.data(), 64));

    std::vector<float> out(64, -1.0f);
    EXPECT_EQ(endpoint.captureMono(out.data(), 64, 256), 64u);
    EXPECT_NEAR(out[0], 0.125f, 1e-6f) << "the endpoint must deliver the processed mic";
    EXPECT_LE(maxAbs(out), 0.126f);

    const va::EndpointCounters counters = endpoint.counters();
    EXPECT_EQ(counters.freshServedFrames, 64u);
    EXPECT_EQ(counters.silenceServedFrames, 0u);
    EXPECT_EQ(counters.underruns, 0u);
}

TEST(VirtualCaptureEndpointTest, ServesDigitalSilenceWhenProducerAbsent)
{
    core::VirtualCaptureTransport transport = makeTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);

    std::vector<float> out(64, -1.0f);
    EXPECT_EQ(endpoint.captureMono(out.data(), 64, 256), 64u)
        << "the endpoint always fills the full block (never blocks, never hangs)";
    EXPECT_EQ(maxAbs(out), 0.0f) << "absent producer => exact digital silence";
    EXPECT_GE(endpoint.counters().silenceServedFrames, 64u);
    EXPECT_EQ(endpoint.counters().freshServedFrames, 0u);
}

TEST(VirtualCaptureEndpointTest, FreshnessResyncNeverServesStaleBacklog)
{
    core::VirtualCaptureTransport transport = makeTransport(1024);
    va::SoftwareCaptureEndpoint endpoint(transport);
    constexpr std::size_t kThreshold = 128;

    // Consumer stalls: producer keeps writing 1.0 "stall-era" audio.
    std::vector<float> stale(64, 1.0f);
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(transport.writeProcessedMic(stale.data(), 64));
    }

    // First pull after recovery: the stale backlog is discarded; the endpoint
    // serves silence (nothing stale), counted.
    std::vector<float> out(64, -1.0f);
    EXPECT_EQ(endpoint.captureMono(out.data(), 64, kThreshold), 64u);
    EXPECT_EQ(maxAbs(out), 0.0f) << "no stall-era audio may reach the endpoint on recovery";
    EXPECT_EQ(endpoint.counters().staleCatchupDrops, 512u);

    // Post-recovery fresh audio is served normally.
    std::vector<float> fresh(64, 0.25f);
    ASSERT_TRUE(transport.writeProcessedMic(fresh.data(), 64));
    EXPECT_EQ(endpoint.captureMono(out.data(), 64, kThreshold), 64u);
    EXPECT_NEAR(out[0], 0.25f, 1e-6f) << "only post-recovery audio is served";
    EXPECT_EQ(endpoint.counters().freshServedFrames, 64u);
}

TEST(VirtualCaptureEndpointTest, ResetYieldsFreshEpoch)
{
    core::VirtualCaptureTransport transport = makeTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);

    std::vector<float> block(64, 0.5f);
    ASSERT_TRUE(transport.writeProcessedMic(block.data(), 64));

    endpoint.reset();
    EXPECT_FALSE(endpoint.producerActive());
    const va::EndpointCounters counters = endpoint.counters();
    EXPECT_EQ(counters.freshServedFrames, 0u);
    EXPECT_EQ(counters.silenceServedFrames, 0u);
    EXPECT_EQ(counters.overflowDrops, 0u);
    EXPECT_EQ(counters.staleCatchupDrops, 0u);
    EXPECT_EQ(counters.underruns, 0u);

    std::vector<float> out(64, -1.0f);
    EXPECT_EQ(endpoint.captureMono(out.data(), 64, 256), 64u);
    EXPECT_EQ(maxAbs(out), 0.0f) << "reset must never serve pre-reset data";
}

TEST(VirtualCaptureEndpointTest, IdentityAndProducerActivity)
{
    core::VirtualCaptureTransport transport = makeTransport();
    va::SoftwareCaptureEndpoint endpoint(transport);

    EXPECT_STREQ(endpoint.friendlyName(), va::kVirtualMicFriendlyName);
    EXPECT_STREQ(endpoint.friendlyName(), "Microphone (Audient Console)") << "AGENTS §4 canonical name";
    EXPECT_EQ(endpoint.format().sampleRateHz, 48000);
    EXPECT_EQ(endpoint.format().channels, 1);
    EXPECT_FALSE(endpoint.producerActive());

    std::vector<float> block(64, 0.25f);
    ASSERT_TRUE(transport.writeProcessedMic(block.data(), 64));
    EXPECT_TRUE(endpoint.producerActive()) << "publication makes the producer active";
}