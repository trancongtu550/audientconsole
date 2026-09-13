#pragma once

#include "id14-control/transport/Id14ControlTransport.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace audient::id14
{

// Deterministic in-process transport for tests and Phase 3 model work. It never
// touches the real iD14 or emits protocol bytes. It records submitted writes and
// answers with the configured result so the semantic layer can be verified before
// any verified adapter exists. Handles are replaced by a bounded queue internally.
class SimulatedId14Transport : public Id14ControlTransport
{
public:
    SimulatedId14Transport();

    TransportResult submit(ControlWrite write, ControlAckHandler onAck) override;
    void setAckHandler(ControlAckHandler onAck) override;
    bool connected() const override;
    std::string deviceFingerprint() const override;
    void releaseDeviceOwnership(std::string& error) override;

    void simulateDisconnect();
    void simulateReconnect(const std::string& fingerprint);
    void setNextResult(TransportResult result);
    void drain(std::size_t maxWrites, std::uint64_t& processed);

    std::size_t submittedCount() const;
    std::map<Id14ControlKind, float> lastValuesByKind() const;
    ControlWrite lastWrite() const;
    bool ackHandlerPending() const;

private:
    mutable std::mutex m_mutex;
    bool m_connected = true;
    std::string m_fingerprint = "sim-Audient-iD14-MK1";
    TransportResult m_nextResult = TransportResult::Ok;
    std::size_t m_submitted = 0;
    std::size_t m_queueFront = 0;
    std::map<std::uint64_t, ControlWrite> m_pending;
    std::map<Id14ControlKind, float> m_lastValues;
    ControlWrite m_lastWrite{};
    bool m_hasLastWrite = false;
    ControlAckHandler m_handler;
};

} // namespace audient::id14