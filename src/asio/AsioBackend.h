#pragma once

#include "asio/AsioDeviceState.h"
#include "asio/AsioTypes.h"
#include "asio/AsioChannelMap.h"
#include "asio/AudientDriverMatcher.h"
#include "asio/IAsioDriver.h"
#include "engine/Counters.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace audient::asio
{

class AsioBackend
{
public:
    explicit AsioBackend(IAsioDriverProvider& provider);

    std::vector<DriverIdentity> availableDevices() const;

    bool selectAudientDevice(std::string& error);
    bool selectDevice(const DriverIdentity& identity, std::string& error);
    bool configure(long sampleRate, long bufferSamples, const ChannelPlan& plan, std::string& error);

    void setStreamProcessor(AsioCallbackFn processor, void* context);
    bool start(std::string& error);
    bool stop(std::string& error);
    bool requestReset(std::string& error);
    bool changeSampleRate(long sampleRate, std::string& error);

    void panic();
    void clearPanic();
    void handleDisconnect();
    void handleReconnect();

    DeviceState state() const;
    const DriverCapabilities& driverCapabilities() const;
    const ChannelPlan& activePlan() const;

    // Actual sample rate currently reported by the open driver (Hz); false when
    // no driver is open or it cannot report one.
    bool queryDriverSampleRate(long& sampleRate) const;

    // Actual buffer size currently reported by the open driver (frames),
    // including a driver-notified external change. False when unavailable.
    bool queryDriverBufferSize(long& bufferSamples) const;

    // Count of asynchronous buffer-size-change notifications from the driver.
    unsigned driverBufferSizeChangeNotifyCount() const;

    std::uint64_t callbackCount() const;
    std::uint64_t xrunCount() const;
    std::uint64_t overloadCount() const;
    double lastCallbackMs() const;
    double p95CallbackMs() const;
    void resetDiagnostics();

private:
    static void forwardCallback(const AsioCallbackInfo& info, void* context);
    void processDriverCallback(const AsioCallbackInfo& info);

    IAsioDriverProvider& m_provider;
    AudientDriverMatcher m_matcher;
    DeviceStateMachine m_state;
    DriverIdentity m_selectedIdentity;
    std::unique_ptr<IAsioDriver> m_driver;
    DriverCapabilities m_capabilities;
    ChannelPlan m_plan;
    long m_sampleRate = 0;
    long m_bufferSamples = 0;
    AsioCallbackFn m_streamProcessor = nullptr;
    void* m_streamContext = nullptr;
    std::atomic<bool> m_panic{false};
    std::atomic<bool> m_safeMode{false};

    engine::EngineCounters m_counters;
    engine::CallbackTimingHistogram m_timing;
};

} // namespace audient::asio