#include "asio/sdk/NativeAsioDriver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace audient::asio
{

namespace
{

NativeAsioDriver* s_activeDriver = nullptr;

bool supportedInputType(ASIOSampleType type)
{
    switch (type)
    {
    case ASIOSTInt16LSB:
    case ASIOSTInt24LSB:
    case ASIOSTInt32LSB:
    case ASIOSTFloat32LSB:
    case ASIOSTFloat64LSB:
        return true;
    default:
        return false;
    }
}

std::size_t sampleBytes(ASIOSampleType type)
{
    switch (type)
    {
    case ASIOSTInt16LSB: return 2;
    case ASIOSTInt24LSB: return 3;
    case ASIOSTInt32LSB: return 4;
    case ASIOSTFloat32LSB: return 4;
    case ASIOSTFloat64LSB: return 8;
    default: return 4;
    }
}

std::int32_t readInt24LE(const unsigned char* src)
{
    std::int32_t value = src[0] | (src[1] << 8) | (src[2] << 16);
    if (value & 0x800000)
    {
        value |= ~0xFFFFFF;
    }
    return value;
}

void writeInt24LE(float sample, unsigned char* dst)
{
    std::int32_t scaled = static_cast<std::int32_t>(std::lround(sample * 8388608.0f));
    if (scaled > 8388607)
    {
        scaled = 8388607;
    }
    if (scaled < -8388608)
    {
        scaled = -8388608;
    }
    dst[0] = static_cast<unsigned char>(scaled & 0xFF);
    dst[1] = static_cast<unsigned char>((scaled >> 8) & 0xFF);
    dst[2] = static_cast<unsigned char>((scaled >> 16) & 0xFF);
}

void convertToFloat(ASIOSampleType type, const void* source, float* destination, std::size_t frames)
{
    const unsigned char* raw = static_cast<const unsigned char*>(source);
    for (std::size_t i = 0; i < frames; ++i)
    {
        float value = 0.0f;
        switch (type)
        {
        case ASIOSTInt16LSB:
        {
            std::int16_t sample = 0;
            std::memcpy(&sample, raw, sizeof(sample));
            value = sample / 32768.0f;
            raw += 2;
            break;
        }
        case ASIOSTInt24LSB:
            value = static_cast<float>(readInt24LE(raw)) / 8388608.0f;
            raw += 3;
            break;
        case ASIOSTInt32LSB:
        {
            std::int32_t sample = 0;
            std::memcpy(&sample, raw, sizeof(sample));
            value = static_cast<float>(sample) / 2147483648.0f;
            raw += 4;
            break;
        }
        case ASIOSTFloat32LSB:
        {
            std::memcpy(&value, raw, sizeof(value));
            raw += 4;
            break;
        }
        case ASIOSTFloat64LSB:
        {
            double sample = 0.0;
            std::memcpy(&sample, raw, sizeof(sample));
            value = static_cast<float>(sample);
            raw += 8;
            break;
        }
        default:
            raw += 4;
            break;
        }
        destination[i] = value;
    }
}

void convertFromFloat(ASIOSampleType type, const float* source, void* destination, std::size_t frames)
{
    unsigned char* raw = static_cast<unsigned char*>(destination);
    for (std::size_t i = 0; i < frames; ++i)
    {
        const float sample = source[i];
        switch (type)
        {
        case ASIOSTInt16LSB:
        {
            std::int32_t scaled = static_cast<std::int32_t>(std::lround(sample * 32768.0f));
            scaled = std::clamp(scaled, static_cast<std::int32_t>(-32768), static_cast<std::int32_t>(32767));
            const std::int16_t output = static_cast<std::int16_t>(scaled);
            std::memcpy(raw, &output, sizeof(output));
            raw += 2;
            break;
        }
        case ASIOSTInt24LSB:
            writeInt24LE(sample, raw);
            raw += 3;
            break;
        case ASIOSTInt32LSB:
        {
            std::int32_t scaled = static_cast<std::int32_t>(std::lround(sample * 2147483648.0f));
            std::memcpy(raw, &scaled, sizeof(scaled));
            raw += 4;
            break;
        }
        case ASIOSTFloat32LSB:
            std::memcpy(raw, &sample, sizeof(sample));
            raw += 4;
            break;
        case ASIOSTFloat64LSB:
        {
            const double output = static_cast<double>(sample);
            std::memcpy(raw, &output, sizeof(output));
            raw += 8;
            break;
        }
        default:
            std::memcpy(raw, &sample, sizeof(sample));
            raw += 4;
            break;
        }
    }
}

SampleFormat mapSampleType(ASIOSampleType type)
{
    switch (type)
    {
    case ASIOSTInt16LSB:
        return SampleFormat::Int16LE;
    case ASIOSTInt24LSB:
        return SampleFormat::Int24LE;
    case ASIOSTInt32LSB:
        return SampleFormat::Int32LE;
    case ASIOSTFloat64LSB:
        return SampleFormat::Float64LE;
    default:
        return SampleFormat::Float32LE;
    }
}

std::string channelName(const ASIOChannelInfo& info)
{
    std::string name(info.name, sizeof(info.name));
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0'))
    {
        name.pop_back();
    }
    return name;
}

} // namespace

std::atomic<long> NativeAsioDriver::s_notifiedBufferSize{0};
std::atomic<unsigned> NativeAsioDriver::s_bufferChangeNotifyCount{0};

NativeAsioDriver::NativeAsioDriver(DriverIdentity identity)
    : m_identity(std::move(identity))
{
}

NativeAsioDriver::~NativeAsioDriver()
{
    disposeDriver();
}

bool NativeAsioDriver::opened() const
{
    return m_opened;
}

const DriverIdentity& NativeAsioDriver::identity() const
{
    return m_identity;
}

const ASIOCallbacks& NativeAsioDriver::callbacks() const
{
    return m_callbacks;
}

bool NativeAsioDriver::open(std::string& error)
{
    error.clear();
    if (m_asio != nullptr || m_opened)
    {
        return true;
    }

    const HRESULT coResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_coInitialized = (coResult == S_OK || coResult == S_FALSE);

    const std::wstring wideClsid(m_identity.clsid.begin(), m_identity.clsid.end());
    CLSID clsid{};
    if (CLSIDFromString(const_cast<LPOLESTR>(wideClsid.c_str()), &clsid) != NOERROR)
    {
        error = "the registered ASIO CLSID is malformed: " + m_identity.clsid;
        if (m_coInitialized)
        {
            CoUninitialize();
            m_coInitialized = false;
        }
        return false;
    }

    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&m_asio));
    if (FAILED(hr) || m_asio == nullptr)
    {
        error = "CoCreateInstance failed for ASIO driver " + m_identity.name + " (hr 0x" + [](HRESULT value) {
            char buffer[16]{};
            std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(value));
            return std::string(buffer);
        }(hr) + ")";
        if (m_coInitialized)
        {
            CoUninitialize();
            m_coInitialized = false;
        }
        return false;
    }

    if (m_asio->init(nullptr) != ASIOTrue)
    {
        error = "ASIO driver " + m_identity.name + " refused init";
        m_asio->Release();
        m_asio = nullptr;
        if (m_coInitialized)
        {
            CoUninitialize();
            m_coInitialized = false;
        }
        return false;
    }

    if (!collectCapabilities(error))
    {
        m_asio->Release();
        m_asio = nullptr;
        if (m_coInitialized)
        {
            CoUninitialize();
            m_coInitialized = false;
        }
        return false;
    }

    m_opened = true;
    return true;
}

