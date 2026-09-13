#include "SimulatedAsioDriver.h"
#include "TestPassthroughPlugin.h"

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioStreamBridge.h"
#include "engine/GraphConfig.h"
#include "engine/EngineGraph.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

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

} // namespace

TEST(Vst3HostTest, DiscoversEffectClassFromFactory)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;

    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);
    EXPECT_EQ(classes[0].name, "Test Passthrough");
    EXPECT_EQ(classes[0].category, "Audio Effect");
    EXPECT_TRUE(classes[0].isEffect());
    EXPECT_FALSE(classes[0].classId.empty());
    EXPECT_EQ(std::memcmp(classes[0].cid, factory.classId().toTUID(), sizeof(Steinberg::TUID)), 0)
        << "PluginClassInfo::cid must carry the RAW 16-byte TUID (VST3 createInstance ABI), "
           "not the hex string form";
}

TEST(Vst3HostTest, CreatesAndProcessesARealtimeBlock)
{
    constexpr float kGain = 0.5f;
    audient::vst3_test::TestPassthroughFactory factory(kGain);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());
    EXPECT_EQ(processor->latencySamples(), 0u);

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        input[i] = 0.25f;
    }

    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());
    for (const float sample : output)
    {
        EXPECT_NEAR(sample, 0.25f * kGain, 1e-6f);
    }
}

TEST(Vst3HostTest, HostRejectsUnsupportedClassId)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    audient::vst3::PluginClassInfo bogus;
    bogus.name = "Bogus";
    bogus.category = "Instrument";
    bogus.classId = "00000000000000000000000000000000";

    auto processor = host.createEffectProcessor(bogus, 48000.0, 64);
    EXPECT_EQ(processor, nullptr);
    EXPECT_FALSE(host.lastError().empty());
}

TEST(Vst3HostTest, StereoOutputBlockPreservesLeftRightAndGain)
{
    constexpr float kGain = 0.5f;
    audient::vst3_test::TestPassthroughFactory factory(kGain);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64, audient::vst3::BusLayout::Stereo);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());
    EXPECT_EQ(processor->latencySamples(), 0u);

    std::array<float, 64> leftIn{};
    std::array<float, 64> rightIn{};
    std::array<float, 64> leftOut{};
    std::array<float, 64> rightOut{};
    for (std::size_t i = 0; i < leftIn.size(); ++i)
    {
        leftIn[i] = 0.25f;
        rightIn[i] = -0.5f;
    }

    audient::vst3::Vst3Processor::chainProcessStereo(leftIn.data(), rightIn.data(), leftOut.data(), rightOut.data(), leftIn.size(), processor.get());

    for (std::size_t i = 0; i < leftOut.size(); ++i)
    {
        EXPECT_NEAR(leftOut[i], 0.25f * kGain, 1e-6f) << "stereo L must be processed with the plug-in gain";
        EXPECT_NEAR(rightOut[i], -0.5f * kGain, 1e-6f) << "stereo R must be processed with the plug-in gain";
    }
}

TEST(Vst3IntegrationGraphTest, MicChainProcessesLiveAudioThroughTheHost)
{
    audient::asio::SimulatedAsioProvider provider;
    audient::asio::AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const audient::asio::ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    constexpr float kGain = 0.5f;
    audient::vst3_test::TestPassthroughFactory factory(kGain);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    auto processor = host.createEffectProcessor(classes[0], 48000.0, 256);
    ASSERT_NE(processor, nullptr);

    audient::engine::EngineGraph graph(256);
    audient::engine::EngineConfig config;
    config.sampleRateHz = 48000;
    config.maxBlockSamples = 256;
    config.micMonitorMute = true;
    config.micSendDb = 0.0f;
    config.revision = 1;
    graph.publishConfig(config);
    graph.setMicChain(audient::vst3::Vst3Processor::chainProcess, processor.get());

    audient::asio::AsioStreamBridge bridge(backend);
    bridge.attach(graph, 256);
    bridge.requestFadeIn();

    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state output is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_TRUE(backend.stop(error));

    const std::vector<float>& uplink = bridge.micUplinkBuffer();
    ASSERT_FALSE(uplink.empty());
    EXPECT_NEAR(uplink[0], 0.25f * kGain, 1e-3f) << "live physical mic through the VST3 host must land in the uplink";
}

