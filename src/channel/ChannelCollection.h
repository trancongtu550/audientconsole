#pragma once

#include "channel/ChannelIdentity.h"
#include "channel/InputChannel.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace audient::channel
{

// Ordered collection of physical input channels for one session. The active
// count is fixed when the session's channel table is resolved (from the ASIO
// plan); it never changes inside the audio callback. Bounds are enforced with
// exceptions on the control thread only.
class ChannelCollection
{
public:
    explicit ChannelCollection(std::vector<ChannelIdentity> identities);

    InputChannel& channel(std::size_t index);
    const InputChannel& channel(std::size_t index) const;

    InputChannel& at(std::size_t index) { return channel(index); }
    const InputChannel& at(std::size_t index) const { return channel(index); }

    std::size_t activeCount() const { return m_channels.size(); }
    bool empty() const { return m_channels.empty(); }

private:
    void checkIndex(std::size_t index) const;

    std::vector<InputChannel> m_channels;
};

} // namespace audient::channel
