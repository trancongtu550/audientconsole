#include "TestPassthroughPlugin.h"

#include "routing/RoutingCore.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

// Slice O — VST-015: route the real VST3 chain latency through the routing
// graph. Proves:
//   destination bus latency = source latency + route processing latency
// where the route processing latency is Vst3Chain::totalLatencySamples() fed
// through the routing core's LatencyQueryFn adapter (chainLatencyQuery). No
// plug-in-specific latency constant exists in the routing core.

namespace
{

constexpr std::size_t kBlocks = 2;
constexpr std::size_t kFrames = 64;
constexpr std::size_t kMaxBlock = 128;

std::vector<float> makeBlock(std::size_t frames, float value)
{
    std::vector<float> block(frames, value);
    return block;
}

} // namespace

TEST(Vst3RoutingLatencyIntegrationTest, MicUplinkLatencyIsSourcePlusInputChain)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, kMaxBlock, audient::vst3::BusLayout::Mono);
    ASSERT_NE(processor, nullptr);
    audient::vst3_test::TestPassthroughComponent* comp =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(comp, nullptr);

    audient::vst3::Vst3Chain micChain;
    ASSERT_TRUE(micChain.publish({{processor.get(), false}}, false));
    // The plug-in reports a latency (as a real plug-in would from state/params).
    comp->setTestLatency(128u);
    ASSERT_TRUE(processor->refreshLatencySamples());
    EXPECT_EQ(micChain.totalLatencySamples(), 128u);

    audient::routing::RoutingCore routing(kMaxBlock);
    routing.setSourceLatency(audient::routing::BusId::PhysicalInput1, 32u);
    routing.setInputChain(&audient::vst3::Vst3Chain::processMono, &micChain);
    routing.setInputChainLatency(&audient::vst3::Vst3Chain::chainLatencyQuery, &micChain);

    const audient::routing::LatencySnapshot model = routing.latencySnapshot();
    EXPECT_EQ(model.physicalInput, 32u);
    EXPECT_EQ(model.micRaw, 32u);
    EXPECT_EQ(model.micProcessed, 32u + 128u) << "mic processed = source + chain total";
    EXPECT_EQ(model.micUplink, 32u + 128u);

    // The wire path still routes audio (chain latency is a control-plane model).
    std::vector<float> input = makeBlock(kFrames, 0.25f);
    std::vector<float> uplink = makeBlock(kFrames, 0.0f);
    std::vector<float> raw = makeBlock(kFrames, 0.0f);
    std::vector<float> processed = makeBlock(kFrames, 0.0f);
    audient::routing::BlockBinding binding;
    binding.frames = kFrames;
    binding.inputChannels = 1; // one runtime slot bound (mono plan)
    binding.micUplinkChannel = 0;
    binding.inputSource[0] = input.data();
    binding.inputRawTap[0] = raw.data();
    binding.inputProcessed[0] = processed.data();
    binding.micUplink = uplink.data();
    routing.process(binding);
    EXPECT_NEAR(uplink[0], 0.25f * 0.5f, 1e-6f);
}

TEST(Vst3RoutingLatencyIntegrationTest, BypassRemovesRouteLatency)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    auto processor = host.createEffectProcessor(classes[0], 48000.0, kMaxBlock, audient::vst3::BusLayout::Stereo);
    ASSERT_NE(processor, nullptr);
    audient::vst3_test::TestPassthroughComponent* comp =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(comp, nullptr);
    comp->setTestLatency(64u);
    ASSERT_TRUE(processor->refreshLatencySamples());

    audient::vst3::Vst3Chain outChain;
    ASSERT_TRUE(outChain.publish({{processor.get(), false}}, false));

    audient::routing::RoutingCore routing(kMaxBlock);
    routing.setSourceLatency(audient::routing::BusId::PlaybackBus, 16u);
    routing.setOutputChain(&audient::vst3::Vst3Chain::processStereo, &outChain);
    routing.setOutputChainLatency(&audient::vst3::Vst3Chain::chainLatencyQuery, &outChain);
    EXPECT_EQ(routing.latencySnapshot().outputProcessed, 16u + 64u);

    // Bypass the whole output chain: the route processing latency is removed,
    // so the physical output latency falls back to the source only.
    ASSERT_TRUE(outChain.publish({{processor.get(), false}}, true));
    EXPECT_EQ(routing.latencySnapshot().outputProcessed, 16u) << "whole-chain bypass removes route latency";
    EXPECT_EQ(routing.latencySnapshot().physicalOutput, 16u);
}

TEST(Vst3RoutingLatencyIntegrationTest, LatencyChangePropagatesThroughLiveQuery)
{
    // A real plug-in latency change (VST-016 path) must be reflected in the
    // routing model once the chain snapshot is republished (safe control-thread
    // reconfiguration) — the query is live, not cached.
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    auto processor = host.createEffectProcessor(classes[0], 48000.0, kMaxBlock, audient::vst3::BusLayout::Mono);
    ASSERT_NE(processor, nullptr);
    audient::vst3_test::TestPassthroughComponent* comp =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(comp, nullptr);

    audient::vst3::Vst3Chain chain;
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    audient::routing::RoutingCore routing(kMaxBlock);
    routing.setSourceLatency(audient::routing::BusId::PhysicalInput1, 20u);
    routing.setInputChain(&audient::vst3::Vst3Chain::processMono, &chain);
    routing.setInputChainLatency(&audient::vst3::Vst3Chain::chainLatencyQuery, &chain);

    comp->setTestLatency(40u);
    ASSERT_TRUE(processor->refreshLatencySamples());
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    EXPECT_EQ(routing.latencySnapshot().micProcessed, 20u + 40u);

    // Latency grew: republish the chain; routing model follows without any
    // routing-side reconfiguration.
    comp->setTestLatency(512u);
    ASSERT_TRUE(processor->refreshLatencySamples());
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    EXPECT_EQ(routing.latencySnapshot().micProcessed, 20u + 512u);
}