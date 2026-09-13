#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <cstdint>

namespace audient::vst3_test
{

// Records the inputParameterChanges a plug-in received through process()
// (VST-009 delivery path). Fixed capacity, allocation-free; the recorder is a
// test double, and process() only ever appends/updates on the callback thread.
struct ReceivedParameter
{
    Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
    Steinberg::Vst::ParamValue value = 0.0;
    std::uint32_t points = 0;
};

class TestPassthroughProcessor : public Steinberg::Vst::IAudioProcessor
{
public:
    explicit TestPassthroughProcessor(Steinberg::Vst::IComponent* owner, float gain);

    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    // VST-006: make the plug-in a STEREO-ONLY effect — setBusArrangements
    // rejects any mono request (mirrors FabFilter Pro-Q 4 refusing mono). The
    // host must fall back to stereo and the chain drives it via the mono<->stereo
    // adapter.
    void setStereoOnly(bool enabled = true) { m_stereoOnly = enabled; }

    Steinberg::tresult setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                          Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;
    Steinberg::tresult getBusArrangement(Steinberg::Vst::BusDirection dir, Steinberg::int32 index,
                                         Steinberg::Vst::SpeakerArrangement& arrangement) override;
    Steinberg::tresult canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::uint32 getLatencySamples() override;
    Steinberg::uint32 getTailSamples() override;
    Steinberg::tresult setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult setProcessing(Steinberg::TBool state) override;
    Steinberg::tresult process(Steinberg::Vst::ProcessData& data) override;

    bool stereoArrangement() const;

    // VST-012: setState may override the processing gain; process() then uses
    // the restored value (proves state actually changes parameters).
    void setStateGain(float gain) { m_gain = gain; }

    // VST-016: the plug-in reports a settable latency (default 0) so the host's
    // totalLatencySamples() aggregation and latency-change detection are
    // testable. Mirrors a real plug-in whose latency depends on state/params.
    void setLatencySamples(std::uint32_t samples) { m_latency = samples; }
    std::uint32_t latencyForTest() const { return m_latency; }

    // Last block's received inputParameterChanges snapshot (test observer).
    // Read after the callback thread has stopped.
    int receivedParameterCount() const;
    const ReceivedParameter& receivedParameterAt(int index) const;
    void clearReceivedParameters();

private:
    void recordReceivedParameters(Steinberg::Vst::ProcessData& data);

    Steinberg::Vst::IComponent* m_owner = nullptr;
    float m_gain = 1.0f;
    std::uint32_t m_latency = 0;
    bool m_processing = false;
    bool m_stereo = false;
    bool m_stereoOnly = false;
    static constexpr int kMaxReceived = 16;
    ReceivedParameter m_received[kMaxReceived]{};
    int m_receivedCount = 0;
};

class TestPassthroughComponent : public Steinberg::Vst::IComponent, public Steinberg::Vst::IEditController, public Steinberg::Vst::IConnectionPoint
{
public:
    explicit TestPassthroughComponent(float gain, bool stereoOnly = false);

    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    Steinberg::tresult initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult terminate() override;
    Steinberg::tresult getControllerClassId(Steinberg::TUID classId) override;
    Steinberg::tresult setIoMode(Steinberg::Vst::IoMode mode) override;
    Steinberg::int32 getBusCount(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir) override;
    Steinberg::tresult getBusInfo(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir, Steinberg::int32 index,
                                  Steinberg::Vst::BusInfo& bus) override;
    Steinberg::tresult getRoutingInfo(Steinberg::Vst::RoutingInfo& inInfo, Steinberg::Vst::RoutingInfo& outInfo) override;
    Steinberg::tresult activateBus(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir, Steinberg::int32 index,
                                   Steinberg::TBool state) override;
    Steinberg::tresult setActive(Steinberg::TBool state) override;
    Steinberg::tresult setState(Steinberg::IBStream* state) override;
    Steinberg::tresult getState(Steinberg::IBStream* state) override;

