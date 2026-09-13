#include "asio/AsioDeviceState.h"

namespace audient::asio
{

DeviceState DeviceStateMachine::current() const
{
    return m_state.load(std::memory_order_relaxed);
}

bool DeviceStateMachine::isLegal(DeviceState from, DeviceState to)
{
    switch (from)
    {
    case DeviceState::Uninitialized:
        return to == DeviceState::Idle || to == DeviceState::NoDevice;
    case DeviceState::NoDevice:
        return to == DeviceState::Idle || to == DeviceState::Opening || to == DeviceState::Error;
    case DeviceState::Idle:
        return to == DeviceState::Opening || to == DeviceState::NoDevice || to == DeviceState::Error;
    case DeviceState::Opening:
        return to == DeviceState::Ready || to == DeviceState::Idle || to == DeviceState::Error || to == DeviceState::Reconnecting;
    case DeviceState::Ready:
        return to == DeviceState::Starting || to == DeviceState::Idle || to == DeviceState::NoDevice ||
               to == DeviceState::Error || to == DeviceState::Reconnecting;
    case DeviceState::Starting:
        return to == DeviceState::Streaming || to == DeviceState::Ready || to == DeviceState::Error || to == DeviceState::Reconnecting;
    case DeviceState::Streaming:
        return to == DeviceState::Stopping || to == DeviceState::Reconnecting || to == DeviceState::Error ||
               to == DeviceState::Idle;
    case DeviceState::Stopping:
        return to == DeviceState::Streaming || to == DeviceState::Ready || to == DeviceState::Idle ||
               to == DeviceState::Reconnecting || to == DeviceState::Error;
    case DeviceState::Reconnecting:
        return to == DeviceState::Opening || to == DeviceState::Ready || to == DeviceState::Idle ||
               to == DeviceState::Error || to == DeviceState::NoDevice;
    case DeviceState::Error:
        return to == DeviceState::Idle || to == DeviceState::NoDevice || to == DeviceState::Reconnecting || to == DeviceState::Opening;
    default:
        return false;
    }
}

bool DeviceStateMachine::transition(DeviceState from, DeviceState to)
{
    if (!isLegal(from, to))
    {
        return false;
    }
    return m_state.compare_exchange_strong(from, to, std::memory_order_acq_rel, std::memory_order_relaxed);
}

bool DeviceStateMachine::moveTo(DeviceState to)
{
    const DeviceState from = m_state.load(std::memory_order_relaxed);
    return transition(from, to);
}

void DeviceStateMachine::force(DeviceState state)
{
    m_state.store(state, std::memory_order_release);
}

} // namespace audient::asio