bool NativeAsioDriver::collectCapabilities(std::string& error)
{
    long inputs = 0;
    long outputs = 0;
    if (m_asio->getChannels(&inputs, &outputs) != ASE_OK || inputs < 1 || outputs < 2)
    {
        error = "the ASIO driver does not expose at least one input and two outputs";
        return false;
    }
    m_caps.inputChannels = inputs;
    m_caps.outputChannels = outputs;

    long minSize = 0;
    long maxSize = 0;
    long preferredSize = 0;
    long granularity = 0;
    if (m_asio->getBufferSize(&minSize, &maxSize, &preferredSize, &granularity) == ASE_OK)
    {
        m_caps.bufferSizes.minimum = minSize;
        m_caps.bufferSizes.maximum = maxSize;
        m_caps.bufferSizes.preferred = preferredSize;
        m_caps.bufferSizes.granularity = granularity;
    }

    long inputLatency = 0;
    long outputLatency = 0;
    if (m_asio->getLatencies(&inputLatency, &outputLatency) == ASE_OK)
    {
        m_caps.latencies.input = inputLatency;
        m_caps.latencies.output = outputLatency;
    }

    m_caps.supportedSampleRates.clear();
    for (const long rate : {44100L, 48000L, 88200L, 96000L, 192000L})
    {
        if (m_asio->canSampleRate(rate) == ASE_OK)
        {
            m_caps.supportedSampleRates.push_back(rate);
        }
    }
    if (m_caps.supportedSampleRates.empty())
    {
        error = "the ASIO driver reported no supported sample rates";
        return false;
    }

    m_caps.channels.clear();
    for (long i = 0; i < inputs; ++i)
    {
        ASIOChannelInfo info{};
        info.channel = i;
        info.isInput = ASIOTrue;
        if (m_asio->getChannelInfo(&info) != ASE_OK)
        {
            continue;
        }
        ChannelInfo channel{};
        channel.index = i;
        channel.isInput = true;
        channel.isActive = true;
        channel.name = channelName(info);
        channel.preferredFormat = mapSampleType(info.type);
        m_caps.channels.push_back(channel);
    }
    for (long i = 0; i < outputs; ++i)
    {
        ASIOChannelInfo info{};
        info.channel = i;
        info.isInput = ASIOFalse;
        const ASIOError chErr = m_asio->getChannelInfo(&info);
        if (chErr != ASE_OK)
        {
            continue;
        }
        ChannelInfo channel{};
        channel.index = i;
        channel.isInput = false;
        channel.isActive = true;
        channel.name = channelName(info);
        channel.preferredFormat = mapSampleType(info.type);
        m_caps.channels.push_back(channel);
    }

    // Diagnostic channel-table sweep (env AUDIENT_ASIO_CHANNEL_DIAG=1). Reveals
    // whether ASIO outputs 2/3 exist but are refused/inactive by the driver, which
    // explains why a secondary headphone pair can never enter the stream.
    bool diag = false;
    {
        char* value = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&value, &len, "AUDIENT_ASIO_CHANNEL_DIAG") == 0)
        {
            diag = (value != nullptr);
            std::free(value);
        }
    }
    if (diag)
    {
        std::printf("[asio-diag] getChannels in=%ld out=%ld\n", inputs, outputs);
        const long probeMax = inputs + outputs + 2;
        for (int dir = 0; dir <= 1; ++dir)
        {
            for (long i = 0; i < probeMax; ++i)
            {
                ASIOChannelInfo info{};
                info.channel = i;
                info.isInput = dir ? ASIOTrue : ASIOFalse;
                const ASIOError err = m_asio->getChannelInfo(&info);
                if (err == ASE_OK)
                {
                    std::printf("[asio-diag] %s ch%-2ld OK \"%s\" type=%ld drvActive=%ld\n",
                                dir ? "in " : "out", i, info.name,
                                static_cast<long>(info.type),
                                static_cast<long>(info.isActive));
                }
                else
                {
                    std::printf("[asio-diag] %s ch%-2ld err=%d\n", dir ? "in " : "out", i,
                                static_cast<int>(err));
                }
            }
        }
    }
    return !m_caps.channels.empty();
}

