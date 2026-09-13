#include "SimulatedAsioDriver.h"
#include "TestPassthroughPlugin.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioStreamBridge.h"
#include "engine/EngineGraph.h"
#include "engine/GraphConfig.h"
#include "transport/TransportLinks.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

// Slice L: multi-plugin VST3 processing chain (project guidelines §9).
//
// Proves the three multi-instance / multi-chain test IDs on the host-side test
// plug-in:
//  - VST-022: two identical plug-in instances retain independent state.
//  - VST-024: mic and output chains run independent instances of the same
//    plug-in without shared state (live full-duplex path).
//  - VST-025: bypassing or rebuilding one chain does not interrupt or mutate
//    the other chain.

namespace
{

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

audient::asio::ChannelPlan planFromCapabilities(const audient::asio::DriverCapabilities& caps)
{
    std::vector<audient::asio::ChannelInfo> inputs;
    std::vector<audient::asio::ChannelInfo> outputs;
    for (const audient::asio::ChannelInfo& channel : caps.channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }
    return audient::asio::AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
}

std::vector<std::uint8_t> gainBlob(float gain)
{
    // A state blob that encodes `gain` as the processing gain. The blob comes
    // from the SAME plugin class (every TestPassthroughFactory shares the same
    // class cid), so restoring it into another instance of that class mutates
    // only that instance.
    audient::vst3_test::TestPassthroughFactory factory(gain);
    audient::vst3::Vst3Host host;
    EXPECT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    EXPECT_EQ(classes.size(), 1u);
    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    EXPECT_NE(processor, nullptr);
    std::vector<std::uint8_t> blob;
    EXPECT_TRUE(processor->saveState(blob));
    return blob;
}

float processFirstSample(audient::vst3::Vst3Processor& processor, float inputValue)
{
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input.fill(inputValue);
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), &processor);
    return output[0];
}

} // namespace

TEST(Vst3MultiPluginTest, TwoInstancesOfSamePluginRetainIndependentState) // VST-022
{
    constexpr float kDefaultGain = 0.5f;
    audient::vst3_test::TestPassthroughFactory factory(kDefaultGain);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto instanceA = host.createEffectProcessor(classes[0], 48000.0, 64);
    auto instanceB = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(instanceA, nullptr);
    ASSERT_NE(instanceB, nullptr);
    EXPECT_NE(instanceA->component(), instanceB->component()) << "two instances must be distinct objects";

    audient::vst3_test::TestPassthroughComponent* compA =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(instanceA->component());
    audient::vst3_test::TestPassthroughComponent* compB =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(instanceB->component());
    ASSERT_NE(compA, nullptr);
    ASSERT_NE(compB, nullptr);

    // Give each instance a distinct persisted state (different state value,
    // sequence, and processing gain via a sibling blob).
    ASSERT_TRUE(instanceA->restoreState(gainBlob(0.25f)));
    ASSERT_TRUE(instanceB->restoreState(gainBlob(0.9f)));
    compA->setTestState(0.75f, 10u);
    compB->setTestState(0.25f, 20u);

    std::vector<std::uint8_t> blobA;
    std::vector<std::uint8_t> blobB;
    ASSERT_TRUE(instanceA->saveState(blobA));
    ASSERT_TRUE(instanceB->saveState(blobB));
    EXPECT_NE(blobA, blobB) << "distinct instance state must persist as distinct blobs";

    // Processing is independent: same input, each instance applies its own gain.
    EXPECT_NEAR(processFirstSample(*instanceA, 0.5f), 0.5f * 0.25f, 1e-6f);
    EXPECT_NEAR(processFirstSample(*instanceB, 0.5f), 0.5f * 0.9f, 1e-6f);

    // Restoring A's blob into a fresh instance reproduces A exactly, and B
    // (with its own state) is untouched.
    auto instanceC = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(instanceC, nullptr);
    audient::vst3_test::TestPassthroughComponent* compC =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(instanceC->component());
    ASSERT_NE(compC, nullptr);
    EXPECT_EQ(compC->testStateValue(), 0.0f) << "fresh instance starts with default state";
    ASSERT_TRUE(instanceC->restoreState(blobA));
    EXPECT_EQ(compC->testStateValue(), 0.75f);
    EXPECT_EQ(compC->testStateSeq(), 10u);
    EXPECT_NEAR(processFirstSample(*instanceC, 0.5f), 0.5f * 0.25f, 1e-6f) << "gain came from A's blob";

    // Mutating A (persisted state) leaves B's saved state bit-for-bit unchanged.
    compA->setTestState(0.1f, 1u);
    std::vector<std::uint8_t> mutatedA;
    ASSERT_TRUE(instanceA->saveState(mutatedA));
    EXPECT_NE(mutatedA, blobA);
    std::vector<std::uint8_t> blobBAfter;
    ASSERT_TRUE(instanceB->saveState(blobBAfter));
    EXPECT_EQ(blobBAfter, blobB) << "mutating one instance must not reach the other";
}

