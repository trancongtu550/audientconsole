#include "vst3/StreamingParameterChanges.h"

#include <gtest/gtest.h>

#include <cstdint>

// Host-side IParameterChanges fed to IAudioProcessor::process as
// inputParameterChanges (VST-009). These tests pin the realtime contract:
// fixed pools (no growth), insertion-order parameter reuse, per-queue point
// caps, defined overflow (nullptr/false, never an allocation), and reset.

namespace
{

constexpr Steinberg::Vst::ParamID kA = 0x11;
constexpr Steinberg::Vst::ParamID kB = 0x22;
constexpr Steinberg::Vst::ParamID kC = 0x33;

void appendAll(audient::vst3::StreamingParameterChanges& changes,
               std::initializer_list<Steinberg::Vst::ParamValue> values,
               Steinberg::Vst::ParamID id = kA)
{
    for (const Steinberg::Vst::ParamValue value : values)
    {
        ASSERT_TRUE(changes.addPoint(id, 0, value)) << "append within capacity must succeed";
    }
}

} // namespace

TEST(StreamingParameterChangesTest, FreshlyConstructedStateIsEmpty)
{
    audient::vst3::StreamingParameterChanges changes;
    EXPECT_EQ(changes.getParameterCount(), 0);
    EXPECT_EQ(changes.getParameterData(0), nullptr);
    EXPECT_EQ(changes.pointCount(kA), 0);
}

TEST(StreamingParameterChangesTest, AddsQueuesInInsertionOrderAndCoalescesByParamId)
{
    audient::vst3::StreamingParameterChanges changes;
    ASSERT_TRUE(changes.addPoint(kA, 0, 0.1));
    ASSERT_TRUE(changes.addPoint(kB, 0, 0.2));
    ASSERT_TRUE(changes.addPoint(kA, 0, 0.3)); // reuse queue for A, append point

    EXPECT_EQ(changes.getParameterCount(), 2); // A + B, not 3
    EXPECT_EQ(changes.pointCount(kA), 2);
    EXPECT_EQ(changes.pointCount(kB), 1);

    Steinberg::Vst::IParamValueQueue* first = changes.getParameterData(0);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->getParameterId(), kA);
    EXPECT_EQ(first->getPointCount(), 2);

    Steinberg::Vst::IParamValueQueue* second = changes.getParameterData(1);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->getParameterId(), kB);
    EXPECT_EQ(second->getPointCount(), 1);
}

TEST(StreamingParameterChangesTest, GetPointRoundTripsOffsetsAndValues)
{
    audient::vst3::StreamingParameterChanges changes;
    ASSERT_TRUE(changes.addPoint(kA, 3, 0.4));
    ASSERT_TRUE(changes.addPoint(kA, 7, 0.9));

    Steinberg::Vst::IParamValueQueue* queue = nullptr;
    for (Steinberg::int32 i = 0; i < changes.getParameterCount(); ++i)
    {
        if (changes.getParameterData(i)->getParameterId() == kA)
        {
            queue = changes.getParameterData(i);
            break;
        }
    }
    ASSERT_NE(queue, nullptr);
    EXPECT_EQ(queue->getParameterId(), kA);

    Steinberg::int32 offset0 = -1;
    Steinberg::Vst::ParamValue value0 = 0.0;
    ASSERT_EQ(queue->getPoint(0, offset0, value0), Steinberg::kResultOk);
    EXPECT_EQ(offset0, 3);
    EXPECT_EQ(value0, 0.4);

    Steinberg::int32 offset1 = -1;
    Steinberg::Vst::ParamValue value1 = 0.0;
    ASSERT_EQ(queue->getPoint(1, offset1, value1), Steinberg::kResultOk);
    EXPECT_EQ(offset1, 7);
    EXPECT_EQ(value1, 0.9);

    Steinberg::int32 badOffset = -1;
    Steinberg::Vst::ParamValue badValue = 0.0;
    EXPECT_EQ(queue->getPoint(2, badOffset, badValue), Steinberg::kInvalidArgument);
    EXPECT_EQ(badOffset, -1) << "out-of-range getPoint must not clobber outputs";
}

