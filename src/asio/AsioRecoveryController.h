#pragma once

// AsioRecoveryController - app-side device-loss detection + automatic recovery
// for the Audient ASIO physical leg (project guidelines §8, Q5-A4 recovery gap).
//
// The real ASIO driver does not emit a reliable explicit device-lost event when
// the USB interface disappears; the AudioEngine simply stops delivering buffer
// switches. This controller therefore detects device loss from a CALLBACK STALL
// fenced by a non-realtime control thread (the driver callback never polls).
//
//   STREAMING: drive tick() periodically; when `callbackCount()` does not advance
//              for `stallMs`, the controller declares the physical device LOST,
//              closes the dead stream via IRecoverableAsioStream::closeStream(),
//              and freezes the UI/status meter to silence/disconnected.
//   LOST:      every `retryMs` (no busy-spin) it calls
//              IRecoverableAsioStream::openAndStart() to create a FRESH ASIO
//              session. The old stream is NEVER resumed (it is dead); each open
//              builds a new driver instance + stream. On success the state returns
//              to STREAMING with a clean transport epoch (no stale pre-loss audio).
//
// Threshold rationale (defaults; ASIO buffer 64 @48 kHz -> 1.333 ms callback
// period):
//   - stallMs = 150 ms: ~112 nominal callback periods and >10x the 10 ms
//     engine/WASAPI period, so ordinary scheduling/DPC jitter can never trip it;
//     far smaller than the ~5 s minimum disconnect the acceptance uses.
//   - retryMs = 750 ms: within the required 500 ms-1 s reconnect cadence.
//
// Threading: the controller performs NO realtime work. It is driven from the
// control thread (the demo's watchdog thread); openAndStart()/closeStream() are
// non-realtime (allocation, driver open, buffer build) by design.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace audient::asio
{

class IRecoverableAsioStream
{
public:
    virtual ~IRecoverableAsioStream() = default;

    // Current ASIO callback count (0 while no stream is live). Called from the
    // control thread only.
    virtual std::uint64_t callbackCount() = 0;

    // Build a FRESH stream/session and start it. The previous stream (if any)
    // must already be closed and is never resumed. Realtime thread untouched.
    virtual bool openAndStart(std::string& error) = 0;

    // Tear down the (dead) stream outside the audio callback. Must not call back
    // into a confirmed-dead driver in a way that can block indefinitely.
    virtual void closeStream() = 0;
};

enum class PhysDeviceState
{
    Streaming, // callbacks arriving (or stream just (re)started)
    Lost,      // physical device lost; stream closed; awaiting/recovering
};

class AsioRecoveryController
{
public:
    struct Params
    {
        std::chrono::milliseconds stallMs{150}; // callback-stall loss threshold
        std::chrono::milliseconds retryMs{750}; // recovery retry cadence (no spin)
    };

    explicit AsioRecoveryController(IRecoverableAsioStream& stream, Params params = {});

    // Re-baseline the stall detector for a healthy live stream (startup or after
    // a successful recovery). Call with the wall clock the tick() loop uses.
    void markStreaming(std::chrono::steady_clock::time_point now);

    // Force a loss event (test/diagnostic only; e.g. --simulate-loss-after).
    void simulateLoss(std::chrono::steady_clock::time_point now);

    // One watchdog step. Call from a non-realtime control thread at <=tick cadence.
    // May invoke openAndStart()/closeStream().
    void tick(std::chrono::steady_clock::time_point now);

    // Planned reconfiguration (e.g. the user chose a new ASIO buffer size):
    // tear the live stream down on the control thread and let the SAME recovery
    // loop rebuild it. Counted separately from a genuine device-loss event so
    // the two can never be conflated in diagnostics.
    void requestReconfigure(std::chrono::steady_clock::time_point now);

    PhysDeviceState state() const;
    std::uint64_t lostCount() const;
    std::uint64_t recoveryCount() const;
    std::uint64_t reconfigureCount() const;
    std::uint64_t failedAttempts() const;

    struct Events
    {
        bool lost = false;
        bool recovered = false;
    };
    // One-shot transition flags (for status/logging), cleared on read.
    Events consumeEvents();

    void requestStop();
    bool stopRequested() const;

private:
    void enterLost(std::chrono::steady_clock::time_point now);

    IRecoverableAsioStream& m_stream;
    Params m_params;

    std::atomic<PhysDeviceState> m_state{PhysDeviceState::Streaming};
    std::atomic<std::uint64_t> m_lostCount{0};
    std::atomic<std::uint64_t> m_recoveryCount{0};
    std::atomic<std::uint64_t> m_reconfigureCount{0};
    std::atomic<std::uint64_t> m_failedAttempts{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_evtLost{false};
    std::atomic<bool> m_evtRecovered{false};

    std::uint64_t m_lastCb = 0;
    bool m_haveBaseline = false;
    std::chrono::steady_clock::time_point m_idleSince{};
    bool m_idleArmed = false;
    std::chrono::steady_clock::time_point m_lastAttempt{};
};

// --- inline implementation (single translation unit, header-only) --------------

inline AsioRecoveryController::AsioRecoveryController(IRecoverableAsioStream& stream, Params params)
    : m_stream(stream), m_params(params)
{
}

inline void AsioRecoveryController::markStreaming(std::chrono::steady_clock::time_point /*now*/)
{
    const PhysDeviceState before = m_state.exchange(PhysDeviceState::Streaming);
    if (before == PhysDeviceState::Lost)
    {
        ++m_recoveryCount;
        m_evtRecovered.store(true, std::memory_order_release);
    }
    m_haveBaseline = false;
    m_idleArmed = false;
}

inline void AsioRecoveryController::enterLost(std::chrono::steady_clock::time_point now)
{
    // Tear the dead stream down on the CONTROL thread (outside the audio path).
    m_stream.closeStream();
    m_state.store(PhysDeviceState::Lost);
    ++m_lostCount;
    m_evtLost.store(true, std::memory_order_release);
    m_lastAttempt = now;
    m_idleArmed = false;
}

inline void AsioRecoveryController::simulateLoss(std::chrono::steady_clock::time_point now)
{
    if (m_state.load() == PhysDeviceState::Streaming)
    {
        enterLost(now);
    }
}

inline void AsioRecoveryController::requestReconfigure(std::chrono::steady_clock::time_point now)
{
    if (m_state.load() != PhysDeviceState::Streaming)
    {
        return;
    }
    // Tear the live stream down on the CONTROL thread (the caller has already
    // performed a graceful fade+stop where the stream was still healthy). Enter
    // Lost so the existing recovery retry loop rebuilds a fresh session from the
    // staged configuration. No lost event and no lostCount bump: this is planned.
    m_stream.closeStream();
    m_state.store(PhysDeviceState::Lost);
    ++m_reconfigureCount;
    m_lastAttempt = now;
    m_idleArmed = false;
}

inline void AsioRecoveryController::tick(std::chrono::steady_clock::time_point now)
{
    if (m_stop.load(std::memory_order_relaxed))
    {
        return;
    }

    if (m_state.load() == PhysDeviceState::Streaming)
    {
        const std::uint64_t cb = m_stream.callbackCount();
        if (!m_haveBaseline)
        {
            m_lastCb = cb;
            m_haveBaseline = true;
            m_idleArmed = false;
            return;
        }
        if (cb == m_lastCb)
        {
            if (!m_idleArmed)
            {
                m_idleArmed = true;
                m_idleSince = now;
            }
            else if (now - m_idleSince >= m_params.stallMs)
            {
                enterLost(now);
            }
        }
        else
        {
            m_lastCb = cb;
            m_idleArmed = false;
        }
        return;
    }

    // Lost: retry at `retryMs` cadence (no busy-spin).
    if (now - m_lastAttempt < m_params.retryMs)
    {
        return;
    }
    m_lastAttempt = now;

    std::string error;
    if (m_stream.openAndStart(error))
    {
        markStreaming(now);
    }
    else
    {
        ++m_failedAttempts;
    }
}

inline PhysDeviceState AsioRecoveryController::state() const
{
    return m_state.load();
}

inline std::uint64_t AsioRecoveryController::lostCount() const
{
    return m_lostCount.load();
}

inline std::uint64_t AsioRecoveryController::recoveryCount() const
{
    return m_recoveryCount.load();
}

inline std::uint64_t AsioRecoveryController::reconfigureCount() const
{
    return m_reconfigureCount.load();
}

inline std::uint64_t AsioRecoveryController::failedAttempts() const
{
    return m_failedAttempts.load();
}

inline AsioRecoveryController::Events AsioRecoveryController::consumeEvents()
{
    Events events;
    events.lost = m_evtLost.exchange(false);
    events.recovered = m_evtRecovered.exchange(false);
    return events;
}

inline void AsioRecoveryController::requestStop()
{
    m_stop.store(true, std::memory_order_relaxed);
}

inline bool AsioRecoveryController::stopRequested() const
{
    return m_stop.load(std::memory_order_relaxed);
}

} // namespace audient::asio