    // --- IEditController (same-object plug-in) ---------------------------
    Steinberg::tresult setComponentState(Steinberg::IBStream* state) override;
    Steinberg::int32 getParameterCount() override;
    Steinberg::tresult getParameterInfo(Steinberg::int32 paramIndex, Steinberg::Vst::ParameterInfo& info) override;
    Steinberg::tresult getParamStringByValue(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized,
                                             Steinberg::Vst::String128 string) override;
    Steinberg::tresult getParamValueByString(Steinberg::Vst::ParamID id, Steinberg::Vst::TChar* string,
                                             Steinberg::Vst::ParamValue& valueNormalized) override;
    Steinberg::Vst::ParamValue normalizedParamToPlain(Steinberg::Vst::ParamID id,
                                                      Steinberg::Vst::ParamValue valueNormalized) override;
    Steinberg::Vst::ParamValue plainParamToNormalized(Steinberg::Vst::ParamID id,
                                                      Steinberg::Vst::ParamValue plainValue) override;
    Steinberg::Vst::ParamValue getParamNormalized(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult setParamNormalized(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override;
    Steinberg::tresult setComponentHandler(Steinberg::Vst::IComponentHandler* handler) override;
    Steinberg::IPlugView* createView(Steinberg::FIDString name) override;

    // --- IConnectionPoint ---------------------------------------------------
    Steinberg::tresult connect(Steinberg::Vst::IConnectionPoint* other) override;
    Steinberg::tresult disconnect(Steinberg::Vst::IConnectionPoint* other) override;
    Steinberg::tresult notify(Steinberg::Vst::IMessage* message) override;

    // Test observer into the owned processor (read after the callback thread
    // has stopped running the plug-in).
    const TestPassthroughProcessor& testProcessor() const { return m_processor; }

    // VST-006: mark the plug-in stereo-only so setBusArrangements rejects mono
    // requests (host must fall back to stereo and adapt the mono mic drive).
    void setStereoOnly(bool enabled = true) { m_processor.setStereoOnly(enabled); }

    // VST-012: state observers. getState/setState round-trip (stateValue,
    // stateSeq, gain) so the host's opaque state blob can be verified
    // bit-for-bit.
    float testStateValue() const { return m_stateValue; }
    std::uint32_t testStateSeq() const { return m_stateSeq; }
    float testGain() const { return m_gain; }
    void setTestState(float stateValue, std::uint32_t stateSeq)
    {
        m_stateValue = stateValue;
        m_stateSeq = stateSeq;
    }

    // VST-016: set the processed latency the plug-in reports via IAudioProcessor::
    // getLatencySamples (used by the host's refreshLatencySamples re-query).
    void setTestLatency(std::uint32_t samples) { m_processor.setLatencySamples(samples); }
    std::uint32_t testLatency() const { return m_processor.latencyForTest(); }

    // IConnectionPoint observer: the peer the host connected (nullptr when the
    // host never connected this component, e.g. single-object plugin lifecycle).
    Steinberg::Vst::IConnectionPoint* connectionPeer() const { return m_connectionPeer; }

    // VST-010 controller lifecycle: this component can act as a single-object
    // (component == controller) plug-in (m_singleObjectController) or as a
    // separate-controller plug-in (m_controllerClassId). Counters expose the
    // shared initialize/terminate counts so tests prove the host never
    // double-initializes or double-terminates the same object.
    Steinberg::uint32 initializeCount() const { return m_initializeCount; }
    Steinberg::uint32 terminateCount() const { return m_terminateCount; }
    void enableSingleObjectController() { m_singleObjectController = true; }
    void setControllerClassId(const Steinberg::TUID& classId);

    // Down-cast helper. The component implements several FUnknown-derived
    // interfaces (IComponent/IEditController/IConnectionPoint), so a raw
    // static_cast from Steinberg::FUnknown* is ambiguous. Resolve through the
    // unique IComponent subobject.
    static TestPassthroughComponent* fromFUnknown(Steinberg::FUnknown* unknown)
    {
        if (unknown == nullptr)
        {
            return nullptr;
        }
        Steinberg::Vst::IComponent* iface = nullptr;
        const Steinberg::tresult result =
            unknown->queryInterface(Steinberg::Vst::IComponent::iid.toTUID(), reinterpret_cast<void**>(&iface));
        if (result != Steinberg::kResultOk || iface == nullptr)
        {
            return nullptr;
        }
        return static_cast<TestPassthroughComponent*>(iface);
    }

private:
    Steinberg::uint32 m_references = 1;
    TestPassthroughProcessor m_processor;
    bool m_active = false;
    bool m_initialized = false;
    bool m_singleObjectController = false;
    float m_gain = 1.0f;
    float m_stateValue = 0.0f;
    std::uint32_t m_stateSeq = 0;
    Steinberg::uint32 m_initializeCount = 0;
    Steinberg::uint32 m_terminateCount = 0;
    Steinberg::TUID m_controllerClassId{};
    Steinberg::Vst::IConnectionPoint* m_connectionPeer = nullptr;
};

class TestPassthroughFactory : public Steinberg::IPluginFactory
{
public:
    // stereoOnly: created components reject any mono setBusArrangements
    // (VST-006 stereo-only effect behavior).
    explicit TestPassthroughFactory(float gain, bool singleObjectController = false, bool stereoOnly = false);

    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    Steinberg::tresult getFactoryInfo(Steinberg::PFactoryInfo* info) override;
    Steinberg::int32 countClasses() override;
    Steinberg::tresult getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info) override;
    Steinberg::tresult createInstance(Steinberg::FIDString cid, Steinberg::FIDString requestId, void** obj) override;

    const Steinberg::FUID& classId() const;

private:
    const Steinberg::FUID m_cid = Steinberg::FUID(0x33663366u, 0x11221122u, 0xAABBCCDDu, 0x01020304u);
    float m_gain = 1.0f;
    bool m_singleObjectController = false;
    bool m_stereoOnly = false;
};

// Separate IEditController for VST-010 separate-controller tests. Standalone
// object (NOT the component): exposes IEditController + IConnectionPoint with
// initialize/terminate counters and a controller-state blob (magic 'TPCT') so
// controller lifecycle + controller-state persistence can be verified in
// isolation from the component.
class TestPassthroughController : public Steinberg::Vst::IEditController, public Steinberg::Vst::IConnectionPoint
{
public:
    TestPassthroughController();

    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    // --- IPluginBase ---
    Steinberg::tresult initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult terminate() override;
    Steinberg::tresult getState(Steinberg::IBStream* state) override;
    Steinberg::tresult setState(Steinberg::IBStream* state) override;

