#include "id14-control/simulator/SimulatedId14Transport.h"

#include <algorithm>

namespace audient::id14
{

SimulatedId14Transport::SimulatedId14Transport() = default;

TransportResult SimulatedId14Transport::submit(ControlWrite write, ControlAckHandler onAck)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected)
    {
        return TransportResult::Disconnected;
    }

    const std::uint64_t sequence = write.sequence == 0 ? m_submitted + 1 : write.sequence;
    write.sequence = sequence;
    m_submitted++;
    m_pending[sequence] = write;
    m_lastWrite = write;
    m_hasLastWrite = true;

    ControlAckHandler handler = onAck ? onAck : m_handler;
    if (m_nextResult == TransportResult::Ok)
    {
        m_lastValues[write.kind] = write.value;
    }
    const TransportResult result = m_nextResult;
    m_pending.erase(sequence);
    if (handler)
    {
        handler(sequence, result);
    }
    return result;
}

void SimulatedId14Transport::setAckHandler(ControlAckHandler onAck)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handler = std::move(onAck);
}

bool SimulatedId14Transport::connected() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected;
}

std::string SimulatedId14Transport::deviceFingerprint() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fingerprint;
}

void SimulatedId14Transport::releaseDeviceOwnership(std::string& error)
{
    (void)error;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = false;
    m_pending.clear();
    m_fingerprint.clear();
    m_lastValues.clear();
}

void SimulatedId14Transport::simulateDisconnect()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = false;
}

void SimulatedId14Transport::simulateReconnect(const std::string& fingerprint)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = true;
    if (!fingerprint.empty())
    {
        m_fingerprint = fingerprint;
    }
    m_pending.clear();
}

void SimulatedId14Transport::setNextResult(TransportResult result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nextResult = result;
}

void SimulatedId14Transport::drain(std::size_t maxWrites, std::uint64_t& processed)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t count = 0;
    for (auto it = m_pending.begin(); it != m_pending.end();)
    {
        if (maxWrites > 0 && count >= maxWrites)
        {
            break;
        }
        it = m_pending.erase(it);
        count++;
        processed++;
    }
}

std::size_t SimulatedId14Transport::submittedCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_submitted;
}

std::map<Id14ControlKind, float> SimulatedId14Transport::lastValuesByKind() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastValues;
}

ControlWrite SimulatedId14Transport::lastWrite() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastWrite;
}

bool SimulatedId14Transport::ackHandlerPending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<bool>(m_handler);
}

} // namespace audient::id14