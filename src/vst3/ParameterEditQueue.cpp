#include "vst3/ParameterEditQueue.h"

#include <cassert>

namespace audient::vst3
{

ParameterEditQueue::ParameterEditQueue(std::size_t capacity)
    : m_capacity(capacity)
    , m_mask(capacity - 1)
    , m_memory(new Edit[capacity])
{
    // Capacity is a power of two so the ring index is a cheap mask.
    assert(capacity > 0);
    assert((capacity & (capacity - 1)) == 0);
}

bool ParameterEditQueue::push(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
{
    const std::size_t tail = m_tail.load(std::memory_order_relaxed);
    const std::size_t head = m_head.load(std::memory_order_acquire);
    if (tail - head >= m_capacity)
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m_memory[tail & m_mask] = Edit{id, value};
    m_tail.store(tail + 1, std::memory_order_release);
    m_pushed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::size_t ParameterEditQueue::drain(Edit* out, std::size_t maxEdits)
{
    if (out == nullptr || maxEdits == 0)
    {
        return 0;
    }
    const std::size_t head = m_head.load(std::memory_order_relaxed);
    const std::size_t tail = m_tail.load(std::memory_order_acquire);
    std::size_t avail = tail - head;
    if (avail > maxEdits)
    {
        avail = maxEdits;
    }
    for (std::size_t i = 0; i < avail; ++i)
    {
        out[i] = m_memory[(head + i) & m_mask];
    }
    m_head.store(head + avail, std::memory_order_release);
    return avail;
}

std::size_t ParameterEditQueue::pending() const
{
    return m_tail.load(std::memory_order_acquire) - m_head.load(std::memory_order_acquire);
}

void ParameterEditQueue::reset()
{
    m_head.store(0, std::memory_order_release);
    m_tail.store(0, std::memory_order_release);
    m_dropped.store(0, std::memory_order_relaxed);
    m_pushed.store(0, std::memory_order_relaxed);
}

} // namespace audient::vst3