    // --- IEditController ---
    Steinberg::tresult setComponentState(Steinberg::IBStream* state) override;
    Steinberg::int32 getParameterCount() override;
    Steinberg::tresult getParameterInfo(Steinberg::int32 paramIndex, Steinberg::Vst::ParameterInfo& info) override;
    Steinberg::tresult getParamStringByValue(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized,
                                             Steinberg::Vst::String128 string) override;
    Steinberg::tresult getParamValueByString(Steinberg::Vst::ParamID id, Steinberg::Vst::TChar* string,
                                             Steinberg::Vst::ParamValue& valueNormalized) override;
    Steinberg::Vst::ParamValue normalizedParamToPlain(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override;
    Steinberg::Vst::ParamValue plainParamToNormalized(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue plainValue) override;
    Steinberg::Vst::ParamValue getParamNormalized(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult setParamNormalized(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override;
    Steinberg::tresult setComponentHandler(Steinberg::Vst::IComponentHandler* handler) override;
    Steinberg::IPlugView* createView(Steinberg::FIDString name) override;

    // --- IConnectionPoint ---
    Steinberg::tresult connect(Steinberg::Vst::IConnectionPoint* other) override;
    Steinberg::tresult disconnect(Steinberg::Vst::IConnectionPoint* other) override;
    Steinberg::tresult notify(Steinberg::Vst::IMessage* message) override;

    // VST-010 lifecycle observers: exactly-once init/terminate from the host.
    Steinberg::uint32 initializeCount() const { return m_initializeCount; }
    Steinberg::uint32 terminateCount() const { return m_terminateCount; }
    bool isInitialized() const { return m_initialized; }

    // VST-010 controller-state observers (magic 'TPCT' payload round-trip).
    float controllerStateValue() const { return m_stateValue; }
    std::uint32_t controllerStateSeq() const { return m_stateSeq; }
    void setControllerState(float stateValue, std::uint32_t stateSeq)
    {
        m_stateValue = stateValue;
        m_stateSeq = stateSeq;
    }

private:
    Steinberg::uint32 m_references = 1;
    bool m_initialized = false;
    Steinberg::uint32 m_initializeCount = 0;
    Steinberg::uint32 m_terminateCount = 0;
    float m_stateValue = 0.0f;
    std::uint32_t m_stateSeq = 0;
};

// Two-class factory: component class (returns the controller class id via
// getControllerClassId) + separate controller class. VST-010 only; existing
// tests keep the single-class TestPassthroughFactory.
class TestPassthroughSeparateControllerFactory : public Steinberg::IPluginFactory
{
public:
    TestPassthroughSeparateControllerFactory();

    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    Steinberg::tresult getFactoryInfo(Steinberg::PFactoryInfo* info) override;
    Steinberg::int32 countClasses() override;
    Steinberg::tresult getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info) override;
    Steinberg::tresult createInstance(Steinberg::FIDString cid, Steinberg::FIDString requestId, void** obj) override;

    const Steinberg::FUID& componentClassId() const;
    const Steinberg::FUID& controllerClassId() const;

private:
    const Steinberg::FUID m_componentCid = Steinberg::FUID(0x44774477u, 0x22332233u, 0x44556677u, 0x11223344u);
    const Steinberg::FUID m_controllerCid = Steinberg::FUID(0x55885588u, 0x33443344u, 0x55667788u, 0x55667755u);
};

} // namespace audient::vst3_test