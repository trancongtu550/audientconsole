#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "pico/PicoRenderDevice.h"

#include <atomic>
#include <cstdint>
#include <string>

struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;

namespace audient::pico
{

struct PicoEndpointNotificationClient;

// WASAPI shared-mode, event-driven render stream to the VoxBridge Pico v0.1.1
// class-compliant UAC2 playback endpoint.
//
// Ownership (AGENTS §7/§11): this object owns ALL COM/device lifetime and is
// driven only from the PicoUsbWorker NORMAL thread. It is never touched by the
// ASIO realtime callback. Discovery keys on the USB hardware id
// (VID_1209&PID_B2DC) with a guarded friendly-name fallback; the endpoint mix
// format is queried (only 48 kHz stereo is accepted) and the internal float mono
// is interleaved to L=R by the worker before submit().
//
// Reconnect: an IMMNotificationClient sets needsReopen() on device-state-change/
// removal so the worker tears down and re-discovers promptly. COM is initialized
// once per worker thread in open() and balanced in threadShutdown().
class WasapiPicoRenderDevice final : public IPicoRenderDevice
{
public:
    WasapiPicoRenderDevice() = default;
    ~WasapiPicoRenderDevice() override;

    PicoDeviceOpenResult open() override;
    void close() override;
    bool isOpen() const override;
    bool needsReopen() const override;
    bool waitForRenderReady(unsigned timeoutMs) override;
    void wake() override;
    unsigned availableRenderFrames() override;
    bool submit(const float* stereoInterleaved, std::size_t frames, bool* deviceLost) override;
    unsigned sampleRate() const override;
    unsigned channels() const override;
    const std::string& deviceName() const override;
    void threadShutdown() override;

    WasapiPicoRenderDevice(const WasapiPicoRenderDevice&) = delete;
    WasapiPicoRenderDevice& operator=(const WasapiPicoRenderDevice&) = delete;

private:
    void writeFrames(unsigned char* dst, const float* src, unsigned frames) const;

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_client = nullptr;
    IAudioRenderClient* m_render = nullptr;
    void* m_mix = nullptr; // WAVEFORMATEX* (CoTaskMem-owned)
    PicoEndpointNotificationClient* m_notify = nullptr;
    void* m_event = nullptr; // HANDLE
    std::uint32_t m_bufferFrames = 0;
    bool m_started = false;
    bool m_comInitialized = false;
    bool m_isFloat = true;
    unsigned m_bitsPerSample = 32;
    unsigned m_channels = 2;
    unsigned m_sampleRate = 48000;
    std::string m_name;
    std::atomic<bool> m_needsReopen{false};
};

} // namespace audient::pico
