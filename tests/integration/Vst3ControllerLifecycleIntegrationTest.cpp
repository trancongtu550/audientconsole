#include "TestPassthroughPlugin.h"

#include "vst3/Vst3Controller.h"
#include "vst3/Vst3Host.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{

// VST-010: same-object (single-component) plug-in. The component itself exposes
// IEditController; the host must resolve it as the component, must NOT
// initialize or connect it again, and must NOT terminate it separately
// (component terminate belongs to Vst3Processor::teardown).
TEST(Vst3ControllerLifecycle, SingleObjectComponentResolvesWithoutDoubleInitOrTerminate)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f, /*singleObjectController=*/true);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    // Preparing the processor initializes the component exactly once.
    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    audient::vst3_test::TestPassthroughComponent* component =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->initializeCount(), 1u);
    EXPECT_EQ(component->terminateCount(), 0u);

    // Resolving the controller must NOT initialize or connect the same object.
    audient::vst3::Vst3Controller controller;
    ASSERT_TRUE(controller.open(processor->component(), &factory, host.hostContext()));
    EXPECT_TRUE(controller.isOpen());
    EXPECT_TRUE(controller.isControllerComponent());
    ASSERT_NE(controller.controller(), nullptr);

    // No double-initialize: the component was prepared once, the controller is
    // the component itself.
    EXPECT_EQ(component->initializeCount(), 1u);
    EXPECT_EQ(component->terminateCount(), 0u);
    EXPECT_EQ(component->connectionPeer(), nullptr) << "single-component plug-in must not be connected to itself";

    // Controller-state persistence through the same object (IEditController
    // getState/setState == component state on a single-component plug-in).
    component->setTestState(0.375f, 7u);
    std::vector<std::uint8_t> blob;
    ASSERT_TRUE(controller.saveControllerState(blob));
    EXPECT_FALSE(blob.empty());

    component->setTestState(0.0f, 0u);
    ASSERT_TRUE(controller.restoreControllerState(blob));
    EXPECT_EQ(component->testStateValue(), 0.375f);
    EXPECT_EQ(component->testStateSeq(), 7u);

    // Corrupt blob is rejected and state stays valid.
    std::vector<std::uint8_t> corrupt = blob;
    corrupt[corrupt.size() - 1] = static_cast<std::uint8_t>(corrupt.back() ^ 0xFF);
    EXPECT_FALSE(controller.restoreControllerState(corrupt));
    EXPECT_EQ(component->testStateValue(), 0.375f);

    // close() must NOT terminate the controller/component object; the component
    // is terminated + released by Vst3Processor::teardown at scope end.
    controller.close();
    EXPECT_FALSE(controller.isOpen());
    EXPECT_EQ(component->terminateCount(), 0u);
    EXPECT_EQ(component->initializeCount(), 1u); // still exactly one initialize
}

// VST-010: separate controller class. Component's getControllerClassId names a
// real controller class; the factory creates it, the host initializes + connects
// it once, and close() terminates + disconnects the controller (never the
// component).
TEST(Vst3ControllerLifecycle, SeparateControllerIsCreatedInitializedConnectedAndTerminatedOnce)
{
    audient::vst3_test::TestPassthroughSeparateControllerFactory factory;
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 2u); // component + controller class from the factory

    // The component class (Audio Effect) is the first one; it returns the
    // controller class id via getControllerClassId.
    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);
    ASSERT_TRUE(processor->valid());

    audient::vst3_test::TestPassthroughComponent* component =
        audient::vst3_test::TestPassthroughComponent::fromFUnknown(processor->component());
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->initializeCount(), 1u);

    audient::vst3::Vst3Controller controller;
    ASSERT_TRUE(controller.open(processor->component(), &factory, host.hostContext()));
    EXPECT_TRUE(controller.isOpen());
    EXPECT_FALSE(controller.isControllerComponent());
    ASSERT_NE(controller.controller(), nullptr);

    // The separate controller was created + initialized exactly once.
    auto* typedController = static_cast<audient::vst3_test::TestPassthroughController*>(controller.controller());
    ASSERT_NE(typedController, nullptr);
    EXPECT_TRUE(typedController->isInitialized());
    EXPECT_EQ(typedController->initializeCount(), 1u);
    EXPECT_EQ(typedController->terminateCount(), 0u);
    EXPECT_EQ(component->initializeCount(), 1u); // component not re-initialized

    // Component <-> controller connected through IConnectionPoint.
    EXPECT_NE(component->connectionPeer(), nullptr);

    // Controller-state persistence round-trips bit-for-bit.
    typedController->setControllerState(0.125f, 42u);
    std::vector<std::uint8_t> blob;
    ASSERT_TRUE(controller.saveControllerState(blob));
    EXPECT_FALSE(blob.empty());

    // Deterministic save: a second save reproduces the same bytes.
    std::vector<std::uint8_t> blob2;
    ASSERT_TRUE(controller.saveControllerState(blob2));
    EXPECT_EQ(blob, blob2);

    typedController->setControllerState(0.0f, 0u);
    ASSERT_TRUE(controller.restoreControllerState(blob));
    EXPECT_EQ(typedController->controllerStateValue(), 0.125f);
    EXPECT_EQ(typedController->controllerStateSeq(), 42u);

    // Corrupt blob is rejected; state remains valid.
    std::vector<std::uint8_t> corrupt = blob;
    corrupt[0] = static_cast<std::uint8_t>(corrupt[0] ^ 0x80);
    EXPECT_FALSE(controller.restoreControllerState(corrupt));
    EXPECT_EQ(typedController->controllerStateValue(), 0.125f);

    // close() terminates + releases the controller, never the component. Hold
    // an extra reference so the object survives close() long enough to read the
    // terminate counter (the host's release() would otherwise delete it).
    typedController->addRef();
    controller.close();
    EXPECT_FALSE(controller.isOpen());
    EXPECT_EQ(typedController->terminateCount(), 1u);
    EXPECT_EQ(component->terminateCount(), 0u);
    EXPECT_EQ(component->initializeCount(), 1u);
    typedController->release();
}

// VST-010: the editor/controller path must tolerate a component with NO
// controller (no IEditController and an empty controller class id) as a clean
// rejection, not a crash.
TEST(Vst3ControllerLifecycle, MissingControllerIsRejectedCleanly)
{
    audient::vst3_test::TestPassthroughFactory factory(0.5f, /*singleObjectController=*/false);
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(&factory));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);

    auto processor = host.createEffectProcessor(classes[0], 48000.0, 64);
    ASSERT_NE(processor, nullptr);

    audient::vst3::Vst3Controller controller;
    EXPECT_FALSE(controller.open(processor->component(), &factory, host.hostContext()));
    EXPECT_FALSE(controller.isOpen());
    EXPECT_FALSE(controller.lastError().empty());
    EXPECT_TRUE(processor->valid()); // processor stays usable
}

} // namespace