TEST(Vst3MultiPluginTest, SamePluginRunsMicAndOutputChainsWithoutSharedState) // VST-024
{
    // Live full-duplex: the SAME plugin class provides BOTH the mono mic chain
    // and the stereo output chain as two independent instances, each with its
    // own processing gain, wired concurrently into one EngineGraph.
    audient::asio::SimulatedAsioProvider provider;
    audient::asio::AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const audient::asio::ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    constexpr float kMicGain = 0.25f;
    constexpr float kOutputGain = 0.9f;
    audient::vst3_test::TestPassthroughFactory factory(1.0f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto micProcessor = host.createEffectProcessor(classes[0], 48000.0, 256, audient::vst3::BusLayout::Mono);
    auto outputProcessor = host.createEffectProcessor(classes[0], 48000.0, 256, audient::vst3::BusLayout::Stereo);
    ASSERT_NE(micProcessor, nullptr);
    ASSERT_NE(outputProcessor, nullptr);
    EXPECT_NE(micProcessor->component(), outputProcessor->component()) << "chains must not share one instance";

    // Two independent instances of the SAME plugin class with different gains.
    ASSERT_TRUE(micProcessor->restoreState(gainBlob(kMicGain)));
    ASSERT_TRUE(outputProcessor->restoreState(gainBlob(kOutputGain)));

    audient::vst3::Vst3Chain micChain;
    audient::vst3::Vst3Chain outputChain;
    ASSERT_TRUE(micChain.publish({{micProcessor.get(), false}}, false));
    ASSERT_TRUE(outputChain.publish({{outputProcessor.get(), false}}, false));

    audient::engine::EngineGraph graph(256);
    audient::engine::EngineConfig config;
    config.sampleRateHz = 48000;
    config.maxBlockSamples = 256;
    config.micMonitorMute = true; // keep the monitor branch out of the output read
    config.micSendDb = 0.0f;
    config.revision = 1;
    graph.publishConfig(config);
    graph.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain);
    graph.setOutputChain(&audient::vst3::Vst3Chain::processStereo, &outputChain);

    audient::transport::DownlinkTransport downlink({48000, 2, 64, audient::transport::SampleType::Float32}, 4096);

    audient::asio::AsioStreamBridge bridge(backend);
    bridge.attach(graph, 256);
    bridge.setDownlinkTransport(&downlink);
    bridge.requestFadeIn();

    std::atomic<bool> stopRender{false};
    std::thread renderProducer([&]() {
        std::vector<float> block(128);
        while (!stopRender.load(std::memory_order_acquire))
        {
            for (std::size_t i = 0; i < 64; ++i)
            {
                block[2 * i] = 0.2f;
                block[2 * i + 1] = -0.4f;
            }
            downlink.writeBlock(block.data(), 64);
        }
    });

    // RAII shutdown fence: whatever happens (ASSERT failure, timeout, early
    // return), the simulated-driver engine loop and the render producer are
    // stopped and joined before the test unwinds, so no live joinable thread
    // survives into gtest or the AsioBackend destructor.
    struct StreamShutdown
    {
        audient::asio::AsioBackend* backend = nullptr;
        std::atomic<bool>* stopRender = nullptr;
        std::thread* renderProducer = nullptr;
        ~StreamShutdown()
        {
            if (stopRender != nullptr)
            {
                stopRender->store(true, std::memory_order_release);
            }
            if (renderProducer != nullptr && renderProducer->joinable())
            {
                renderProducer->join();
            }
            if (backend != nullptr)
            {
                std::string ignored;
                backend->stop(ignored);
            }
        }
    } shutdown{&backend, &stopRender, &renderProducer};

    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state output is read";
    // Wait for a level to actually reach the outputs instead of a fixed sleep,
    // so a slow test machine cannot sample a zero/incomplete block.
    ASSERT_TRUE(waitUntil(
        [&]() {
            audient::asio::SimulatedAsioDriver* live = provider.lastOpenedDriver();
            if (live == nullptr)
            {
                return false;
            }
            const std::vector<float> liveLeft = live->lastOutput(0);
            const std::vector<float> liveRight = live->lastOutput(1);
            return !liveLeft.empty() && !liveRight.empty() &&
                   (std::fabs(liveLeft[0]) > 0.01f || std::fabs(liveRight[0]) > 0.01f);
        },
        std::chrono::milliseconds(3000)))
        << "processed blocks must reach the physical outputs";

    audient::asio::SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> outLeft = driver->lastOutput(0);
    const std::vector<float> outRight = driver->lastOutput(1);
    const std::vector<float>& uplink = bridge.micUplinkBuffer();
    ASSERT_FALSE(outLeft.empty());
    ASSERT_FALSE(outRight.empty());
    ASSERT_FALSE(uplink.empty());

    ASSERT_TRUE(backend.stop(error));

    // Mic chain: physical input 0.25 -> mic-chain gain 0.25 into the uplink.
    EXPECT_NEAR(uplink[0], 0.25f * kMicGain, 1e-3f) << "mic chain must apply its own instance's gain";
    // Output chain: downlink L/R scaled by the output instance's gain, L/R kept.
    EXPECT_NEAR(outLeft[0], 0.2f * kOutputGain, 0.02f) << "output chain must apply its own instance's gain";
    EXPECT_NEAR(outRight[0], -0.4f * kOutputGain, 0.02f);
    EXPECT_NE(outLeft[0], outRight[0]) << "left/right mapping must survive the output chain";

    // No shared state after the run: the two instances persist different state
    // and mutating one does not change the other's saved state.
    std::vector<std::uint8_t> micBlob;
    std::vector<std::uint8_t> outputBlob;
    ASSERT_TRUE(micProcessor->saveState(micBlob));
    ASSERT_TRUE(outputProcessor->saveState(outputBlob));
    EXPECT_NE(micBlob, outputBlob);

    audient::vst3_test::TestPassthroughComponent* micComp =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(micProcessor->component());
    ASSERT_NE(micComp, nullptr);
    micComp->setTestState(0.5f, 999u);
    std::vector<std::uint8_t> outputBlobAfter;
    ASSERT_TRUE(outputProcessor->saveState(outputBlobAfter));
    EXPECT_EQ(outputBlobAfter, outputBlob) << "mutating the mic instance must not reach the output instance";
}

