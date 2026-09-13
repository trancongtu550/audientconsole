#include "channel/ChannelCollection.h"

namespace audient::channel
{

ChannelCollection::ChannelCollection(std::vector<ChannelIdentity> identities)
{
    if (identities.empty() || identities.size() > kMaxPhysicalInputChannels)
    {
        throw std::invalid_argument("channel collection size out of range");
    }
    m_channels.reserve(identities.size());
    for (ChannelIdentity& identity : identities)
    {
        if (!identity.valid())
        {
            throw std::invalid_argument("channel collection contains an invalid identity");
        }
        m_channels.emplace_back(identity);
    }
}

InputChannel& ChannelCollection::channel(std::size_t index)
{
    checkIndex(index);
    return m_channels[index];
}

const InputChannel& ChannelCollection::channel(std::size_t index) const
{
    checkIndex(index);
    return m_channels[index];
}

void ChannelCollection::checkIndex(std::size_t index) const
{
    if (index >= m_channels.size())
    {
        throw std::out_of_range("channel index out of range");
    }
}

} // namespace audient::channel