DriverCapabilities NativeAsioDriver::capabilities() const
{
    return m_caps;
}

bool NativeAsioDriver::initDriver(long sampleRate, std::string& error)
{
    error.clear();
    if (m_asio == nullptr)
    {
        error = "driver not opened";
        return false;
    }
    if (m_asio->canSampleRate(sampleRate) != ASE_OK)
    {
        error = "sample rate not supported by this ASIO driver";
        return false;
    }
    if (m_asio->setSampleRate(sampleRate) != ASE_OK)
    {
        error = "setSampleRate failed on this ASIO driver";
        return false;
    }
    ASIOSampleRate actual = 0.0;
    if (m_asio->getSampleRate(&actual) == ASE_OK && std::abs(actual - static_cast<double>(sampleRate)) > 1.0)
    {
        error = "the ASIO driver did not accept the requested sample rate";
        return false;
    }
    return true;
}

bool NativeAsioDriver::prepareBuffers(long bufferSamples, const std::vector<ChannelInfo>& inputs,
                                      const std::vector<ChannelInfo>& outputs, std::string& error)
{
    error.clear();
    if (m_asio == nullptr)
    {
        error = "driver not opened";
        return false;
    }

    s_notifiedBufferSize.store(0, std::memory_order_relaxed);
    m_bufferInfos.clear();
    for (const ChannelInfo& input : inputs)
    {
        ASIOBufferInfo buffer{};
        buffer.isInput = ASIOTrue;
        buffer.channelNum = input.index;
        m_bufferInfos.push_back(buffer);
    }
    for (const ChannelInfo& output : outputs)
    {
        ASIOBufferInfo buffer{};
        buffer.isInput = ASIOFalse;
        buffer.channelNum = output.index;
        m_bufferInfos.push_back(buffer);
    }
    if (m_bufferInfos.empty())
    {
        error = "no ASIO channels requested";
        return false;
    }

    // The ASIO driver keeps this pointer for the lifetime of the buffers and may
    // invoke it from its own audio thread immediately during ASIOStart(). Must be
    // a stable object member (see project guidelines Â§8/Â§20 and incident notes Checkpoint 10), never a
    // stack temporary: an optimized build reuses the dead stack slot and the driver
    // then calls through overwritten function pointers (0xC0000005 before the first
    // bufferSwitch). The Steinberg sample keeps its ASIOCallbacks at file scope.
    m_callbacks = {};
    m_callbacks.bufferSwitch = &NativeAsioDriver::asioBufferSwitch;
    m_callbacks.sampleRateDidChange = &NativeAsioDriver::asioSampleRateDidChange;
    m_callbacks.asioMessage = &NativeAsioDriver::asioMessage;
    m_callbacks.bufferSwitchTimeInfo = nullptr;

    const ASIOError createResult = m_asio->createBuffers(m_bufferInfos.data(), static_cast<long>(m_bufferInfos.size()), bufferSamples, &m_callbacks);
    if (createResult != ASE_OK)
    {
        error = "createBuffers failed on this ASIO driver";
        return false;
    }

    bool bufDiag = false;
    {
        char* value = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&value, &len, "AUDIENT_ASIO_BUFFERS_DIAG") == 0)
        {
            bufDiag = (value != nullptr);
            std::free(value);
        }
    }
    if (bufDiag)
    {
        std::printf("[asio-diag] requested %zu buffers @ %ld samples:\n", m_bufferInfos.size(),
                    bufferSamples);
        for (const ASIOBufferInfo& b : m_bufferInfos)
        {
            std::printf("[asio-diag]   %s ch%-2ld\n", b.isInput ? "in " : "out", b.channelNum);
        }
        std::printf("[asio-diag] after createBuffers (pointers):\n");
        for (const ASIOBufferInfo& b : m_bufferInfos)
        {
            std::printf("[asio-diag]   %s ch%-2ld buf0=%p buf1=%p\n",
                        b.isInput ? "in " : "out", b.channelNum,
                        b.buffers[0], b.buffers[1]);
        }
    }

    m_bufferTypes.clear();
    for (const ASIOBufferInfo& buffer : m_bufferInfos)
    {
        ASIOChannelInfo info{};
        info.channel = buffer.channelNum;
        info.isInput = buffer.isInput;
        if (m_asio->getChannelInfo(&info) != ASE_OK || !supportedInputType(info.type))
        {
            error = "the ASIO driver uses an unsupported sample format on a requested channel";
            m_asio->disposeBuffers();
            m_bufferInfos.clear();
            return false;
        }
        m_bufferTypes.push_back(info.type);
    }

    m_bufferSamples = bufferSamples;
    const std::size_t inputCount = inputs.size();
    const std::size_t outputCount = outputs.size();
    m_inputStaging.assign(inputCount, std::vector<float>(static_cast<std::size_t>(bufferSamples), 0.0f));
    m_outputStaging.assign(outputCount, std::vector<float>(static_cast<std::size_t>(bufferSamples), 0.0f));
    m_inputPtrs.resize(inputCount);
    m_outputPtrs.resize(outputCount);
    for (std::size_t i = 0; i < inputCount; ++i)
    {
        m_inputPtrs[i] = m_inputStaging[i].data();
    }
    for (std::size_t i = 0; i < outputCount; ++i)
    {
        m_outputPtrs[i] = m_outputStaging[i].data();
    }

    // DE-POP startup contract (AGENTS §7/§8): a freshly created ASIO output
    // buffer may contain stale samples from the previous session (or be
    // uninitialized). The driver can play one half before our first callback
    // ever fills it, which produced an audible POP on external buffer changes.
    // Explicitly zero BOTH double-buffer halves of every active OUTPUT channel
    // BEFORE ASIOStart. All supported formats encode exact zero as all-zero
    // bytes. Control thread, pre-start: no realtime impact.
    std::size_t preclearedChannels = 0;
    for (std::size_t i = inputCount; i < m_bufferInfos.size(); ++i)
    {
        const std::size_t bytes =
            static_cast<std::size_t>(bufferSamples) * sampleBytes(m_bufferTypes[i]);
        for (int half = 0; half < 2; ++half)
        {
            void* buffer = m_bufferInfos[i].buffers[half];
            if (buffer != nullptr)
            {
                std::memset(buffer, 0, bytes);
            }
        }
        ++preclearedChannels;
    }
    std::printf("[audio-depop] output buffers precleared (channels=%zu samples=%ld)\n",
                preclearedChannels, bufferSamples);

    m_samplePosition = 0;
    s_activeDriver = this;
    m_prepared = true;
    return true;
}

