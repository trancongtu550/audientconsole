#include "TestPassthroughPlugin.h"

#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3HostContext.h"
#include "vst3/Vst3Processor.h"

#include "pluginterfaces/vst/ivstcomponent.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Slice M: VST-016 latency change + safe chain reconfiguration.
//
// Confirmed current behavior (from code + SDK spec) is recorded in project plan:
// getLatencySamples is queried once at prepare; the chain had NO latency
// aggregation; restartComponent(kLatencyChanged) was ignored. Slice M adds the
// chain's active-path total and control-thread detect + safe republish on
// latency change. This file proves:
//  - Vst3Chain::totalLatencySamples() sums ACTIVE slots only (VST-016 path).
//  - Bypass (per-slot and whole-chain) excludes the plug-in from the total.
//  - Vst3Processor::refreshLatencySamples() detects a reported change and the
//    host republishes the chain snapshot safely on the control thread.
//  - Vst3HostContext records+consumes the kLatencyChanged restart signal.

TEST(Vst3LatencyTest, TotalLatencySumsOnlyActiveSlots) // VST-016 path
{
    // Three independent instances of the SAME test plug-in class, each with its
    // own reported latency. Chain total = sum of non-null, non-bypassed slots.
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto a = host.createEffectProcessor(classes[0], 48000.0, 64);
    auto b = host.createEffectProcessor(classes[0], 48000.0, 64);
    auto c = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    audient::vst3_test::TestPassthroughComponent* compA =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(a->component());
    audient::vst3_test::TestPassthroughComponent* compB =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(b->component());
    audient::vst3_test::TestPassthroughComponent* compC =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(c->component());
    ASSERT_NE(compA, nullptr);
    ASSERT_NE(compB, nullptr);
    ASSERT_NE(compC, nullptr);

    // Latency reported at prepare time is whatever the plugin says NOW (0 by
    // default for the test plugin). Set distinct latencies; prepare already
    // cached getLatencySamples once, so refresh must see the change.
    compA->setTestLatency(128u);
    compB->setTestLatency(64u);
    compC->setTestLatency(32u);
    ASSERT_TRUE(a->refreshLatencySamples());
    ASSERT_TRUE(b->refreshLatencySamples());
    ASSERT_TRUE(c->refreshLatencySamples());

    audient::vst3::Vst3Chain chain;
    ASSERT_TRUE(chain.publish({{a.get(), false}, {b.get(), false}, {c.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 128u + 64u + 32u) << "active slots sum to the chain total";

    // Whole-chain bypass: nothing in the audio path -> zero latency.
    ASSERT_TRUE(chain.publish({{a.get(), false}, {b.get(), false}, {c.get(), false}}, true));
    EXPECT_EQ(chain.totalLatencySamples(), 0u) << "whole-chain bypass removes the whole path";

    // Restore active, then bypass ONE slot: its latency must be excluded.
    ASSERT_TRUE(chain.publish({{a.get(), false}, {b.get(), false}, {c.get(), false}}, false));
    ASSERT_TRUE(chain.publish({{a.get(), false}, {b.get(), true}, {c.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 128u + 32u) << "bypassed slot's latency is out of the path";

    // Remove a slot entirely (null-missing slot) -> also excluded.
    ASSERT_TRUE(chain.publish({{a.get(), false}, {c.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 128u + 32u) << "missing slot contributes nothing";
}

TEST(Vst3LatencyTest, RefreshLatencyDetectsChangeAndChainRepublishIsSafe) // VST-016
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    audient::vst3_test::TestPassthroughComponent* comp =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(comp, nullptr);

    // Default: 0 latency. Refresh returns false while the reported value is
    // unchanged (idempotent).
    EXPECT_FALSE(processor->refreshLatencySamples()) << "no change -> no rebuild needed";
    EXPECT_EQ(processor->latencySamples(), 0u);

    // The plug-in's latency changes (e.g. a parameter/state change made it
    // internally look ahead). The host re-queries and detects the change.
    comp->setTestLatency(256u);
    ASSERT_TRUE(processor->refreshLatencySamples()) << "a reported change must be detected";
    EXPECT_EQ(processor->latencySamples(), 256u);

    // Re-publish the chain on the control thread with the new value; the swap
    // machinery (grace/reap) is the same safe barrier used for add/remove.
    audient::vst3::Vst3Chain chain;
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 256u);

    // A second refresh with no further change is idempotent.
    EXPECT_FALSE(processor->refreshLatencySamples());

    // Reverting the plugin's latency is also detected.
    comp->setTestLatency(0u);
    ASSERT_TRUE(processor->refreshLatencySamples());
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 0u);
}

TEST(Vst3LatencyTest, HostContextRecordsAndConsumesLatencyChangedSignal) // VST-016
{
    // Spec: a plug-in whose reported latency changes must call
    // IComponentHandler::restartComponent(kLatencyChanged); the host reacts on
    // the control thread. The host context must record it instead of ignoring it.
    audient::vst3::Vst3HostContext context;
    EXPECT_FALSE(context.hasPendingLatencyChanged());

    Steinberg::Vst::IComponentHandler* handler = nullptr;
    ASSERT_EQ(context.queryInterface(Steinberg::Vst::IComponentHandler::iid.toTUID(),
                                     reinterpret_cast<void**>(&handler)),
              Steinberg::kResultOk);
    ASSERT_NE(handler, nullptr);

    // A non-latency restart flag must not trigger a latency rebuild.
    EXPECT_EQ(handler->restartComponent(0x0), Steinberg::kResultOk);
    EXPECT_FALSE(context.hasPendingLatencyChanged());

    EXPECT_EQ(handler->restartComponent(Steinberg::Vst::kLatencyChanged), Steinberg::kResultOk);
    EXPECT_TRUE(context.hasPendingLatencyChanged());
    EXPECT_TRUE(context.consumeLatencyChanged()) << "consume reads + clears the pending flag";
    EXPECT_FALSE(context.hasPendingLatencyChanged()) << "consumed flag is cleared";

    handler->release();
}

TEST(Vst3LatencyTest, ControlThreadReactToLatencyChangedSignalsRebuild) // VST-016 full loop
{
    // The control-thread reaction a host performs when a plug-in signals
    // kLatencyChanged: consume the signal, re-query the reported latency, and
    // republish the prepared chain snapshot so the callback swaps to a
    // consistent latency view on the next block. No realtime work here.
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    audient::vst3_test::TestPassthroughComponent* comp =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(comp, nullptr);

    audient::vst3::Vst3Chain chain;
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 0u);

    // The plug-in's latency changes (post-param/state) and it signals the host
    // through the SAME host context object that the processor was prepared with.
    // The control loop consumes the signal, re-queries getLatencySamples, and
    // republishes the chain snapshot on the control thread.
    Steinberg::Vst::IComponentHandler* handler = nullptr;
    ASSERT_EQ(host.hostContext()->queryInterface(Steinberg::Vst::IComponentHandler::iid.toTUID(),
                                                 reinterpret_cast<void**>(&handler)),
              Steinberg::kResultOk);
    ASSERT_NE(handler, nullptr);

    comp->setTestLatency(384u);
    // The plug-in calls restartComponent(kLatencyChanged); the host notices and
    // refreshes its cached latency for the slot.
    EXPECT_EQ(handler->restartComponent(Steinberg::Vst::kLatencyChanged), Steinberg::kResultOk);
    ASSERT_TRUE(processor->refreshLatencySamples());
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 384u) << "chain total reflects the re-queried latency";

    // Consumption order in a real loop: each iteration consumes + refreshes.
    // After a stable state, no further change means no rebuild expected.
    comp->setTestLatency(384u);
    EXPECT_FALSE(processor->refreshLatencySamples()) << "no change after signal -> no republish needed";

    comp->setTestLatency(192u);
    ASSERT_TRUE(processor->refreshLatencySamples());
    ASSERT_TRUE(chain.publish({{processor.get(), false}}, false));
    EXPECT_EQ(chain.totalLatencySamples(), 192u);

    handler->release();
}

TEST(Vst3LatencyTest, TotalLatencySumsMaxSlotsWithoutOverflow) // boundary
{
    // The summation must not overflow for per-slot uint32 latencies at
    // kMaxSlots. Each slot below INT32_MAX/8 keeps the total well inside uint32.
    audient::vst3_test::TestPassthroughFactory factory(0.5f);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    std::vector<audient::vst3::Vst3Chain::Slot> slots;
    std::vector<std::unique_ptr<audient::vst3::Vst3Processor>> keepAlive; // chain does not own processors
    audient::vst3::Vst3Chain chain;
    constexpr std::uint32_t kPerSlot = (1u << 28);
    for (std::size_t i = 0; i < audient::vst3::Vst3Chain::kMaxSlots; ++i)
    {
        auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
        ASSERT_NE(processor, nullptr);
        audient::vst3_test::TestPassthroughComponent* comp =
            audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
        ASSERT_NE(comp, nullptr);
        comp->setTestLatency(kPerSlot);
        ASSERT_TRUE(processor->refreshLatencySamples());
        slots.push_back({processor.get(), false});
        keepAlive.push_back(std::move(processor));
    }
    ASSERT_TRUE(chain.publish(std::move(slots), false));
    EXPECT_EQ(chain.totalLatencySamples(), kPerSlot * audient::vst3::Vst3Chain::kMaxSlots)
        << "sum of active slot latencies must be exact with no overflow";
}