TEST(StreamingParameterChangesTest, AddPointEnforcesPerQueuePointCap)
{
    audient::vst3::StreamingParameterChanges changes;
    appendAll(changes, {0.1, 0.2, 0.3, 0.4});
    EXPECT_EQ(changes.pointCount(kA), audient::vst3::StreamingParameterChanges::kMaxPoints);

    // Fifth value in the same queue must be rejected, not wrapped or dropped
    // silently, and must not grow the structure.
    EXPECT_FALSE(changes.addPoint(kA, 0, 0.5));
    EXPECT_EQ(changes.pointCount(kA), audient::vst3::StreamingParameterChanges::kMaxPoints);
    EXPECT_EQ(changes.getParameterCount(), 1);
}

TEST(StreamingParameterChangesTest, AddPointEnforcesQueuePoolCap)
{
    audient::vst3::StreamingParameterChanges changes;
    for (Steinberg::int32 i = 0; i < audient::vst3::StreamingParameterChanges::kMaxQueues; ++i)
    {
        ASSERT_TRUE(changes.addPoint(static_cast<Steinberg::Vst::ParamID>(kA + i), 0, 0.1))
            << "distinct params up to the pool must be accepted";
    }
    EXPECT_EQ(changes.getParameterCount(), audient::vst3::StreamingParameterChanges::kMaxQueues);

    const Steinberg::Vst::ParamID extra = static_cast<Steinberg::Vst::ParamID>(kC + 0x10FF);
    EXPECT_FALSE(changes.addPoint(extra, 0, 0.1)) << "pool exhausted: new distinct param must be rejected";
    EXPECT_EQ(changes.getParameterCount(), audient::vst3::StreamingParameterChanges::kMaxQueues);
}

TEST(StreamingParameterChangesTest, AddParameterDataReusesExistingQueueAndSignalsExhaustion)
{
    audient::vst3::StreamingParameterChanges changes;
    Steinberg::int32 indexA1 = -1;
    Steinberg::Vst::IParamValueQueue* queueA1 = changes.addParameterData(kA, indexA1);
    ASSERT_NE(queueA1, nullptr);

    Steinberg::int32 indexA2 = -1;
    Steinberg::Vst::IParamValueQueue* queueA2 = changes.addParameterData(kA, indexA2);
    EXPECT_EQ(indexA2, indexA1) << "same param id must reuse its existing queue";
    EXPECT_EQ(queueA2, queueA1);

    // kA already occupies one queue, so at most (kMaxQueues - 1) distinct
    // additions remain available.
    for (Steinberg::int32 i = 0; i < audient::vst3::StreamingParameterChanges::kMaxQueues - 1; ++i)
    {
        Steinberg::Vst::IParamValueQueue* q =
            changes.addParameterData(static_cast<Steinberg::Vst::ParamID>(kB + i), indexA1);
        ASSERT_NE(q, nullptr) << "distinct params up to pool capacity";
        q->addPoint(0, 0.5, indexA1);
    }

    Steinberg::int32 indexExhausted = -1;
    EXPECT_EQ(changes.addParameterData(kC, indexExhausted), nullptr);
    EXPECT_EQ(indexExhausted, -1);
}

TEST(StreamingParameterChangesTest, ResetReturnsToEmpty)
{
    audient::vst3::StreamingParameterChanges changes;
    appendAll(changes, {0.1, 0.2});
    ASSERT_TRUE(changes.addPoint(kB, 0, 0.3));
    ASSERT_TRUE(changes.addPoint(kC, 0, 0.4));
    EXPECT_EQ(changes.getParameterCount(), 3);

    changes.reset();
    EXPECT_EQ(changes.getParameterCount(), 0);
    EXPECT_EQ(changes.pointCount(kA), 0);
    EXPECT_EQ(changes.pointCount(kB), 0);
    EXPECT_EQ(changes.pointCount(kC), 0);

    appendAll(changes, {0.9});
    EXPECT_EQ(changes.pointCount(kA), 1);
}