bool NativeAsioDriver::setCallback(AsioCallbackFn callback, void* context, std::string& error)
{
    error.clear();
    m_callback = callback;
    m_callbackContext = context;
    return true;
}

bool NativeAsioDriver::startStream(std::string& error)
{
    error.clear();
    if (m_asio == nullptr || !m_prepared)
    {
        error = "stream is not prepared";
        return false;
    }
    if (m_asio->start() != ASE_OK)
    {
        error = "ASIO start failed";
        return false;
    }
    return true;
}

bool NativeAsioDriver::stopStream(std::string& error)
{
    error.clear();
    if (m_asio != nullptr)
    {
        (void)m_asio->stop();
    }
    return true;
}

bool NativeAsioDriver::currentSampleRate(long& sampleRate) const
{
    sampleRate = 0;
    if (m_asio == nullptr)
    {
        return false;
    }
    ASIOSampleRate actual = 0.0;
    if (m_asio->getSampleRate(&actual) != ASE_OK || actual <= 0.0)
    {
        return false;
    }
    sampleRate = static_cast<long>(actual + 0.5);
    return true;
}

unsigned NativeAsioDriver::bufferSizeChangeNotifyCount() const
{
    return s_bufferChangeNotifyCount.load(std::memory_order_relaxed);
}