TEST(Vst3HostTest, EnqueuedParameterReachesPluginProcessInputChanges)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    audient::vst3_test::TestPassthroughComponent* component =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(component, nullptr);
    const audient::vst3_test::TestPassthroughProcessor& plugProcessor = component->testProcessor();

    constexpr Steinberg::Vst::ParamID kParamId = 0x1234;
    ASSERT_TRUE(processor->enqueueParameter(kParamId, 0.75));

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input.fill(0.25f);
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());

    EXPECT_EQ(processor->parameterDelivered(), 1u) << "one enqueued edit must be delivered to the block";
    EXPECT_EQ(processor->parameterDrops(), 0u);
    EXPECT_EQ(plugProcessor.receivedParameterCount(), 1) << "plugin must see exactly one parameter";
    EXPECT_EQ(plugProcessor.receivedParameterAt(0).id, kParamId);
    EXPECT_EQ(plugProcessor.receivedParameterAt(0).value, 0.75);
    EXPECT_EQ(plugProcessor.receivedParameterAt(0).points, 1u);
}

TEST(Vst3HostTest, ParameterEditDeliveredThroughHostContextPerformEdit)
{
    // VST-009 full path: the plug-in controller's IComponentHandler::performEdit
    // routes into the processor's bounded queue; the next block's process()
    // receives it as inputParameterChanges.
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    // The host withholds a sink by default: edits are a no-op (never crash).
    {
        Steinberg::FUnknown* sinkTarget = host.hostContext();
        ASSERT_NE(sinkTarget, nullptr);

        Steinberg::Vst::IComponentHandler* handler = nullptr;
        ASSERT_EQ(sinkTarget->queryInterface(Steinberg::Vst::IComponentHandler::iid.toTUID(),
                                             reinterpret_cast<void**>(&handler)),
                  Steinberg::kResultOk);
        ASSERT_NE(handler, nullptr);
        EXPECT_EQ(handler->performEdit(0x77, 0.9), Steinberg::kResultOk);
        handler->release();
    }

    // Attach the processor as the active edit sink; edits now enter its queue.
    host.setParameterEditSink(processor.get());

    audient::vst3_test::TestPassthroughComponent* component =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(component, nullptr);
    const audient::vst3_test::TestPassthroughProcessor& plugProcessor = component->testProcessor();

    Steinberg::Vst::IComponentHandler* handler = nullptr;
    ASSERT_EQ(host.hostContext()->queryInterface(Steinberg::Vst::IComponentHandler::iid.toTUID(),
                                                 reinterpret_cast<void**>(&handler)),
              Steinberg::kResultOk);
    ASSERT_NE(handler, nullptr);

    constexpr Steinberg::Vst::ParamID kParamId = 0x99;
    EXPECT_EQ(handler->beginEdit(kParamId), Steinberg::kResultOk);
    EXPECT_EQ(handler->performEdit(kParamId, 0.25), Steinberg::kResultOk);
    EXPECT_EQ(handler->performEdit(kParamId, 0.5), Steinberg::kResultOk);
    EXPECT_EQ(handler->endEdit(kParamId), Steinberg::kResultOk);
    EXPECT_EQ(processor->parameterDelivered(), 0u) << "edits are queued, not yet delivered";

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input.fill(0.25f);
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());

    EXPECT_EQ(processor->parameterDelivered(), 2u);
    EXPECT_EQ(plugProcessor.receivedParameterCount(), 1);
    EXPECT_EQ(plugProcessor.receivedParameterAt(0).id, kParamId);
    EXPECT_EQ(plugProcessor.receivedParameterAt(0).value, 0.5) << "last performEdit value wins in the block";
    EXPECT_GE(plugProcessor.receivedParameterAt(0).points, 2u);

    // Clearing the sink restores the pass-through no-op for the host context.
    host.setParameterEditSink(nullptr);
    EXPECT_EQ(handler->performEdit(kParamId, 0.1), Steinberg::kResultOk);
    input.fill(0.25f);
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());
    EXPECT_EQ(processor->parameterDelivered(), 2u) << "no further edits delivered after sink cleared";
    EXPECT_EQ(processor->parameterDrops(), 0u);
}