TEST(Vst3MultiPluginTest, BypassingOrRebuildingOneChainLeavesTheOtherUntouched) // VST-025
{
    constexpr float kMicGain = 0.25f;
    constexpr float kOutputGain = 0.9f;
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto micProcessor = host.createEffectProcessor(classes[0], 48000.0, 64, audient::vst3::BusLayout::Mono);
    auto outputProcessor = host.createEffectProcessor(classes[0], 48000.0, 256, audient::vst3::BusLayout::Stereo);
    ASSERT_NE(micProcessor, nullptr);
    ASSERT_NE(outputProcessor, nullptr);
    ASSERT_TRUE(micProcessor->restoreState(gainBlob(kMicGain)));
    ASSERT_TRUE(outputProcessor->restoreState(gainBlob(kOutputGain)));

    // Two separate chain objects (independent snapshot/state/retirement).
    audient::vst3::Vst3Chain micChain;
    audient::vst3::Vst3Chain outputChain;
    ASSERT_TRUE(micChain.publish({{micProcessor.get(), false}}, false));
    ASSERT_TRUE(outputChain.publish({{outputProcessor.get(), false}}, false));

    const auto processMono = [](audient::vst3::Vst3Chain& chain, float value) {
        // In-place (the engine always runs the chain in-place): a bypassed
        // chain leaves the buffer untouched, which in-place equals passthrough.
        std::array<float, 64> io{};
        io.fill(value);
        audient::vst3::Vst3Chain::processMono(io.data(), io.data(), io.size(), &chain);
        return io[0];
    };
    const auto processStereoLeft = [](audient::vst3::Vst3Chain& chain, float valueLeft) {
        // In-place stereo passthrough for active-chain reads.
        std::array<float, 64> left{};
        std::array<float, 64> right{};
        left.fill(valueLeft);
        right.fill(-valueLeft);
        audient::vst3::Vst3Chain::processStereo(left.data(), right.data(), left.data(), right.data(), left.size(),
                                                &chain);
        return left[0];
    };

    // Baseline: both chains process with their own gains.
    EXPECT_NEAR(processMono(micChain, 0.5f), 0.5f * kMicGain, 1e-6f);
    EXPECT_NEAR(processStereoLeft(outputChain, 0.5f), 0.5f * kOutputGain, 1e-6f);
    const std::uint64_t micBlocksBefore = micChain.blockCounter();
    const std::uint64_t outputBlocksBefore = outputChain.blockCounter();

    // Rebuild the MIC chain (new snapshot, whole-chain bypass) while the output
    // chain keeps its old active snapshot.
    ASSERT_TRUE(micChain.publish({{micProcessor.get(), false}}, true));
    EXPECT_NEAR(processStereoLeft(outputChain, 0.5f), 0.5f * kOutputGain, 1e-6f)
        << "rebuilding the mic chain must not mutate the output chain snapshot";
    EXPECT_NEAR(processMono(micChain, 0.5f), 0.5f, 1e-6f) << "mic chain is now whole-chain bypassed";
    EXPECT_GE(micChain.blockCounter(), micBlocksBefore);
    EXPECT_GE(outputChain.blockCounter(), outputBlocksBefore);
    {
        // Out-of-place sentinel: whole-chain bypass must not write the buffer at all.
        std::array<float, 64> io{};
        std::array<float, 64> out{};
        io.fill(0.5f);
        out.fill(-9.0f);
        audient::vst3::Vst3Chain::processMono(io.data(), out.data(), io.size(), &micChain);
        EXPECT_NEAR(out[0], -9.0f, 1e-6f) << "whole-chain bypass must not write a non-aliased buffer";
    }

    // Rebuild the OUTPUT chain (per-slot bypass) without touching the mic chain.
    ASSERT_TRUE(outputChain.publish({{outputProcessor.get(), true}}, false));
    EXPECT_NEAR(processMono(micChain, 0.5f), 0.5f, 1e-6f) << "mic chain still bypassed, untouched";
    {
        // Separate buffers with a sentinel: a bypassed slot must not write them.
        std::array<float, 64> left{};
        std::array<float, 64> right{};
        std::array<float, 64> leftOut{};
        std::array<float, 64> rightOut{};
        left.fill(0.5f);
        right.fill(-0.5f);
        leftOut.fill(-9.0f);
        rightOut.fill(-9.0f);
        audient::vst3::Vst3Chain::processStereo(left.data(), right.data(), leftOut.data(), rightOut.data(), left.size(),
                                                &outputChain);
        EXPECT_NEAR(leftOut[0], -9.0f, 1e-6f) << "bypassed output slot must leave the buffer untouched";
        EXPECT_NEAR(rightOut[0], -9.0f, 1e-6f);
    }

    // Restore the mic chain active; output chain's current per-slot bypass is
    // independent, and the mic processor's state survived every rebuild.
    ASSERT_TRUE(micChain.publish({{micProcessor.get(), false}}, false));
    EXPECT_NEAR(processMono(micChain, 0.5f), 0.5f * kMicGain, 1e-6f);
    audient::vst3_test::TestPassthroughComponent* micComp =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(micProcessor->component());
    ASSERT_NE(micComp, nullptr);
    EXPECT_EQ(micComp->testGain(), kMicGain) << "mic processor state untouched by output-chain rebuilds";

    // Restore the output chain active and confirm both are independent again.
    ASSERT_TRUE(outputChain.publish({{outputProcessor.get(), false}}, false));
    EXPECT_NEAR(processStereoLeft(outputChain, 0.5f), 0.5f * kOutputGain, 1e-6f);
    EXPECT_NEAR(processMono(micChain, 0.5f), 0.5f * kMicGain, 1e-6f);
}