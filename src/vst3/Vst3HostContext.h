#pragma once

#include "vst3/IParameterEditSink.h"

#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"

#include <atomic>

namespace audient::vst3
{

// Host-side context handed to plug-ins in IComponent::initialize(hostContext).
// Implements the minimum mandatory host contract for v1 effects (project guidelines §9,
// VST-009): an IHostApplication plus an IComponentHandler that routes
// parameter edits into the owning processor's bounded realtime queue.
//
// Realtime contract: beginEdit/performEdit/endEdit are called by the plug-in
// controller (UI/controller thread), never the ASIO callback. The sink
// enqueues into the processor's queue; the callback drains it.
class Vst3HostContext : public Steinberg::Vst::IHostApplication,
                        public Steinberg::Vst::IComponentHandler
{
public:
    Vst3HostContext();

    // FUnknown
    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    // IHostApplication
    Steinberg::tresult getName(Steinberg::Vst::String128 name) override;
    Steinberg::tresult createInstance(Steinberg::TUID cid, Steinberg::TUID requestedIid, void** obj) override;

    // IComponentHandler (VST-009: routes edits into the sink's realtime queue)
    Steinberg::tresult beginEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override;
    Steinberg::tresult endEdit(Steinberg::Vst::ParamID id) override;
    Steinberg::tresult restartComponent(Steinberg::int32 flags) override;

    void setParameterEditSink(IParameterEditSink* sink);

    // VST-016: IComponentHandler::restartComponent records whether the plug-in
    // signalled kLatencyChanged (spec: a plug-in whose reported latency changed
    // must call restartComponent(kLatencyChanged); the host then re-queries
    // getLatencySamples and rebuilds its delay compensation on the control
    // thread). consumeLatencyChanged() reads and clears the flag.
    bool hasPendingLatencyChanged() const;
    bool consumeLatencyChanged();

private:
    IParameterEditSink* m_parameterEditSink = nullptr;
    std::atomic<Steinberg::uint32> m_references{1};
    std::atomic<bool> m_latencyChangedPending{false};
};

} // namespace audient::vst3