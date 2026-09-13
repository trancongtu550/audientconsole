#pragma once

#include <atomic>

namespace audient::asio
{

enum class DeviceState
{
    Uninitialized,
    NoDevice,
    Idle,
    Opening,
    Ready,
    Starting,
    Streaming,
    Stopping,
    Reconnecting,
    Error,
};

class DeviceStateMachine
{
public:
    DeviceState current() const;

    bool transition(DeviceState from, DeviceState to);
    bool moveTo(DeviceState to);
    void force(DeviceState state);

    static bool isLegal(DeviceState from, DeviceState to);

private:
    std::atomic<DeviceState> m_state{DeviceState::Uninitialized};
};

} // namespace audient::asio