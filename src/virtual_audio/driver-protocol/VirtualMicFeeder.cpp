#include "virtual_audio/driver-protocol/VirtualMicFeeder.h"

namespace audient::virtual_audio
{

VirtualMicFeeder::VirtualMicFeeder(VirtualCaptureEndpoint& endpoint, std::size_t blockFrames)
    : m_endpoint(endpoint)
    , m_block(blockFrames, 0.0f)
{
}

void VirtualMicFeeder::attachSink(IDriverCaptureSink* sink, std::uint64_t controlPlaneGeneration)
{
    if (sink == nullptr)
    {
        detachSink();
        return;
    }
    // Fresh epoch on (re)attach: use the provided control-plane CONNECT
    // generation when present (so the app and driver agree on the ring
    // generation and the kernel epoch is NOT clobbered), otherwise bump our own
    // per-attach epoch. Either way the sink flushes any old-epoch audio and the
    // endpoint is reset so the first pushed blocks are CURRENT engine output.
    if (controlPlaneGeneration != 0u)
    {
        m_generation.store(controlPlaneGeneration, std::memory_order_relaxed);
    }
    else
    {
        m_generation.fetch_add(1, std::memory_order_relaxed);
    }
    m_sequence.store(0, std::memory_order_relaxed);
    m_endpoint.reset();
    m_sink.store(sink, std::memory_order_release);
}

void VirtualMicFeeder::setControlPlaneGeneration(std::uint64_t generation)
{
    if (generation == 0u)
    {
        return;
    }
    // The driver re-initialized the region to a fresh epoch (FLUSH) or a new
    // CONNECT occurred (reconnect). Adopt the new generation and reopen a fresh
    // endpoint epoch so the next pushed block is CURRENT engine output. Because
    // the adopted generation equals the driver's, the consumer (kernel) reset
    // epoch is not re-flushed by the producer.
    m_generation.store(generation, std::memory_order_relaxed);
    m_sequence.store(0, std::memory_order_relaxed);
    m_endpoint.reset();
}

void VirtualMicFeeder::detachSink()
{
    m_sink.store(nullptr, std::memory_order_release);
}

bool VirtualMicFeeder::sinkAttached() const
{
    return m_sink.load(std::memory_order_acquire) != nullptr;
}

std::uint64_t VirtualMicFeeder::generation() const
{
    return m_generation.load(std::memory_order_relaxed);
}

bool VirtualMicFeeder::tick(std::size_t staleThresholdFrames)
{
    IDriverCaptureSink* sink = m_sink.load(std::memory_order_acquire);
    if (sink == nullptr)
    {
        m_noSinkTicks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    m_pulls.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t generation = m_generation.load(std::memory_order_relaxed);

    // Only REAL engine blocks are ever pushed. When the transport has nothing
    // fresh, the endpoint fills synthesized digital silence and reports it —
    // the feeder must NOT forward invented silence (the driver's empty-read
    // rule already gives OS clients silence).
    const bool fresh = m_endpoint.captureMonoFresh(m_block.data(), m_block.size(), staleThresholdFrames);
    if (!fresh)
    {
        m_engineGaps.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::uint64_t sequence = m_sequence.fetch_add(1, std::memory_order_relaxed);
    if (!sink->writeMono(m_block.data(), m_block.size(), generation, sequence))
    {
        m_rejected.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    m_pushed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void VirtualMicFeeder::setStaleThresholdFrames(std::size_t frames)
{
    m_staleThresholdFrames = frames;
}

void VirtualMicFeeder::start(std::chrono::microseconds pollInterval)
{
    if (m_thread.joinable())
    {
        return;
    }
    m_pollInterval = pollInterval;
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&VirtualMicFeeder::run, this);
}

void VirtualMicFeeder::requestStop()
{
    m_running.store(false, std::memory_order_release);
}

void VirtualMicFeeder::stop()
{
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

bool VirtualMicFeeder::running() const
{
    return m_running.load(std::memory_order_acquire);
}

VirtualMicFeeder::Snapshot VirtualMicFeeder::snapshot() const
{
    Snapshot result;
    result.generation = m_generation.load(std::memory_order_relaxed);
    result.sequence = m_sequence.load(std::memory_order_relaxed);
    result.pulls = m_pulls.load(std::memory_order_relaxed);
    result.pushedBlocks = m_pushed.load(std::memory_order_relaxed);
    result.rejectedBlocks = m_rejected.load(std::memory_order_relaxed);
    result.engineGaps = m_engineGaps.load(std::memory_order_relaxed);
    result.noSinkTicks = m_noSinkTicks.load(std::memory_order_relaxed);
    return result;
}

VirtualMicFeeder::~VirtualMicFeeder()
{
    stop();
}

void VirtualMicFeeder::run()
{
    const std::size_t threshold = m_staleThresholdFrames;

    // Adaptive pacing (see the header note): while a realtime producer is active
    // the drain MUST keep up with the 750 block/s stream; OS sleep granularity
    // (>= ~1.5 ms) is coarser than the 1.33 ms block period, so the worker cannot
    // rely on sleeping between blocks. When data flows, tick() succeeds and we
    // loop with only a short yield; when the endpoint is empty we allow a small
    // bounded number of fast idle ticks and then back off by pollInterval.
    constexpr int kMaxIdleTicks = 2048;
    int idleTicks = 0;
    while (m_running.load(std::memory_order_acquire))
    {
        if (tick(threshold))
        {
            idleTicks = 0;
            continue;
        }
        if (++idleTicks >= kMaxIdleTicks)
        {
            idleTicks = 0;
            std::this_thread::sleep_for(m_pollInterval);
        }
        else
        {
            // Yield (not sleep): keeps the poll far above the realtime block rate
            // while letting other ready threads run. Sleep(0)/yield is ~sub-ms,
            // independent of the scheduler timer tick.
            std::this_thread::yield();
        }
    }
}

} // namespace audient::virtual_audio