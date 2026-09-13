#pragma once

#include "asio/IAsioDriver.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace audient::asio
{

class SimulatedAsioDriver : public IAsioDriver
{
public:
    explicit SimulatedAsioDriver(DriverIdentity identity, int inputChannels = 2, int outputChannels = 2);

    DriverCapabilities capabilities() const override;
    bool initDriver(long sampleRate, std::string& error) override;
    bool prepareBuffers(long bufferSamples, const std::vector<ChannelInfo>& inputs,
                        const std::vector<ChannelInfo>& outputs, std::string& error) override;
    bool setCallback(AsioCallbackFn callback, void* context, std::string& error) override;
    bool startStream(std::string& error) override;
    bool stopStream(std::string& error) override;
    void disposeDriver() override;

    void simulateDisconnect();
    void simulateReconnect();

    // Enables/disables the physical input buffers supplied to the callback.
    // When disabled, the callback receives inputs == nullptr and
    // inputChannels == 0, exercising the adapter's "input unavailable" path
    // (silence, no stale replay) without stopping the stream.
    void setInputsEnabled(bool enabled);

    // Per-channel test hooks (safe while streaming).
    //  - setInputChannelContents(slot, samples): the requested-slot buffer is
    //    filled from `samples` every block (exactly the first `frames` samples);
    //    pass an empty vector to restore the driver's default per-channel tone.
    //  - setInputChannelSilent(slot, true): that requested slot carries digital
    //    silence while its buffer pointer stays valid (per-channel missing-
    //    input simulation; never drops the slot from the callback).
    void setInputChannelContents(std::size_t slot, const std::vector<float>& samples);
    void setInputChannelSilent(std::size_t slot, bool silent);

    // Replaces the driver's input channel descriptors (index/name/order/active)
    // BEFORE capabilities are read (i.e. before selectDevice). Outputs remain
    // the default sequential analogue pairs. Used to prove the channel binding
    // table never depends on contiguous driver indexes or enumeration order.
    void overrideInputChannels(const std::vector<ChannelInfo>& inputChannels);

    long lastSampleCount() const;
    bool isStreaming() const;
    std::int64_t lastSamplePosition() const;
    std::uint64_t startedCount() const;
    std::vector<float> lastOutput(std::size_t channel) const;
    float lastInput(std::size_t channel, std::size_t sample) const;

private:
    void engineLoop();

    DriverIdentity m_identity;
    DriverCapabilities m_caps;
    long m_sampleRate = 48000;
    long m_bufferSamples = 64;
    AsioCallbackFn m_callback = nullptr;
    void* m_context = nullptr;

    mutable std::mutex m_mutex;
    std::vector<std::vector<float>> m_inputs;
    std::vector<std::vector<float>> m_outputs;
    std::vector<std::vector<float>> m_lastOutputs;
    std::vector<long> m_requestAsioIndex;   // driver channel index of each requested input slot
    std::vector<std::vector<float>> m_overrideInputs; // per-requested-slot waveform override
    std::vector<bool> m_silentInputs;       // per-requested-slot digital-silence flag
    std::vector<ChannelInfo> m_inputOverrideDescs; // applied by overrideInputChannels before open
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{true};
    std::atomic<std::int64_t> m_samplePosition{0};
    std::atomic<std::uint64_t> m_startedCount{0};
    std::atomic<bool> m_inputsEnabled{true};
};

class SimulatedAsioProvider : public IAsioDriverProvider
{
public:
    void setPresent(bool present);
    // Output channel count for the simulated device (default 2; 4 exercises the
    // secondary/headphone output pair). Set BEFORE the driver is opened.
    void setOutputChannels(int outputChannels);
    // Optional non-default input channel layout (names/indexes/order) applied
    // to every driver opened afterwards. Empty = default "Analog N" 0..N-1.
    void setInputChannelLayout(const std::vector<ChannelInfo>& inputChannels);
    std::vector<DriverIdentity> available() const override;
    std::unique_ptr<IAsioDriver> open(const DriverIdentity& identity, std::string& error) override;
    SimulatedAsioDriver* lastOpenedDriver() const;

private:
    bool m_present = true;
    int m_outputChannels = 2;
    std::vector<ChannelInfo> m_inputLayout;
    SimulatedAsioDriver* m_last = nullptr;
};

} // namespace audient::asio