TEST(Vst3HostTest, ParameterQueueOverflowDropsAndCountsNewEdits)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    // Fill the bounded queue to capacity without delivering, then keep pushing.
    const std::size_t capacity = audient::vst3::ParameterEditQueue::kDefaultCapacity;
    for (std::size_t i = 0; i < capacity; ++i)
    {
        ASSERT_TRUE(processor->enqueueParameter(static_cast<Steinberg::Vst::ParamID>(i), 0.1))
            << "edits up to capacity must be accepted";
    }
    EXPECT_EQ(processor->parameterDelivered(), 0u);

    std::size_t accepted = 0;
    for (std::size_t i = 0; i < 10; ++i)
    {
        if (processor->enqueueParameter(0x42, 0.2))
        {
            ++accepted;
        }
    }
    EXPECT_EQ(accepted, 0u) << "queue is full: every further edit must be dropped";
    EXPECT_EQ(processor->parameterDrops(), 10u);

    // Drain with a bounded number of blocks. deliverParameterChanges() exposes
    // at most (kMaxQueues * kMaxPoints) distinct points per block and drains up
    // to 32 edits per block, so delivering 256 accepted edits never completes
    // in finite time; the point of this test is that the queue drained enough
    // to accept new edits again (overflow releases as the consumer advances).
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input.fill(0.25f);
    for (std::size_t block = 0; block < 64; ++block)
    {
        audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());
    }

    EXPECT_GT(processor->parameterDelivered(), 0u) << "accepted edits must begin flowing to the plug-in";
    EXPECT_TRUE(processor->enqueueParameter(0x43, 0.3))
        << "after draining, the queue must accept a new edit again";
    EXPECT_GE(processor->parameterDrops(), 10u)
        << "queue-full pushes are counted, and per-block pool overflow may add more";
}

TEST(Vst3HostTest, ParameterDeliveryIsIdempotentAcrossBlocks)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    audient::vst3_test::TestPassthroughComponent* component =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(component, nullptr);

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input.fill(0.25f);

    // Blocks with no pending edits carry an empty parameter change set.
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());
    EXPECT_EQ(component->testProcessor().receivedParameterCount(), 0);
    EXPECT_EQ(processor->parameterDelivered(), 0u);

    ASSERT_TRUE(processor->enqueueParameter(0x10, 0.4));
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());
    EXPECT_EQ(processor->parameterDelivered(), 1u);
    EXPECT_EQ(component->testProcessor().receivedParameterCount(), 1);
    EXPECT_EQ(component->testProcessor().receivedParameterAt(0).value, 0.4);

    // A later empty block must not redeliver the previous parameter.
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());
    EXPECT_EQ(processor->parameterDelivered(), 1u);
    EXPECT_EQ(component->testProcessor().receivedParameterAt(0).value, 0.4);
    EXPECT_EQ(component->testProcessor().receivedParameterCount(), 1);
}

namespace
{

audient::vst3_test::TestPassthroughComponent* componentOf(audient::vst3::Vst3Processor& processor)
{
    return audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor.component());
}

} // namespace

TEST(Vst3HostTest, ComponentStateRoundTripsBitForBit)
{
    constexpr float kGain = 0.25f;
    audient::vst3_test::TestPassthroughFactory factory(kGain);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    audient::vst3_test::TestPassthroughComponent* component = componentOf(*processor);
    ASSERT_NE(component, nullptr);

    component->setTestState(0.75f, 42u);

    std::vector<std::uint8_t> blob;
    ASSERT_TRUE(processor->saveState(blob)) << "saveState must capture plugin state";
    ASSERT_FALSE(blob.empty());

    std::vector<std::uint8_t> again;
    ASSERT_TRUE(processor->saveState(again));
    EXPECT_EQ(again, blob) << "saveState must be deterministic (bit-for-bit)";

    // A second independent instance restores the same blob and reproduces it.
    auto restored = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(restored->valid());

    audient::vst3_test::TestPassthroughComponent* restoredComponent = componentOf(*restored);
    ASSERT_NE(restoredComponent, nullptr);
    EXPECT_NE(restoredComponent->testStateValue(), 0.75f) << "fresh instance starts with default state";

    ASSERT_TRUE(restored->restoreState(blob)) << "restoreState must accept a valid blob";
    EXPECT_EQ(restoredComponent->testStateValue(), 0.75f);
    EXPECT_EQ(restoredComponent->testStateSeq(), 42u);
    EXPECT_EQ(restoredComponent->testGain(), 0.25f) << "state includes the processing gain";

    std::vector<std::uint8_t> reproduced;
    ASSERT_TRUE(restored->saveState(reproduced));
    EXPECT_EQ(reproduced, blob) << "restored instance must reproduce the original blob bit-for-bit";

    // Restored gain actually affects processing: a fresh 0.9-gain instance
    // restored with the 0.25 blob now scales by 0.25, not 0.9.
    auto lateGain = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(lateGain, nullptr);
    ASSERT_TRUE(lateGain->valid());
    ASSERT_TRUE(lateGain->restoreState(blob));

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input.fill(0.2f);
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), lateGain.get());
    EXPECT_NEAR(output[0], 0.2f * kGain, 1e-6f) << "restored state must change the processed gain";
}

