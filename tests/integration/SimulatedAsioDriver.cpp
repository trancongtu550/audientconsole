#include "SimulatedAsioDriver.h"

#include <algorithm>
#include <mutex>
#include <thread>

namespace audient::asio
{

SimulatedAsioDriver::SimulatedAsioDriver(DriverIdentity identity, int inputChannels, int outputChannels)
    : m_identity(std::move(identity))
{
    m_caps.inputChannels = inputChannels > 0 ? inputChannels : 2;
    m_caps.outputChannels = outputChannels > 0 ? outputChannels : 2;
    m_caps.supportedSampleRates = {44100, 48000, 88200};
    m_caps.bufferSizes = {16, 512, 64, 32};
    m_caps.latencies = {11, 5};

    for (int i = 0; i < m_caps.inputChannels; ++i)
    {
        m_caps.channels.push_back({i, true, true, "Analog " + std::to_string(i + 1), SampleFormat::Float32LE});
    }
    for (int i = 0; i < m_caps.outputChannels; ++i)
    {
        m_caps.channels.push_back({i, false, true, "Analog " + std::to_string(i + 1), SampleFormat::Float32LE});
    }
}

DriverCapabilities SimulatedAsioDriver::capabilities() const
{
    return m_caps;
}

bool SimulatedAsioDriver::initDriver(long sampleRate, std::string& error)
{
    if (!m_caps.supportsSampleRate(sampleRate))
    {
        error = "sample rate not supported by simulated driver";
        return false;
    }
    m_sampleRate = sampleRate;
    return true;
}

bool SimulatedAsioDriver::prepareBuffers(long bufferSamples, const std::vector<ChannelInfo>& inputs,
                                         const std::vector<ChannelInfo>& outputs, std::string& error)
{
    if (bufferSamples < m_caps.bufferSizes.minimum || bufferSamples > m_caps.bufferSizes.maximum)
    {
        error = "buffer size outside simulated driver range";
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_bufferSamples = bufferSamples;

    m_inputs.clear();
    for (const ChannelInfo& channel : inputs)
    {
        if (channel.isActive)
        {
            m_inputs.emplace_back(static_cast<std::size_t>(bufferSamples), 0.0f);
        }
    }
    m_outputs.clear();
    m_lastOutputs.clear();
    for (const ChannelInfo& channel : outputs)
    {
        if (channel.isActive)
        {
            m_outputs.emplace_back(static_cast<std::size_t>(bufferSamples), 0.0f);
            m_lastOutputs.emplace_back(static_cast<std::size_t>(bufferSamples), 0.0f);
        }
    }

    // Record the driver channel index of each requested input slot so the
    // engine loop can feed an index-specific tone (slot fill never depends on
    // position, only on which driver channel was bound to the slot).
    m_requestAsioIndex.clear();
    for (const ChannelInfo& channel : inputs)
    {
        if (channel.isActive)
        {
            m_requestAsioIndex.push_back(channel.index);
        }
    }
    m_overrideInputs.assign(m_inputs.size(), std::vector<float>());
    m_silentInputs.assign(m_inputs.size(), false);

    if (m_inputs.empty() || m_outputs.size() < 2)
    {
        error = "simulated driver needs at least one input and two outputs";
        return false;
    }
    return true;
}

bool SimulatedAsioDriver::setCallback(AsioCallbackFn callback, void* context, std::string& error)
{
    if (callback == nullptr)
    {
        error = "null callback";
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = callback;
    m_context = context;
    return true;
}

bool SimulatedAsioDriver::startStream(std::string& error)
{
    if (!m_connected.load(std::memory_order_acquire))
    {
        error = "simulated device is disconnected";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_callback == nullptr)
        {
            error = "no callback registered";
            return false;
        }
    }

    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true))
    {
        error = "already streaming";
        return false;
    }

    m_startedCount.fetch_add(1, std::memory_order_relaxed);
    m_thread = std::thread(&SimulatedAsioDriver::engineLoop, this);
    return true;
}

bool SimulatedAsioDriver::stopStream(std::string& error)
{
    (void)error;
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    return true;
}

void SimulatedAsioDriver::disposeDriver()
{
    std::string ignored;
    stopStream(ignored);
}

void SimulatedAsioDriver::simulateDisconnect()
{
    std::string ignored;
    stopStream(ignored);
    m_connected.store(false, std::memory_order_release);
}

void SimulatedAsioDriver::setInputsEnabled(bool enabled)
{
    m_inputsEnabled.store(enabled, std::memory_order_release);
}

void SimulatedAsioDriver::simulateReconnect()
{
    m_connected.store(true, std::memory_order_release);
}

void SimulatedAsioDriver::setInputChannelContents(std::size_t slot, const std::vector<float>& samples)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (slot >= m_overrideInputs.size())
    {
        return;
    }
    m_overrideInputs[slot] = samples;
}

void SimulatedAsioDriver::setInputChannelSilent(std::size_t slot, bool silent)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (slot >= m_silentInputs.size())
    {
        return;
    }
    m_silentInputs[slot] = silent;
}

void SimulatedAsioDriver::overrideInputChannels(const std::vector<ChannelInfo>& inputChannels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inputOverrideDescs = inputChannels;

    std::vector<ChannelInfo> channels = inputChannels;
    const int outputCount = m_caps.outputChannels;
    for (int i = 0; i < outputCount; ++i)
    {
        channels.push_back({i, false, true, "Analog " + std::to_string(i + 1), SampleFormat::Float32LE});
    }
    m_caps.inputChannels = static_cast<long>(inputChannels.size());
    m_caps.channels = std::move(channels);
}

