#include "channel/InputChannel.h"

#include <utility>

namespace audient::channel
{

InputChannel::InputChannel(ChannelIdentity identity)
    : m_identity(std::move(identity))
    , m_runtimeSnapshot(std::make_unique<engine::SeqLockSnapshot<ChannelRuntimeSnapshot>>())
{
    m_runtimeSnapshot->publish({m_runtime.virtualMicSend, m_runtime.localMonitorSend, m_runtime.revision});
}

InputChannel::InputChannel(InputChannel&& other) noexcept
    : m_identity(std::move(other.m_identity))
    , m_runtime(std::move(other.m_runtime))
    , m_runtimeSnapshot(std::move(other.m_runtimeSnapshot))
    , m_rack(std::move(other.m_rack))
{
}

InputChannel& InputChannel::operator=(InputChannel&& other) noexcept
{
    if (this != &other)
    {
        m_identity = std::move(other.m_identity);
        m_runtime = std::move(other.m_runtime);
        m_runtimeSnapshot = std::move(other.m_runtimeSnapshot);
        m_rack = std::move(other.m_rack);
    }
    return *this;
}

void InputChannel::mutateRuntime(const std::function<void(ChannelRuntimeConfig&)>& mutate)
{
    if (mutate)
    {
        mutate(m_runtime);
        ++m_runtime.revision;
        m_runtimeSnapshot->publish({m_runtime.virtualMicSend, m_runtime.localMonitorSend, m_runtime.revision});
    }
}

void InputChannel::setVirtualMicSend(const ChannelSendState& send)
{
    m_runtime.virtualMicSend = send;
    ++m_runtime.revision;
    m_runtimeSnapshot->publish({m_runtime.virtualMicSend, m_runtime.localMonitorSend, m_runtime.revision});
}

void InputChannel::setLocalMonitorSend(const ChannelSendState& send)
{
    m_runtime.localMonitorSend = send;
    ++m_runtime.revision;
    m_runtimeSnapshot->publish({m_runtime.virtualMicSend, m_runtime.localMonitorSend, m_runtime.revision});
}

} // namespace audient::channel
