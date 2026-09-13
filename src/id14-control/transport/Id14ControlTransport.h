#pragma once

#include "id14-control/semantic-model/Id14SemanticControl.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace audient::id14
{

// Envelope evaluation for a single semantic control write. No raw protocol bytes
// appear in this layer (ADR-005). The transport serializes, correlates ack/timeout,
// and reports a safe result to the caller.
struct ControlWrite
{
    Id14ControlKind kind;
    float value;
    std::uint64_t sequence = 0;
};

enum class TransportResult
{
    Ok,
    NotSupported,   // control is physical-only / read-only / forbidden / unverified
    InvalidValue,   // out of range or off the step grid
    Busy,           // queue bounded and full
    Timeout,
    DeviceMismatch, // identity/firmware tuple not revalidated on reconnect
    Disconnected,
    NotImplemented, // no verified adapter yet
    Other,
};

const char* transportResultName(TransportResult result);

// Callback fired once per write with the correlated sequence number. Runs on the
// transport worker, not the ASIO callback (project guidelines §10.2).
using ControlAckHandler = std::function<void(std::uint64_t sequence, TransportResult result)>;

class Id14ControlTransport
{
public:
    virtual ~Id14ControlTransport() = default;

    Id14ControlTransport(const Id14ControlTransport&) = delete;
    Id14ControlTransport& operator=(const Id14ControlTransport&) = delete;

    // Validate and enqueue a semantic write. Returns Ok when the write was accepted
    // for transmission; the final outcome arrives via onAck.
    virtual TransportResult submit(ControlWrite write, ControlAckHandler onAck) = 0;

    // Register the single-threaded ack sink (may be thread-local in tests).
    virtual void setAckHandler(ControlAckHandler onAck) = 0;

    virtual bool connected() const = 0;
    virtual std::string deviceFingerprint() const = 0;

    virtual void releaseDeviceOwnership(std::string& error) = 0;

protected:
    Id14ControlTransport() = default;
};

} // namespace audient::id14