long SimulatedAsioDriver::lastSampleCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bufferSamples;
}

bool SimulatedAsioDriver::isStreaming() const
{
    return m_running.load(std::memory_order_acquire);
}

std::int64_t SimulatedAsioDriver::lastSamplePosition() const
{
    return m_samplePosition.load(std::memory_order_relaxed);
}

std::uint64_t SimulatedAsioDriver::startedCount() const
{
    return m_startedCount.load(std::memory_order_relaxed);
}

std::vector<float> SimulatedAsioDriver::lastOutput(std::size_t channel) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (channel >= m_lastOutputs.size())
    {
        return {};
    }
    return m_lastOutputs[channel];
}

float SimulatedAsioDriver::lastInput(std::size_t channel, std::size_t sample) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inputs[channel][sample];
}

void SimulatedAsioDriver::engineLoop()
{
    const auto blockDuration = std::chrono::duration<double>(m_bufferSamples / static_cast<double>(m_sampleRate));
    std::int64_t position = 0;

    while (m_running.load(std::memory_order_acquire) && m_connected.load(std::memory_order_acquire))
    {
        AsioCallbackFn callback = nullptr;
        void* context = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_callback;
            context = m_context;
        }

        if (callback != nullptr)
        {
            if (m_inputsEnabled.load(std::memory_order_acquire))
            {
                // Fill each requested slot: per-slot silence override wins, then a
                // per-slot waveform override, else an index-based constant tone so
                // content never depends on request position alone.
                std::lock_guard<std::mutex> lock(m_mutex);
                for (std::size_t channel = 0; channel < m_inputs.size(); ++channel)
                {
                    const bool silent = channel < m_silentInputs.size() && m_silentInputs[channel];
                    const std::vector<float>* overrideWave =
                        (channel < m_overrideInputs.size() && !m_overrideInputs[channel].empty())
                            ? &m_overrideInputs[channel]
                            : nullptr;
                    for (std::size_t i = 0; i < m_inputs[channel].size(); ++i)
                    {
                        if (silent)
                        {
                            m_inputs[channel][i] = 0.0f;
                        }
                        else if (overrideWave != nullptr && i < overrideWave->size())
                        {
                            m_inputs[channel][i] = (*overrideWave)[i];
                        }
                        else
                        {
                            const long asioIndex = channel < m_requestAsioIndex.size() ? m_requestAsioIndex[channel] : 0L;
                            m_inputs[channel][i] = 0.25f * static_cast<float>(asioIndex + 1);
                        }
                    }
                }
            }
            for (std::vector<float>& channel : m_outputs)
            {
                std::fill(channel.begin(), channel.end(), 0.0f);
            }

            std::vector<float*> inputPtrs;
            std::vector<float*> outputPtrs;
            if (m_inputsEnabled.load(std::memory_order_acquire))
            {
                for (std::vector<float>& channel : m_inputs)
                {
                    inputPtrs.push_back(channel.data());
                }
            }
            for (std::vector<float>& channel : m_outputs)
            {
                outputPtrs.push_back(channel.data());
            }

            AsioCallbackInfo info{};
            info.sampleCount = m_bufferSamples;
            info.inputChannels = inputPtrs.size();
            info.outputChannels = outputPtrs.size();
            info.inputs = inputPtrs.empty() ? nullptr : inputPtrs.data();
            info.outputs = outputPtrs.data();
            info.samplePosition = position;
            info.nanoSeconds = 0.0;

            callback(info, context);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (std::size_t channel = 0; channel < m_outputs.size(); ++channel)
                {
                    m_lastOutputs[channel] = m_outputs[channel];
                }
                m_samplePosition.store(position, std::memory_order_relaxed);
            }
        }

        position += m_bufferSamples;
        std::this_thread::sleep_for(blockDuration);
    }
}

void SimulatedAsioProvider::setPresent(bool present)
{
    m_present = present;
}

void SimulatedAsioProvider::setOutputChannels(int outputChannels)
{
    m_outputChannels = outputChannels > 2 ? outputChannels : 2;
}

void SimulatedAsioProvider::setInputChannelLayout(const std::vector<ChannelInfo>& inputChannels)
{
    m_inputLayout = inputChannels;
}

std::vector<DriverIdentity> SimulatedAsioProvider::available() const
{
    if (!m_present)
    {
        return {};
    }
    DriverIdentity identity;
    identity.clsid = "{11111111-2222-3333-4444-555555555555}";
    identity.name = "Audient iD14 ASIO Simulator";
    return {identity};
}

std::unique_ptr<IAsioDriver> SimulatedAsioProvider::open(const DriverIdentity& identity, std::string& error)
{
    if (!m_present)
    {
        error = "no device present";
        return nullptr;
    }
    const std::size_t inputCount = m_inputLayout.empty() ? 2 : m_inputLayout.size();
    auto driver = std::make_unique<SimulatedAsioDriver>(identity, static_cast<int>(inputCount), m_outputChannels);
    if (!m_inputLayout.empty())
    {
        driver->overrideInputChannels(m_inputLayout);
    }
    m_last = driver.get();
    return driver;
}

SimulatedAsioDriver* SimulatedAsioProvider::lastOpenedDriver() const
{
    return m_last;
}

} // namespace audient::asio