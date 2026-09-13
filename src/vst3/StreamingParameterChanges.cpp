#include "vst3/StreamingParameterChanges.h"

#include "pluginterfaces/base/funknown.h"

#include <cstring>

namespace audient::vst3
{

StreamingParameterChanges::StreamingParameterChanges() = default;

StreamingParameterChanges::~StreamingParameterChanges() = default;

Steinberg::int32 StreamingParameterChanges::getParameterCount()
{
    return m_count;
}

Steinberg::Vst::IParamValueQueue* StreamingParameterChanges::getParameterData(Steinberg::int32 index)
{
    if (index < 0 || index >= m_count)
    {
        return nullptr;
    }
    return &m_queues[index];
}

Steinberg::Vst::IParamValueQueue* StreamingParameterChanges::addParameterData(const Steinberg::Vst::ParamID& id,
                                                                              Steinberg::int32& index)
{
    // VST3 allows the host to add multiple queues for the same id; we reuse an
    // existing matching queue in insertion order so duplicate UI edits coalesce.
    for (Steinberg::int32 i = 0; i < m_count; ++i)
    {
        if (m_queues[i].id() == id)
        {
            index = i;
            return &m_queues[i];
        }
    }
    if (m_count >= kMaxQueues)
    {
        index = -1;
        return nullptr; // pool exhausted; defined overflow, never grow
    }
    m_queues[m_count].init(id);
    index = m_count;
    ++m_count;
    return &m_queues[m_count - 1];
}

Steinberg::tresult StreamingParameterChanges::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (obj == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(requestId, Steinberg::Vst::IParameterChanges::iid.toTUID()) ||
        Steinberg::FUnknownPrivate::iidEqual(requestId, Steinberg::FUnknown::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IParameterChanges*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 StreamingParameterChanges::addRef()
{
    return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

Steinberg::uint32 StreamingParameterChanges::release()
{
    // Owned by Vst3Processor for its lifetime; released weightfully tracked but
    // never deleted here (the object is a member, not heap-allocated).
    const std::uint32_t previous = m_refs.fetch_sub(1, std::memory_order_relaxed);
    if (previous == 1)
    {
        return 0;
    }
    return previous - 1;
}

void StreamingParameterChanges::reset()
{
    for (Steinberg::int32 i = 0; i < m_count; ++i)
    {
        m_queues[i].clear();
    }
    m_count = 0;
}

bool StreamingParameterChanges::addPoint(const Steinberg::Vst::ParamID& id, Steinberg::int32 sampleOffset,
                                         Steinberg::Vst::ParamValue value)
{
    for (Steinberg::int32 i = 0; i < m_count; ++i)
    {
        if (m_queues[i].id() == id)
        {
            return m_queues[i].add(sampleOffset, value);
        }
    }
    if (m_count >= kMaxQueues)
    {
        return false;
    }
    m_queues[m_count].init(id);
    const bool ok = m_queues[m_count].add(sampleOffset, value);
    ++m_count;
    return ok;
}

Steinberg::int32 StreamingParameterChanges::pointCount(const Steinberg::Vst::ParamID& id) const
{
    for (Steinberg::int32 i = 0; i < m_count; ++i)
    {
        if (m_queues[i].id() == id)
        {
            return m_queues[i].count();
        }
    }
    return 0;
}

// ---------------- Queue ----------------

Steinberg::Vst::ParamID StreamingParameterChanges::Queue::getParameterId()
{
    return m_id;
}

Steinberg::int32 StreamingParameterChanges::Queue::getPointCount()
{
    return m_count;
}

Steinberg::tresult StreamingParameterChanges::Queue::getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset,
                                                              Steinberg::Vst::ParamValue& value)
{
    if (index < 0 || index >= m_count)
    {
        return Steinberg::kInvalidArgument;
    }
    sampleOffset = m_offsets[index];
    value = m_values[index];
    return Steinberg::kResultOk;
}

Steinberg::tresult StreamingParameterChanges::Queue::addPoint(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value,
                                                              Steinberg::int32& index)
{
    if (m_count >= kMaxPoints)
    {
        index = -1;
        return Steinberg::kResultFalse;
    }
    index = m_count;
    m_offsets[m_count] = sampleOffset;
    m_values[m_count] = value;
    ++m_count;
    return Steinberg::kResultOk;
}

Steinberg::tresult StreamingParameterChanges::Queue::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (obj == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(requestId, Steinberg::Vst::IParamValueQueue::iid.toTUID()) ||
        Steinberg::FUnknownPrivate::iidEqual(requestId, Steinberg::FUnknown::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IParamValueQueue*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 StreamingParameterChanges::Queue::addRef()
{
    return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

Steinberg::uint32 StreamingParameterChanges::Queue::release()
{
    const std::uint32_t previous = m_refs.fetch_sub(1, std::memory_order_relaxed);
    if (previous == 1)
    {
        return 0;
    }
    return previous - 1;
}

bool StreamingParameterChanges::Queue::init(const Steinberg::Vst::ParamID& id)
{
    clear();
    m_id = id;
    return true;
}

bool StreamingParameterChanges::Queue::add(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value)
{
    if (m_count >= kMaxPoints)
    {
        return false;
    }
    m_offsets[m_count] = sampleOffset;
    m_values[m_count] = value;
    ++m_count;
    return true;
}

void StreamingParameterChanges::Queue::clear()
{
    m_id = Steinberg::Vst::kNoParamId;
    m_count = 0;
}

} // namespace audient::vst3