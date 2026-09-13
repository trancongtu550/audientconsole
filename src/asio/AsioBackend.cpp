#include "asio/AsioBackend.h"

#include "engine/CallbackGuard.h"

#include <chrono>

namespace audient::asio
{

AsioBackend::AsioBackend(IAsioDriverProvider& provider)
    : m_provider(provider)
{
    m_state.transition(DeviceState::Uninitialized, DeviceState::Idle);
}

std::vector<DriverIdentity> AsioBackend::availableDevices() const
{
    return m_provider.available();
}

bool AsioBackend::selectAudientDevice(std::string& error)
{
    error.clear();
    const std::vector<DriverIdentity> available = m_provider.available();
    DriverIdentity matched;
    if (!m_matcher.match(available, matched))
    {
        m_state.force(DeviceState::NoDevice);
        error = "no Audient ASIO driver found";
        return false;
    }
    return selectDevice(matched, error);
}

bool AsioBackend::selectDevice(const DriverIdentity& identity, std::string& error)
{
    error.clear();
    if (m_state.current() != DeviceState::Idle)
    {
        error = "device selection requires the idle state";
        return false;
    }

    if (!m_state.transition(DeviceState::Idle, DeviceState::Opening))
    {
        error = "device selection is already in progress";
        return false;
    }

    m_driver = m_provider.open(identity, error);
    if (!m_driver)
    {
        m_state.transition(DeviceState::Opening, DeviceState::Idle);
        return false;
    }

    m_selectedIdentity = identity;
    m_matcher.rememberPreferredIdentity(identity.clsid);
    m_capabilities = m_driver->capabilities();

    if (!m_state.transition(DeviceState::Opening, DeviceState::Ready))
    {
        m_driver->disposeDriver();
        m_driver.reset();
        m_state.force(DeviceState::Idle);
        error = "unexpected state during device open";
        return false;
    }

    return true;
}

bool AsioBackend::configure(long sampleRate, long bufferSamples, const ChannelPlan& plan, std::string& error)
{
    error.clear();
    if (m_state.current() != DeviceState::Ready)
    {
        error = "configure requires the ready state (open a device first)";
        return false;
    }
    if (!m_capabilities.supportsSampleRate(sampleRate))
    {
        error = "sample rate not supported by this driver";
        return false;
    }
    if (!plan.valid())
    {
        error = "the channel plan is incomplete or ambiguous";
        return false;
    }
    if (bufferSamples < m_capabilities.bufferSizes.minimum || bufferSamples > m_capabilities.bufferSizes.maximum)
    {
        error = "buffer size outside the driver range";
        return false;
    }

    if (!m_driver->initDriver(sampleRate, error))
    {
        return false;
    }

    std::vector<ChannelInfo> inputs;
    std::vector<ChannelInfo> outputs;
    for (const ChannelInfo& channel : m_capabilities.channels)
    {
        if (!channel.isActive)
        {
            continue;
        }
        if (channel.isInput)
        {
            inputs.push_back(channel);
        }
        else
        {
            outputs.push_back(channel);
        }
    }

    // Canonical request construction from the binding table in RUNTIME-SLOT
    // order: callback request slot N == binding table slot N (the backend's
    // AsioChannelMap resolves driver index -> runtime slot once here; the
    // callback never re-derives the mapping).
    //
    // Invariant (Phase B B2): the populated binding table is AUTHORITATIVE.
    // The legacy `micInput` alias is honored ONLY for a legacy single-input
    // plan (inputCount == 0 + an explicitly set alias). When the table is
    // populated (inputCount > 0) a conflicting alias is normalized to mirror
    // the table and is NEVER consulted — the canonical production mapping must
    // not be silently replaced by a stale legacy value. Callers that want a
    // different single input clear the table (inputCount = 0) and set the
    // alias, or rebuild the table.
    ChannelPlan effective = plan;
    if (plan.inputCount == 0)
    {
        if (plan.micInput < 0)
        {
            error = "the channel plan has no input binding";
            return false;
        }
        effective.inputCount = 1;
        effective.inputs[0] = StreamInputBinding{};
        effective.inputs[0].asioChannelIndex = plan.micInput;
        effective.inputs[0].runtimeSlot = 0;
    }
    else
    {
        // Table authoritative: the alias may mirror inputs[0].asioChannelIndex
        // but must never disagree with (or override) the populated table.
        effective.micInput = effective.inputs[0].asioChannelIndex;
    }

    std::vector<ChannelInfo> inputRequest;
    for (std::uint32_t slot = 0; slot < effective.inputCount; ++slot)
    {
        const long requestedIndex = effective.inputs[slot].asioChannelIndex;
        bool found = false;
        for (const ChannelInfo& channel : inputs)
        {
            if (channel.index == requestedIndex)
            {
                inputRequest.push_back(channel);
                found = true;
                break;
            }
        }
        if (!found)
        {
            error = "the channel plan references an input index not present in the driver";
            return false;
        }
    }
    std::vector<ChannelInfo> outputRequest;
    // Request order is PLAN order (primary pair, then secondary pair), NOT
    // driver index order: the ASIO callback hands back buffers in the exact
    // request order, so callback output slot 0/1 = primary pair and slot 2/3 =
    // secondary pair regardless of the absolute channel indices.
    const long requestedOutputs[] = {plan.outputLeft, plan.outputRight, plan.outputLeft2, plan.outputRight2};
    for (const long requested : requestedOutputs)
    {
        if (requested < 0)
        {
            continue;
        }
        for (const ChannelInfo& channel : outputs)
        {
            if (channel.index == requested)
            {
                outputRequest.push_back(channel);
                break;
            }
        }
    }

    if (!m_driver->prepareBuffers(bufferSamples, inputRequest, outputRequest, error))
    {
        return false;
    }
    if (!m_driver->setCallback(&AsioBackend::forwardCallback, this, error))
    {
        return false;
    }

    m_plan = effective;
    m_sampleRate = sampleRate;
    m_bufferSamples = bufferSamples;
    return true;
}

void AsioBackend::setStreamProcessor(AsioCallbackFn processor, void* context)
{
    m_streamProcessor = processor;
    m_streamContext = context;
}

bool AsioBackend::start(std::string& error)
{
    error.clear();
    if (m_state.current() != DeviceState::Ready)
    {
        error = "start requires the ready state";
        return false;
    }

    m_state.transition(DeviceState::Ready, DeviceState::Starting);
    m_state.force(DeviceState::Streaming);

    if (!m_driver->startStream(error))
    {
        m_state.transition(DeviceState::Streaming, DeviceState::Stopping);
        m_state.force(DeviceState::Ready);
        return false;
    }

    return true;
}

bool AsioBackend::stop(std::string& error)
{
    error.clear();
    if (m_state.current() != DeviceState::Streaming && m_state.current() != DeviceState::Stopping)
    {
        m_state.force(DeviceState::Ready);
        return true;
    }

    m_state.transition(m_state.current(), DeviceState::Stopping);
    if (m_driver != nullptr)
    {
        (void)m_driver->stopStream(error);
    }
    m_state.transition(DeviceState::Stopping, DeviceState::Ready);
    return m_state.current() == DeviceState::Ready;
}

bool AsioBackend::requestReset(std::string& error)
{
    error.clear();
    if (m_state.current() != DeviceState::Streaming && m_state.current() != DeviceState::Stopping)
    {
        return m_state.current() == DeviceState::Ready;
    }
    return stop(error);
}

bool AsioBackend::changeSampleRate(long sampleRate, std::string& error)
{
    error.clear();
    if (m_state.current() != DeviceState::Ready)
    {
        error = "sample-rate change requires the ready state (stop first)";
        return false;
    }
    if (m_driver == nullptr || m_bufferSamples == 0)
    {
        error = "no configured stream to re-tune";
        return false;
    }
    return configure(sampleRate, m_bufferSamples, m_plan, error);
}

void AsioBackend::panic()
{
    m_panic.store(true, std::memory_order_relaxed);

    if (m_state.current() == DeviceState::Streaming)
    {
        m_state.transition(DeviceState::Streaming, DeviceState::Stopping);
        if (m_driver != nullptr)
        {
            std::string ignored;
            (void)m_driver->stopStream(ignored);
        }
        m_state.transition(DeviceState::Stopping, DeviceState::Ready);
    }
}

void AsioBackend::clearPanic()
{
    m_panic.store(false, std::memory_order_relaxed);
}

void AsioBackend::handleDisconnect()
{
    if (m_state.current() == DeviceState::NoDevice)
    {
        return;
    }

    m_safeMode.store(true, std::memory_order_relaxed);
    m_counters.recordXrun();

    if (m_state.current() == DeviceState::Streaming && m_driver != nullptr)
    {
        std::string ignored;
        (void)m_driver->stopStream(ignored);
    }

    m_state.force(DeviceState::Reconnecting);
}

void AsioBackend::handleReconnect()
{
    if (m_state.current() != DeviceState::Reconnecting)
    {
        return;
    }

    if (!m_selectedIdentity.clsid.empty())
    {
        std::string error;
        std::unique_ptr<IAsioDriver> reopened = m_provider.open(m_selectedIdentity, error);
        if (!reopened)
        {
            m_state.force(DeviceState::Error);
            return;
        }
        m_driver = std::move(reopened);
        m_capabilities = m_driver->capabilities();
    }

    m_safeMode.store(false, std::memory_order_relaxed);
    m_state.transition(DeviceState::Reconnecting, DeviceState::Opening);
    m_state.transition(DeviceState::Opening, DeviceState::Ready);
    m_state.force(DeviceState::Ready);
}

DeviceState AsioBackend::state() const
{
    return m_state.current();
}

const DriverCapabilities& AsioBackend::driverCapabilities() const
{
    return m_capabilities;
}

const ChannelPlan& AsioBackend::activePlan() const
{
    return m_plan;
}

bool AsioBackend::queryDriverSampleRate(long& sampleRate) const
{
    sampleRate = 0;
    if (m_driver == nullptr)
    {
        return false;
    }
    return m_driver->currentSampleRate(sampleRate);
}

bool AsioBackend::queryDriverBufferSize(long& bufferSamples) const
{
    bufferSamples = 0;
    if (m_driver == nullptr)
    {
        return false;
    }
    return m_driver->currentBufferSize(bufferSamples);
}

unsigned AsioBackend::driverBufferSizeChangeNotifyCount() const
{
    return m_driver != nullptr ? m_driver->bufferSizeChangeNotifyCount() : 0u;
}

std::uint64_t AsioBackend::callbackCount() const
{
    return m_counters.snapshot().callbacks;
}

std::uint64_t AsioBackend::xrunCount() const
{
    return m_counters.snapshot().xruns;
}

std::uint64_t AsioBackend::overloadCount() const
{
    return m_counters.snapshot().overloads;
}

double AsioBackend::lastCallbackMs() const
{
    return m_timing.lastMs();
}

double AsioBackend::p95CallbackMs() const
{
    return m_timing.percentileMs(0.95);
}

void AsioBackend::resetDiagnostics()
{
    m_counters.reset();
    m_timing.reset();
}

void AsioBackend::forwardCallback(const AsioCallbackInfo& info, void* context)
{
    auto* backend = static_cast<AsioBackend*>(context);
    backend->processDriverCallback(info);
}

void AsioBackend::processDriverCallback(const AsioCallbackInfo& info)
{
    m_counters.recordCallback();

    const bool streaming = m_state.current() == DeviceState::Streaming;
    const bool suppressed = m_panic.load(std::memory_order_relaxed) || m_safeMode.load(std::memory_order_relaxed);

    if (!streaming || suppressed || m_streamProcessor == nullptr)
    {
        if (info.outputs != nullptr)
        {
            for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
            {
                std::fill(info.outputs[channel], info.outputs[channel] + info.sampleCount, 0.0f);
            }
        }
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    const bool handled = engine::invokeInCallback([&]() { m_streamProcessor(info, m_streamContext); });
    if (!handled)
    {
        m_counters.recordOverload();
        m_counters.recordXrun();
        if (info.outputs != nullptr)
        {
            for (std::size_t channel = 0; channel < info.outputChannels; ++channel)
            {
                std::fill(info.outputs[channel], info.outputs[channel] + info.sampleCount, 0.0f);
            }
        }
    }
    const auto end = std::chrono::steady_clock::now();

    m_timing.record(std::chrono::duration<double, std::milli>(end - start).count());
    m_counters.recordSequenceStep();
}

} // namespace audient::asio