#pragma once

#include "channel/ChannelIdentity.h"
#include "channel/ChannelRack.h"
#include "channel/ChannelTypes.h"
#include "engine/GraphConfigSnapshot.h"

#include <functional>
#include <memory>
#include <string>

namespace audient::channel
{

// One physical input channel: stable identity, independent per-channel runtime
// config (virtual-mic + local-monitor sends), and an independent ordered rack.
// Control/UI thread object only; the audio path never touches this class
// directly (Phase B wire-up publishes a snapshot per channel and binds a
// Vst3Chain per channel on a later checkpoint).
class InputChannel
{
public:
    explicit InputChannel(ChannelIdentity identity);
    ~InputChannel() = default;

    InputChannel(const InputChannel&) = delete;
    InputChannel& operator=(const InputChannel&) = delete;
    InputChannel(InputChannel&&) noexcept;
    InputChannel& operator=(InputChannel&&) noexcept;

    const ChannelIdentity& identity() const { return m_identity; }
    const std::string& channelName() const { return m_identity.stableName; }

    // Applies a mutation to THIS channel's runtime config and bumps ONLY this
    // channel's revision. Other channels are never touched.
    void mutateRuntime(const std::function<void(ChannelRuntimeConfig&)>& mutate);

    void setVirtualMicSend(const ChannelSendState& send);
    void setLocalMonitorSend(const ChannelSendState& send);

    const ChannelSendState& virtualMicSend() const { return m_runtime.virtualMicSend; }
    const ChannelSendState& localMonitorSend() const { return m_runtime.localMonitorSend; }
    const ChannelRuntimeConfig& runtimeConfig() const { return m_runtime; }
    ChannelRuntimeSnapshot runtimeSnapshot() const { return m_runtimeSnapshot->read(); }
    std::uint64_t revision() const { return m_runtime.revision; }

    ChannelRack& rack() { return m_rack; }
    const ChannelRack& rack() const { return m_rack; }

private:
    ChannelIdentity m_identity;
    ChannelRuntimeConfig m_runtime;
    std::unique_ptr<engine::SeqLockSnapshot<ChannelRuntimeSnapshot>> m_runtimeSnapshot;
    ChannelRack m_rack;
};

} // namespace audient::channel
