#include "TestPassthroughPlugin.h"

#include "routing/RoutingCore.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

// VST-006 — stereo-only effects on the mono mic chain (documented mono<->stereo
// adapter). Mirrors adding FabFilter Pro-Q 4 (a stereo-only effect) to the mic
// chain: the host negotiates stereo when mono is rejected, and the chain drives
// the stereo slot from a mono source by duplicating L=R before processing and
// downmixing after. The reverse (a mono processor on a stereo output chain) is
// covered symmetrically.

namespace
{

constexpr std::size_t kMaxBlock = 256;
constexpr std::size_t kFrames = 64;

struct ChainBundle
{
    audient::vst3_test::TestPassthroughFactory factory;
    audient::vst3::Vst3Host host;
    std::unique_ptr<audient::vst3::Vst3Processor> processor;
    audient::vst3::Vst3Chain chain;

    ChainBundle(float gain, audient::vst3::BusLayout request, bool stereoOnly, bool wholeBypass = false)
        : factory(gain, false, stereoOnly)
    {
        host.attachFactory(&factory);
        processor = host.createEffectProcessor(host.classes()[0], 48000.0, static_cast<long>(kMaxBlock), request);
        chain.publish({{processor.get(), false}}, wholeBypass);
    }
};

std::vector<float> makeBlock(std::size_t frames, float value)
{
    return std::vector<float>(frames, value);
}

} // namespace

TEST(Vst3StereoAdapter, StereoOnlyProcessorNegotiatesStereoAndAdaptsMonoDrive)
{
    // Stereo-only effect (Pro-Q-like) requested as the mono mic chain: prepare
    // must fall back to stereo, and the chain's mono drive must process it.
    audient::vst3_test::TestPassthroughFactory factory(0.5f, false, true /* stereoOnly */);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    auto processor = host.createEffectProcessor(host.classes()[0], 48000.0, static_cast<long>(kMaxBlock),
                                                audient::vst3::BusLayout::Mono);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid()) << host.lastError();
    EXPECT_EQ(processor->layout(), audient::vst3::BusLayout::Stereo)
        << "a stereo-only effect must negotiate stereo even when mono is requested";

    audient::vst3::Vst3Chain chain;
    chain.configure(kMaxBlock);
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));

    std::vector<float> input = makeBlock(kFrames, 0.25f);
    std::vector<float> output = makeBlock(kFrames, 0.0f);
    chain.processMono(input.data(), output.data(), kFrames, &chain);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(output[i], 0.25f * 0.5f, 1e-5f)
            << "mono drive of a stereo slot must dup->process->downmix (0.125 at gain 0.5)";
    }
}

TEST(Vst3StereoAdapter, StereoOnlyEffectOnMicPathRoutesThroughRoutingCore)
{
    ChainBundle bundle(0.5f, audient::vst3::BusLayout::Mono, true /* stereoOnly */);
    ASSERT_NE(bundle.processor, nullptr);
    ASSERT_TRUE(bundle.processor->valid()) << bundle.host.lastError();
    EXPECT_EQ(bundle.processor->layout(), audient::vst3::BusLayout::Stereo);
    bundle.chain.configure(kMaxBlock);
    ASSERT_TRUE(bundle.chain.publish({{bundle.processor.get(), false}}, false));

    audient::routing::RoutingCore routing(kMaxBlock);
    routing.setInputChain(&audient::vst3::Vst3Chain::processMono, &bundle.chain);

    std::vector<float> input = makeBlock(kFrames, 0.25f);
    std::vector<float> micRaw = makeBlock(kFrames, 0.0f);
    std::vector<float> processed = makeBlock(kFrames, 0.0f);
    std::vector<float> uplink = makeBlock(kFrames, 0.0f);
    std::vector<float> monitor = makeBlock(kFrames, 0.0f);
    audient::routing::BlockBinding binding;
    binding.frames = kFrames;
    binding.inputChannels = 1; // one runtime slot bound (mono mic path)
    binding.micUplinkChannel = 0;
    binding.inputSource[0] = input.data();
    binding.inputRawTap[0] = micRaw.data();
    binding.inputProcessed[0] = processed.data();
    binding.inputMonitorFeed[0] = monitor.data();
    binding.micUplink = uplink.data();
    routing.process(binding);

    EXPECT_NEAR(processed[0], 0.25f * 0.5f, 1e-5f) << "micProcessed must carry the adapted stereo-only effect";
    EXPECT_NEAR(uplink[0], 0.25f * 0.5f, 1e-5f) << "mic uplink carries the processed (downmixed) mono result";
    EXPECT_NEAR(monitor[0], 0.25f * 0.5f, 1e-5f);
}

