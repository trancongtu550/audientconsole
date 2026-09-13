#include "channel/ChannelRack.h"

#include <utility>

namespace audient::channel
{

ChannelRack::ChannelRack()
{
    m_slots.reserve(kMaxSlots);
}

ChannelRackResult ChannelRack::add(std::string name)
{
    Slot slot;
    slot.name = std::move(name);
    return add(std::move(slot));
}

ChannelRackResult ChannelRack::add(Slot slot)
{
    if (full())
    {
        return ChannelRackResult::Full;
    }
    m_slots.push_back(std::move(slot));
    return ChannelRackResult::Ok;
}

ChannelRackResult ChannelRack::removeAt(std::size_t index)
{
    if (!validIndex(index))
    {
        return ChannelRackResult::NotFound;
    }
    m_slots.erase(m_slots.begin() + static_cast<std::ptrdiff_t>(index));
    return ChannelRackResult::Ok;
}

ChannelRackResult ChannelRack::move(std::size_t from, std::size_t to)
{
    if (!validIndex(from) || !validIndex(to))
    {
        return ChannelRackResult::NotFound;
    }
    if (from == to)
    {
        return ChannelRackResult::Invalid;
    }
    Slot slot = std::move(m_slots[from]);
    m_slots.erase(m_slots.begin() + static_cast<std::ptrdiff_t>(from));
    m_slots.insert(m_slots.begin() + static_cast<std::ptrdiff_t>(to), std::move(slot));
    return ChannelRackResult::Ok;
}

ChannelRackResult ChannelRack::setBypass(std::size_t index, bool bypass)
{
    Slot* target = slot(index);
    if (target == nullptr)
    {
        return ChannelRackResult::NotFound;
    }
    target->bypass = bypass;
    return ChannelRackResult::Ok;
}

ChannelRackResult ChannelRack::setMissing(std::size_t index, bool missing)
{
    Slot* target = slot(index);
    if (target == nullptr)
    {
        return ChannelRackResult::NotFound;
    }
    target->missing = missing;
    return ChannelRackResult::Ok;
}

void ChannelRack::setWholeChainBypass(bool bypass)
{
    m_wholeChainBypass = bypass;
}

const ChannelRack::Slot* ChannelRack::slot(std::size_t index) const
{
    if (!validIndex(index))
    {
        return nullptr;
    }
    return &m_slots[index];
}

ChannelRack::Slot* ChannelRack::slot(std::size_t index)
{
    if (!validIndex(index))
    {
        return nullptr;
    }
    return &m_slots[index];
}

void ChannelRack::clear()
{
    m_slots.clear();
    m_wholeChainBypass = false;
}

} // namespace audient::channel
