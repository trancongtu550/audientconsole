#pragma once

#include "pluginterfaces/vst/vsttypes.h"

namespace audient::vst3
{

// Target that receives parameter edits sourced from a plug-in's controller /
// editor UI (IComponentHandler::performEdit). Implemented by Vst3Processor so
// host-context edits are delivered through the realtime ParameterEditQueue.
class IParameterEditSink
{
public:
    virtual ~IParameterEditSink() = default;

    // Called by performEdit on the controller thread; forwards into the
    // processor's bounded queue. Must be non-blocking and never touch audio.
    virtual void onParameterEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) = 0;
};

} // namespace audient::vst3