bool NativeAsioDriver::currentBufferSize(long& bufferSamples) const
{
    bufferSamples = 0;
    const long notified = s_notifiedBufferSize.load(std::memory_order_relaxed);
    if (notified > 0)
    {
        bufferSamples = notified;
        return true;
    }
    if (m_asio == nullptr)
    {
        return false;
    }
    long minimum = 0;
    long maximum = 0;
    long preferred = 0;
    long granularity = 0;
    if (m_asio->getBufferSize(&minimum, &maximum, &preferred, &granularity) != ASE_OK || preferred <= 0)
    {
        return false;
    }
    bufferSamples = preferred;
    return true;
}

void NativeAsioDriver::disposeDriver()
{
    if (m_prepared && m_asio != nullptr)
    {
        (void)m_asio->stop();
        (void)m_asio->disposeBuffers();
        m_prepared = false;
    }
    if (s_activeDriver == this)
    {
        s_activeDriver = nullptr;
    }
    m_bufferInfos.clear();
    m_bufferTypes.clear();
    m_inputStaging.clear();
    m_outputStaging.clear();
    m_inputPtrs.clear();
    m_outputPtrs.clear();

    if (m_asio != nullptr)
    {
        m_asio->Release();
        m_asio = nullptr;
    }
    if (m_coInitialized)
    {
        CoUninitialize();
        m_coInitialized = false;
    }
    m_opened = false;
}