TEST(Vst3HostTest, RestoreStateRejectsCorruptBlobAndKeepsProcessorUsable)
{
    audient::vst3_test::TestPassthroughFactory factory(0.25f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    audient::vst3_test::TestPassthroughComponent* component = componentOf(*processor);
    ASSERT_NE(component, nullptr);

    std::vector<std::uint8_t> validBlob;
    ASSERT_TRUE(processor->saveState(validBlob));

    // Corrupt a payload byte: magic/version/length/checksum combine to reject.
    std::vector<std::uint8_t> corrupt = validBlob;
    corrupt.back() ^= 0x01;
    EXPECT_FALSE(processor->restoreState(corrupt)) << "corrupt payload must be rejected (VST-014)";
    EXPECT_FALSE(processor->lastPrepareError().empty()) << "a reason must be recorded";
    EXPECT_TRUE(processor->valid()) << "a rejected blob must not break the processor";

    // Corrupt the magic.
    std::vector<std::uint8_t> badMagic = validBlob;
    badMagic[0] ^= 0xFF;
    EXPECT_FALSE(processor->restoreState(badMagic));

    // A truncated blob is rejected without touching the processor.
    std::vector<std::uint8_t> truncated(validBlob.begin(), validBlob.end() - 4);
    EXPECT_FALSE(processor->restoreState(truncated));

    // The processor still saves and processes correctly afterwards.
    EXPECT_TRUE(processor->valid());
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input.fill(0.5f);
    audient::vst3::Vst3Processor::chainProcess(input.data(), output.data(), input.size(), processor.get());
    EXPECT_NEAR(output[0], 0.5f * 0.25f, 1e-6f);
    EXPECT_EQ(component->testStateValue(), 0.0f) << "rejected restore must not mutate state";
}

TEST(Vst3HostTest, RestoreStateRejectsOversizedBlobWithoutAllocatingPayload)
{
    audient::vst3_test::TestPassthroughFactory factory(0.25f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    // A header claiming a payload beyond kMaxStateBytes must be rejected by the
    // decoder before any payload buffer is allocated (STATE-004 behavior).
    std::vector<std::uint8_t> oversizedHeader = audient::vst3::Vst3StateBlob::encode({0x01});
    // Corrupt the length field to a huge value.
    oversizedHeader[8] = 0xFF;
    oversizedHeader[9] = 0xFF;
    oversizedHeader[10] = 0xFF;
    oversizedHeader[11] = 0x3F; // ~6.8 GB claimed

    EXPECT_FALSE(processor->restoreState(oversizedHeader));
    EXPECT_TRUE(processor->valid());
}

TEST(Vst3IntegrationGraphTest, StereoOutputChainProcessesDownlinkThroughTheHost)
{
    audient::asio::SimulatedAsioProvider provider;
    audient::asio::AsioBackend backend(provider);

    std::string error;
    ASSERT_TRUE(backend.selectAudientDevice(error));
    const audient::asio::ChannelPlan plan = planFromCapabilities(backend.driverCapabilities());
    ASSERT_TRUE(plan.valid());
    ASSERT_TRUE(backend.configure(48000, 64, plan, error)) << error;

    constexpr float kGain = 0.25f;
    audient::vst3_test::TestPassthroughFactory factory(kGain);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    auto processor = host.createEffectProcessor(classes[0], 48000.0, 256, audient::vst3::BusLayout::Stereo);
    ASSERT_NE(processor, nullptr);

    audient::engine::EngineGraph graph(256);
    audient::engine::EngineConfig config;
    config.sampleRateHz = 48000;
    config.maxBlockSamples = 256;
    config.micMonitorMute = true;
    config.micSendDb = 0.0f;
    config.revision = 1;
    graph.publishConfig(config);
    graph.setOutputChain(audient::vst3::Vst3Processor::chainProcessStereo, processor.get());

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

    ASSERT_TRUE(backend.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return backend.callbackCount() > 0; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(waitUntil([&]() { return bridge.fadeIsFull(); }, std::chrono::milliseconds(3000)))
        << "the stream must fade in before steady-state output is read";
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    audient::asio::SimulatedAsioDriver* driver = provider.lastOpenedDriver();
    ASSERT_NE(driver, nullptr);
    const std::vector<float> outLeft = driver->lastOutput(0);
    const std::vector<float> outRight = driver->lastOutput(1);
    ASSERT_FALSE(outLeft.empty());
    ASSERT_FALSE(outRight.empty());

    ASSERT_TRUE(backend.stop(error));
    stopRender.store(true, std::memory_order_release);
    renderProducer.join();

    EXPECT_NEAR(outLeft[0], 0.2f * kGain, 0.02f) << "stereo output chain must process downlink L";
    EXPECT_NEAR(outRight[0], -0.4f * kGain, 0.02f) << "stereo output chain must process downlink R";
    EXPECT_NE(outLeft[0], outRight[0]) << "left/right mapping must be preserved by the stereo output chain";
}