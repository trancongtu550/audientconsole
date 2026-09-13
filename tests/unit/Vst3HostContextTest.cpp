#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "vst3/Vst3HostContext.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

// Minimal IParameterEditSink used by the host context routing tests. No audio
// code, no queue: it just records what performEdit handed to the sink so the
// routing, clearing, and re-routing behavior can be asserted directly.
class RecordingEditSink final : public audient::vst3::IParameterEditSink
{
public:
    void onParameterEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override
    {
        lastId = id;
        lastValue = valueNormalized;
        ++calls;
    }

    Steinberg::Vst::ParamID lastId = Steinberg::Vst::kNoParamId;
    Steinberg::Vst::ParamValue lastValue = 0.0;
    int calls = 0;
};

template <typename I, typename O>
Steinberg::IPtr<I> queryFrom(O* object)
{
    Steinberg::IPtr<I> result;
    EXPECT_EQ(object->queryInterface(I::iid.toTUID(), reinterpret_cast<void**>(&result)), Steinberg::kResultOk);
    return result;
}

} // namespace

TEST(Vst3HostContextTest, ExposesIHostApplicationWithName)
{
    Steinberg::Vst::IHostApplication* app = nullptr;
    {
        Steinberg::IPtr<audient::vst3::Vst3HostContext> owned = Steinberg::owned(new audient::vst3::Vst3HostContext());
        ASSERT_EQ(owned->queryInterface(Steinberg::Vst::IHostApplication::iid.toTUID(),
                                        reinterpret_cast<void**>(&app)),
                  Steinberg::kResultOk);
        ASSERT_NE(app, nullptr);

        Steinberg::Vst::String128 name{};
        EXPECT_EQ(app->getName(name), Steinberg::kResultOk);
        std::string utf8;
        for (int i = 0; i < 128 && name[i] != 0; ++i)
        {
            utf8.push_back(static_cast<char>(name[i]));
        }
        EXPECT_EQ(utf8, "Audient Console");
    }
    // The host context released its own ref when `owned` went out of scope; `app`
    // is the same object and is now dangling, which is fine because this test only
    // validates the interface surface above while `owned` was alive.
    (void)app;
}

TEST(Vst3HostContextTest, ExposesIComponentHandlerSurface)
{
    Steinberg::IPtr<audient::vst3::Vst3HostContext> context = Steinberg::owned(new audient::vst3::Vst3HostContext());

    Steinberg::Vst::IComponentHandler* handler = nullptr;
    EXPECT_EQ(context->queryInterface(Steinberg::Vst::IComponentHandler::iid.toTUID(),
                                      reinterpret_cast<void**>(&handler)),
              Steinberg::kResultOk);
    ASSERT_NE(handler, nullptr);

    EXPECT_EQ(handler->beginEdit(1), Steinberg::kResultOk);
    EXPECT_EQ(handler->performEdit(1, 0.5), Steinberg::kResultOk);
    EXPECT_EQ(handler->endEdit(1), Steinberg::kResultOk);
    EXPECT_EQ(handler->restartComponent(0), Steinberg::kResultOk);
    handler->release();
}

TEST(Vst3HostContextTest, CreateInstanceOfHostObjectIsNotProvided)
{
    Steinberg::IPtr<audient::vst3::Vst3HostContext> context = Steinberg::owned(new audient::vst3::Vst3HostContext());
    Steinberg::FUnknown* obj = nullptr;
    Steinberg::TUID someCid = {};
    EXPECT_EQ(context->createInstance(someCid, someCid, reinterpret_cast<void**>(&obj)), Steinberg::kNoInterface);
    EXPECT_EQ(obj, nullptr);
}

TEST(Vst3HostContextTest, RefcountIsReferenceTracked)
{
    auto* context = new audient::vst3::Vst3HostContext();
    EXPECT_EQ(context->queryInterface(Steinberg::Vst::IHostApplication::iid.toTUID(), nullptr), Steinberg::kInvalidArgument);
    const Steinberg::uint32 count = context->release();
    EXPECT_EQ(count, 0u);
}

TEST(Vst3HostContextTest, PerformEditRoutesToParameterEditSinkWhenSet)
{
    audient::vst3::Vst3HostContext context;
    auto handler = queryFrom<Steinberg::Vst::IComponentHandler>(&context);
    ASSERT_NE(handler, nullptr);

    RecordingEditSink sink;
    context.setParameterEditSink(&sink);

    EXPECT_EQ(handler->performEdit(0x22, 0.31), Steinberg::kResultOk);
    EXPECT_EQ(handler->performEdit(0x22, 0.72), Steinberg::kResultOk);
    EXPECT_EQ(sink.calls, 2);
    EXPECT_EQ(sink.lastId, Steinberg::Vst::ParamID(0x22));
    EXPECT_DOUBLE_EQ(sink.lastValue, 0.72);
}

TEST(Vst3HostContextTest, PerformEditWithoutSinkIsBoundedNoOp)
{
    audient::vst3::Vst3HostContext context;
    auto handler = queryFrom<Steinberg::Vst::IComponentHandler>(&context);
    ASSERT_NE(handler, nullptr);

    // No sink configured: performEdit must remain a safe, near-free no-op and
    // never null-deref or allocate.
    EXPECT_EQ(handler->performEdit(0x44, 0.9), Steinberg::kResultOk);
    EXPECT_EQ(handler->performEdit(0x44, 0.1), Steinberg::kResultOk);
}

TEST(Vst3HostContextTest, ClearingSinkRestoresBoundedNoOp)
{
    audient::vst3::Vst3HostContext context;
    auto handler = queryFrom<Steinberg::Vst::IComponentHandler>(&context);
    ASSERT_NE(handler, nullptr);

    RecordingEditSink sink;
    context.setParameterEditSink(&sink);
    EXPECT_EQ(handler->performEdit(0x55, 0.8), Steinberg::kResultOk);
    EXPECT_EQ(sink.calls, 1);

    context.setParameterEditSink(nullptr);
    EXPECT_EQ(handler->performEdit(0x55, 0.7), Steinberg::kResultOk);
    EXPECT_EQ(sink.calls, 1) << "cleared sink must no longer receive edits";

    context.setParameterEditSink(&sink);
    EXPECT_EQ(handler->performEdit(0x55, 0.6), Steinberg::kResultOk);
    EXPECT_EQ(sink.calls, 2) << "re-setting a sink resumes routing";
}

TEST(Vst3HostContextTest, BeginAndEndEditRemainNoOps)
{
    audient::vst3::Vst3HostContext context;
    auto handler = queryFrom<Steinberg::Vst::IComponentHandler>(&context);
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->beginEdit(0x66), Steinberg::kResultOk);
    EXPECT_EQ(handler->endEdit(0x66), Steinberg::kResultOk);
}