TEST(Vst3StereoAdapter, MonoProcessorInStereoOutputChainDownmixDuplicate)
{
    // Reverse adapter: a mono processor on a stereo drive (output chain) is
    // downmixed to mono, processed, then duplicated back to L/R.
    ChainBundle bundle(2.0f, audient::vst3::BusLayout::Mono, false /* not stereo-only -> mono accepted */);
    ASSERT_NE(bundle.processor, nullptr);
    ASSERT_TRUE(bundle.processor->valid());
    EXPECT_EQ(bundle.processor->layout(), audient::vst3::BusLayout::Mono);
    bundle.chain.configure(kMaxBlock);
    ASSERT_TRUE(bundle.chain.publish({{bundle.processor.get(), false}}, false));

    std::vector<float> left = makeBlock(kFrames, 0.4f);
    std::vector<float> right = makeBlock(kFrames, -0.6f);
    std::vector<float> leftOut = makeBlock(kFrames, 0.0f);
    std::vector<float> rightOut = makeBlock(kFrames, 0.0f);
    bundle.chain.processStereo(left.data(), right.data(), leftOut.data(), rightOut.data(), kFrames, &bundle.chain);

    const float expected = 2.0f * 0.5f * (0.4f - 0.6f); // mono gain 2 * mid 0.5 * (L+R=-0.2)
    EXPECT_NEAR(expected, -0.2f, 1e-6f);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(leftOut[i], expected, 1e-5f);
        EXPECT_NEAR(rightOut[i], expected, 1e-5f);
    }
}

TEST(Vst3StereoAdapter, MixedChainHostsMonoAndStereoSlotsIndependently)
{
    // A single mic chain holding a mono slot THEN a stereo-only slot: each slot
    // must run with its own layout variant and pass the intermediate mono
    // result along (0.25 -> x0.5 -> 0.125 -> x2 -> 0.25).
    audient::vst3_test::TestPassthroughFactory monoFactory(0.5f);
    audient::vst3::Vst3Host monoHost;
    ASSERT_TRUE(monoHost.attachFactory(&monoFactory));
    auto monoProc = monoHost.createEffectProcessor(monoHost.classes()[0], 48000.0, static_cast<long>(kMaxBlock),
                                                   audient::vst3::BusLayout::Mono);
    ASSERT_TRUE(monoProc != nullptr && monoProc->valid());

    audient::vst3_test::TestPassthroughFactory stereoFactory(2.0f, false, true /* stereoOnly */);
    audient::vst3::Vst3Host stereoHost;
    ASSERT_TRUE(stereoHost.attachFactory(&stereoFactory));
    auto stereoProc = stereoHost.createEffectProcessor(stereoHost.classes()[0], 48000.0, static_cast<long>(kMaxBlock),
                                                       audient::vst3::BusLayout::Mono);
    ASSERT_TRUE(stereoProc != nullptr && stereoProc->valid());
    EXPECT_EQ(stereoProc->layout(), audient::vst3::BusLayout::Stereo);

    audient::vst3::Vst3Chain chain;
    chain.configure(kMaxBlock);
    ASSERT_TRUE(chain.publish({{monoProc.get(), false},
                               {stereoProc.get(), false}},
                              false));

    std::vector<float> input = makeBlock(kFrames, 0.25f);
    std::vector<float> output = makeBlock(kFrames, 0.0f);
    chain.processMono(input.data(), output.data(), kFrames, &chain);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(output[i], 0.25f * 0.5f * 2.0f, 1e-4f)
            << "mixed mono + stereo-only slots must chain the processed mono stream";
    }
}

TEST(Vst3StereoAdapter, UnconfiguredChainPassesActiveStereoSlotThrough)
{
    // No configure() (no adapter staging): an active stereo slot on a mono drive
    // must pass the block through untouched (the chain materializes the source
    // into the output and skips the unadaptable slot) — never garbage.
    audient::vst3_test::TestPassthroughFactory factory(0.5f, false, true /* stereoOnly */);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    auto processor = host.createEffectProcessor(host.classes()[0], 48000.0, static_cast<long>(kMaxBlock),
                                                audient::vst3::BusLayout::Stereo);
    ASSERT_TRUE(processor != nullptr && processor->valid());

    audient::vst3::Vst3Chain chain;
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));

    std::vector<float> input = makeBlock(kFrames, 0.25f);
    std::vector<float> output(kFrames, -9.0f);
    chain.processMono(input.data(), output.data(), kFrames, &chain);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
        EXPECT_NEAR(output[i], 0.25f, 1e-6f)
            << "an unconfigured chain must be a true passthrough, never garbage";
    }
}