void NativeAsioDriver::asioBufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
{
    (void)directProcess;
    if (s_activeDriver != nullptr)
    {
        s_activeDriver->processBufferSwitch(doubleBufferIndex);
    }
}

void NativeAsioDriver::asioSampleRateDidChange(ASIOSampleRate sampleRate)
{
    (void)sampleRate;
}

long NativeAsioDriver::asioMessage(long selector, long value, void* message, double* opt)
{
    (void)message;
    (void)opt;
    if (selector == kAsioEngineVersion)
    {
        return 2;
    }
    if (selector == kAsioSelectorSupported)
    {
        // Advertise that we handle the asynchronous buffer-size-change
        // notification so the driver reports external (iD.exe) changes.
        if (value == kAsioBufferSizeChange || value == kAsioResetRequest ||
            value == kAsioResyncRequest)
        {
            return 1;
        }
        return 0;
    }
    if (selector == kAsioBufferSizeChange)
    {
        // Driver-thread callback. RT-safe: atomic stores only, no locks/alloc.
        if (value > 0)
        {
            s_notifiedBufferSize.store(value, std::memory_order_relaxed);
        }
        s_bufferChangeNotifyCount.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    if (selector == kAsioResetRequest || selector == kAsioResyncRequest)
    {
        // A reset/resync request is surfaced to the control thread as a
        // "reconcile" trigger by the application; acknowledge it here.
        return 1;
    }
    return 0;
}

void NativeAsioDriver::processBufferSwitch(long doubleBufferIndex)
{
    if (!m_prepared || m_callback == nullptr)
    {
        return;
    }
    const std::size_t frames = static_cast<std::size_t>(m_bufferSamples);

    const std::size_t inputCount = m_inputStaging.size();
    for (std::size_t i = 0; i < inputCount; ++i)
    {
        convertToFloat(m_bufferTypes[i], m_bufferInfos[i].buffers[doubleBufferIndex], m_inputStaging[i].data(), frames);
    }

    for (std::vector<float>& staging : m_outputStaging)
    {
        std::fill(staging.begin(), staging.end(), 0.0f);
    }

    AsioCallbackInfo info{};
    info.sampleCount = m_bufferSamples;
    info.inputChannels = m_inputPtrs.size();
    info.outputChannels = m_outputPtrs.size();
    info.inputs = m_inputPtrs.data();
    info.outputs = m_outputPtrs.data();
    info.samplePosition = m_samplePosition;
    m_callback(info, m_callbackContext);
    m_samplePosition += m_bufferSamples;

    const std::size_t outputCount = m_outputStaging.size();
    for (std::size_t i = 0; i < outputCount; ++i)
    {
        convertFromFloat(m_bufferTypes[inputCount + i], m_outputStaging[i].data(),
                         m_bufferInfos[inputCount + i].buffers[doubleBufferIndex], frames);
    }
}

} // namespace audient::asio