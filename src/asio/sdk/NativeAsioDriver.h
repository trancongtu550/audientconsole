#pragma once

#include "asio/AsioTypes.h"
#include "asio/IAsioDriver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "common/iasiodrv.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audient::asio
{

class NativeAsioDriver : public IAsioDriver
{
public:
    explicit NativeAsioDriver(DriverIdentity identity);
    ~NativeAsioDriver() override;

    NativeAsioDriver(const NativeAsioDriver&) = delete;
    NativeAsioDriver& operator=(const NativeAsioDriver&) = delete;

    bool open(std::string& error);
    bool opened() const;
    const DriverIdentity& identity() const;
    const ASIOCallbacks& callbacks() const;

    DriverCapabilities capabilities() const override;
    bool initDriver(long sampleRate, std::string& error) override;
    bool prepareBuffers(long bufferSamples, const std::vector<ChannelInfo>& inputs,
                        const std::vector<ChannelInfo>& outputs, std::string& error) override;
    bool setCallback(AsioCallbackFn callback, void* context, std::string& error) override;
    bool startStream(std::string& error) override;
    bool stopStream(std::string& error) override;
    void disposeDriver() override;
    bool currentSampleRate(long& sampleRate) const override;
    bool currentBufferSize(long& bufferSamples) const override;
    unsigned bufferSizeChangeNotifyCount() const override;

private:
    static void asioBufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
    static void asioSampleRateDidChange(ASIOSampleRate sampleRate);
    static long asioMessage(long selector, long value, void* message, double* opt);

    void processBufferSwitch(long doubleBufferIndex);
    bool collectCapabilities(std::string& error);

    DriverIdentity m_identity;
    DriverCapabilities m_caps;
    IASIO* m_asio = nullptr;
    bool m_opened = false;
    bool m_coInitialized = false;

    // Host-owned callbacks struct. The ASIO driver retains the pointer passed to
    // createBuffers() and invokes these functions from its own threads until
    // disposeBuffers() returns; it must therefore be a stable object member, not a
    // stack temporary that dies when prepareBuffers() returns.
    ASIOCallbacks m_callbacks{};

    long m_bufferSamples = 0;
    std::vector<ASIOBufferInfo> m_bufferInfos;
    std::vector<ASIOSampleType> m_bufferTypes;
    std::vector<std::vector<float>> m_inputStaging;
    std::vector<std::vector<float>> m_outputStaging;
    std::vector<float*> m_inputPtrs;
    std::vector<float*> m_outputPtrs;
    std::int64_t m_samplePosition = 0;

    AsioCallbackFn m_callback = nullptr;
    void* m_callbackContext = nullptr;
    bool m_prepared = false;

    // Driver-thread notification of an external buffer-size change (iD.exe /
    // ASIO driver settings). Static because asioMessage() is a static callback
    // selected by the driver; RT-safe: plain atomic, no locks/alloc. One active
    // driver at a time is already an invariant (s_activeDriver).
    static std::atomic<long> s_notifiedBufferSize;
    static std::atomic<unsigned> s_bufferChangeNotifyCount;
};

} // namespace audient::asio