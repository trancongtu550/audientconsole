#include "pico/PicoUsbWorker.h"

#include <algorithm>
#include <cstdio>

namespace audient::pico
{

PicoUsbWorker::PicoUsbWorker(PicoVirtualMicSink& sink, std::unique_ptr<IPicoRenderDevice> device,
                             PicoWorkerConfig config)
    : m_sink(sink)
    , m_device(std::move(device))
    , m_config(config)
    , m_maxFrames(std::max<std::size_t>(1, config.maxRenderFrames))
{
    m_mono.assign(m_maxFrames, 0.0f);
    m_stereo.assign(m_maxFrames * 2u, 0.0f);
}

PicoUsbWorker::~PicoUsbWorker()
{
    stop();
}

void PicoUsbWorker::start()
{
    if (m_thread.joinable())
    {
        return;
    }
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&PicoUsbWorker::run, this);
}

void PicoUsbWorker::requestStop()
{
    m_running.store(false, std::memory_order_release);
    if (m_device)
    {
        m_device->wake();
    }
}

void PicoUsbWorker::stop()
{
    m_running.store(false, std::memory_order_release);
    if (m_device)
    {
        m_device->wake();
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

bool PicoUsbWorker::running() const
{
    return m_running.load(std::memory_order_acquire);
}

void PicoUsbWorker::closeDevice()
{
    if (m_device)
    {
        m_device->close();
    }
    m_sink.setConnected(false);
    m_deviceOpen.store(false, std::memory_order_release);
    m_deviceLost.fetch_add(1, std::memory_order_relaxed);
}

bool PicoUsbWorker::stepOnce()
{
    if (!m_device)
    {
        return false;
    }

    if (!m_device->isOpen())
    {
        const PicoDeviceOpenResult result = m_device->open();
        m_deviceOpen.store(result.ok, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_lastError = result.error;
            if (result.ok)
            {
                m_deviceName = result.deviceName;
            }
        }
        if (!result.ok)
        {
            // No endpoint: mark the transport disconnected (fresh epoch) but
            // never touch the engine or the driver path.
            m_sink.setConnected(false);
            m_failedOpens.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const std::uint64_t opens = m_opens.fetch_add(1, std::memory_order_relaxed) + 1;
        if (opens > 1)
        {
            m_reconnects.fetch_add(1, std::memory_order_relaxed);
        }
        // Fresh epoch on (re)connect: any pre-connect PCM is flushed and never
        // replayed. No-op when the sink was already connected.
        m_sink.setConnected(true);
        if (m_config.log)
        {
            std::printf("[pico] connected: %s (%u Hz, %u ch)\n", result.deviceName.c_str(),
                        m_device->sampleRate(), m_device->channels());
        }
    }

    if (m_device->needsReopen())
    {
        if (m_config.log)
        {
            std::printf("[pico] endpoint changed -> re-discover\n");
        }
        closeDevice();
        return false;
    }

    if (!m_device->waitForRenderReady(50))
    {
        if (m_device->needsReopen())
        {
            if (m_config.log)
            {
                std::printf("[pico] endpoint removed\n");
            }
            closeDevice();
        }
        return false;
    }

    unsigned available = m_device->availableRenderFrames();
    if (m_device->needsReopen())
    {
        closeDevice();
        return false;
    }
    if (available == 0)
    {
        return false;
    }
    if (available > m_maxFrames)
    {
        available = static_cast<unsigned>(m_maxFrames);
    }

    // Drain only what the sink actually holds; the remainder is exact silence.
    // Read in bounded chunks (the feeder block size) so no assumption is made
    // about the ASIO callback block size or the WASAPI period.
    const std::size_t want = std::min<std::size_t>(available, m_sink.availableFrames());
    std::size_t got = 0;
    const std::size_t chunkFrames = std::max<std::size_t>(1, m_config.readChunkFrames);
    while (got < want)
    {
        const std::size_t chunk = std::min<std::size_t>(chunkFrames, want - got);
        if (!m_sink.readMono(m_mono.data() + got, chunk, m_config.staleThresholdFrames))
        {
            break;
        }
        got += chunk;
    }

    // The Pico firmware computes (left >> 1) + (right >> 1); duplicating the
    // processed mono to both channels reconstructs the original signal exactly.
    for (unsigned i = 0; i < available; ++i)
    {
        const float sample = (i < got) ? m_mono[i] : 0.0f;
        m_stereo[static_cast<std::size_t>(i) * 2u] = sample;
        m_stereo[static_cast<std::size_t>(i) * 2u + 1u] = sample;
    }

    bool deviceLost = false;
    const bool submitted = m_device->submit(m_stereo.data(), available, &deviceLost);
    if (deviceLost)
    {
        if (m_config.log)
        {
            std::printf("[pico] device lost during render\n");
        }
        closeDevice();
        return false;
    }
    if (!submitted)
    {
        return false;
    }

    m_submitCalls.fetch_add(1, std::memory_order_relaxed);
    m_renderedFrames.fetch_add(available, std::memory_order_relaxed);
    m_silenceFrames.fetch_add(static_cast<std::uint64_t>(available - got),
                              std::memory_order_relaxed);
    if (got == 0)
    {
        m_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void PicoUsbWorker::run()
{
    while (m_running.load(std::memory_order_acquire))
    {
        (void)stepOnce();
        if (!m_deviceOpen.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(m_config.reconnectBackoff);
        }
    }
    if (m_device)
    {
        m_device->close();
        m_device->threadShutdown();
    }
}

PicoUsbWorker::Status PicoUsbWorker::status() const
{
    Status result;
    result.deviceOpen = m_deviceOpen.load(std::memory_order_acquire);
    result.opens = m_opens.load(std::memory_order_relaxed);
    result.failedOpens = m_failedOpens.load(std::memory_order_relaxed);
    result.reconnects = m_reconnects.load(std::memory_order_relaxed);
    result.deviceLost = m_deviceLost.load(std::memory_order_relaxed);
    result.submitCalls = m_submitCalls.load(std::memory_order_relaxed);
    result.renderedFrames = m_renderedFrames.load(std::memory_order_relaxed);
    result.silenceFrames = m_silenceFrames.load(std::memory_order_relaxed);
    result.underruns = m_underruns.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_statusMutex);
    result.deviceName = m_deviceName;
    result.lastError = m_lastError;
    return result;
}

} // namespace audient::pico