TEST(StreamingParameterChangesTest, QueryInterfaceExposesIParameterChangesAndFUnknown)
{
    audient::vst3::StreamingParameterChanges changes;

    Steinberg::Vst::IParameterChanges* asChanges = nullptr;
    EXPECT_EQ(changes.queryInterface(Steinberg::Vst::IParameterChanges::iid.toTUID(),
                                     reinterpret_cast<void**>(&asChanges)),
              Steinberg::kResultOk);
    ASSERT_NE(asChanges, nullptr);
    // queryInterface already took one ref (1 -> 2); addRef is then balanced.
    EXPECT_EQ(asChanges->addRef(), 3u);
    EXPECT_EQ(asChanges->release(), 2u);
    EXPECT_EQ(asChanges->release(), 1u);

    Steinberg::FUnknown* asUnknown = nullptr;
    EXPECT_EQ(changes.queryInterface(Steinberg::FUnknown::iid.toTUID(), reinterpret_cast<void**>(&asUnknown)),
              Steinberg::kResultOk);
    ASSERT_NE(asUnknown, nullptr);
    EXPECT_EQ(asUnknown->release(), 1u) << "release cannot delete an object owned by the processor";

    Steinberg::Vst::IParamValueQueue* asQueue = nullptr;
    EXPECT_EQ(changes.queryInterface(Steinberg::Vst::IParamValueQueue::iid.toTUID(),
                                     reinterpret_cast<void**>(&asQueue)),
              Steinberg::kNoInterface);
    EXPECT_EQ(asQueue, nullptr);
}

TEST(StreamingParameterChangesTest, QueueQueryInterfaceAndRefcount)
{
    audient::vst3::StreamingParameterChanges changes;
    ASSERT_TRUE(changes.addPoint(kA, 0, 0.1));

    Steinberg::Vst::IParamValueQueue* queue = nullptr;
    for (Steinberg::int32 i = 0; i < changes.getParameterCount(); ++i)
    {
        queue = changes.getParameterData(i);
    }
    ASSERT_NE(queue, nullptr);

    Steinberg::Vst::IParamValueQueue* asValueQueue = nullptr;
    EXPECT_EQ(queue->queryInterface(Steinberg::Vst::IParamValueQueue::iid.toTUID(),
                                    reinterpret_cast<void**>(&asValueQueue)),
              Steinberg::kResultOk);
    ASSERT_NE(asValueQueue, nullptr);
    EXPECT_EQ(asValueQueue->addRef(), 3u);
    EXPECT_EQ(asValueQueue->release(), 2u);
    EXPECT_EQ(asValueQueue->release(), 1u);
}

TEST(StreamingParameterChangesTest, QueueAddPointBoundary)
{
    audient::vst3::StreamingParameterChanges changes;
    Steinberg::int32 index = -1;
    Steinberg::Vst::IParamValueQueue* queue = changes.addParameterData(kA, index);
    ASSERT_NE(queue, nullptr);

    for (Steinberg::int32 i = 0; i < audient::vst3::StreamingParameterChanges::kMaxPoints; ++i)
    {
        EXPECT_EQ(queue->addPoint(0, 0.1, index), Steinberg::kResultOk);
    }
    EXPECT_EQ(queue->addPoint(0, 0.7, index), Steinberg::kResultFalse);
    EXPECT_EQ(index, -1);
    EXPECT_EQ(queue->getPointCount(), audient::vst3::StreamingParameterChanges::kMaxPoints);
}

TEST(StreamingParameterChangesTest, GetParameterDataRejectsOutOfRange)
{
    audient::vst3::StreamingParameterChanges changes;
    appendAll(changes, {0.1});
    EXPECT_EQ(changes.getParameterData(1), nullptr);
    EXPECT_EQ(changes.getParameterData(-1), nullptr);
    EXPECT_EQ(changes.getParameterCount(), 1);
}
