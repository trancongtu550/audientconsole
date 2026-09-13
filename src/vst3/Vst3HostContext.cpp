#include "vst3/Vst3HostContext.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ustring.h"

#include <cstring>

namespace audient::vst3
{

Vst3HostContext::Vst3HostContext() = default;

Steinberg::tresult Vst3HostContext::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    using namespace Steinberg;
    if (obj == nullptr)
    {
        return kInvalidArgument;
    }

    if (FUnknownPrivate::iidEqual(requestId, Steinberg::Vst::IHostApplication::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
        addRef();
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(requestId, Steinberg::Vst::IComponentHandler::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
        addRef();
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(requestId, FUnknown::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
        addRef();
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

Steinberg::uint32 Vst3HostContext::addRef()
{
    return ++m_references;
}

Steinberg::uint32 Vst3HostContext::release()
{
    if (--m_references == 0)
    {
        delete this;
        return 0;
    }
    return m_references;
}

Steinberg::tresult Vst3HostContext::getName(Steinberg::Vst::String128 name)
{
    Steinberg::UString128("Audient Console").copyTo(name, 128);
    return Steinberg::kResultOk;
}

Steinberg::tresult Vst3HostContext::createInstance(Steinberg::TUID cid, Steinberg::TUID requestedIid, void** obj)
{
    (void)cid;
    (void)requestedIid;
    if (obj == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    // v1 host does not provide host-side objects (IMessage etc.) yet.
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::tresult Vst3HostContext::beginEdit(Steinberg::Vst::ParamID id)
{
    (void)id;
    return Steinberg::kResultOk;
}

Steinberg::tresult Vst3HostContext::performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized)
{
    if (m_parameterEditSink != nullptr)
    {
        m_parameterEditSink->onParameterEdit(id, valueNormalized);
    }
    return Steinberg::kResultOk;
}

Steinberg::tresult Vst3HostContext::endEdit(Steinberg::Vst::ParamID id)
{
    (void)id;
    return Steinberg::kResultOk;
}

Steinberg::tresult Vst3HostContext::restartComponent(Steinberg::int32 flags)
{
    if ((flags & Steinberg::Vst::kLatencyChanged) != 0)
    {
        m_latencyChangedPending.store(true, std::memory_order_release);
    }
    return Steinberg::kResultOk;
}

bool Vst3HostContext::hasPendingLatencyChanged() const
{
    return m_latencyChangedPending.load(std::memory_order_acquire);
}

bool Vst3HostContext::consumeLatencyChanged()
{
    return m_latencyChangedPending.exchange(false, std::memory_order_acq_rel);
}

void Vst3HostContext::setParameterEditSink(IParameterEditSink* sink)
{
    m_parameterEditSink = sink;
}

} // namespace audient::vst3