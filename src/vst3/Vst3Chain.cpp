#include "vst3/Vst3Chain.h"

#include <cstring>
#include <memory>

namespace audient::vst3
{

Vst3Chain::Vst3Chain()
    : m_state(new State())
{
    m_retired.reserve(kMaxSlots);
}

void Vst3Chain::configure(std::size_t maxBlockSamples)
{
    m_adapterLeft.assign(maxBlockSamples, 0.0f);
    m_adapterRight.assign(maxBlockSamples, 0.0f);
    m_adapterMono.assign(maxBlockSamples, 0.0f);
}

Vst3Chain::~Vst3Chain()
{
    quiescentShutdown();
}

void Vst3Chain::quiescentShutdown()
{
    if (m_quiescent)
    {
        return;
    }
    m_quiescent = true;
    State* live = m_state.exchange(nullptr, std::memory_order_acq_rel);
    delete live;
    for (State* state : m_retired)
    {
        delete state;
    }
    m_retired.clear();
}

bool Vst3Chain::publish(std::vector<Slot> slots, bool wholeChainBypass, std::uint64_t* floorOut)
{
    if (m_quiescent || slots.size() > kMaxSlots)
    {
        if (floorOut != nullptr)
        {
            *floorOut = 0;
        }
        return false;
    }
    std::unique_ptr<State> fresh(new State());
    fresh->slots = std::move(slots);
    fresh->wholeChainBypass = wholeChainBypass;
    m_retired.reserve(m_retired.size() + 1);

    State* retired = m_state.exchange(fresh.release(), std::memory_order_acq_rel);
    const std::uint64_t floor = m_processedBlocks.load(std::memory_order_acquire);
    retired->retiredAtBlocks = floor;
    m_retired.push_back(retired);
    if (floorOut != nullptr)
    {
        *floorOut = floor;
    }
    return true;
}

void Vst3Chain::reap()
{
    if (m_quiescent)
    {
        return;
    }
    const std::uint64_t now = m_processedBlocks.load(std::memory_order_acquire);
    auto it = m_retired.begin();
    while (it != m_retired.end())
    {
        State* state = *it;
        if (now >= state->retiredAtBlocks + kGraceBlocks)
        {
            delete state;
            it = m_retired.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool Vst3Chain::settled() const
{
    const std::uint64_t now = m_processedBlocks.load(std::memory_order_acquire);
    for (const State* state : m_retired)
    {
        if (now < state->retiredAtBlocks + kGraceBlocks)
        {
            return false;
        }
    }
    return true;
}

std::uint64_t Vst3Chain::blockCounter() const
{
    return m_processedBlocks.load(std::memory_order_acquire);
}

std::uint32_t Vst3Chain::totalLatencySamples() const
{
    State* state = m_state.load(std::memory_order_acquire);
    if (state == nullptr || state->wholeChainBypass)
    {
        return 0;
    }
    std::uint32_t total = 0;
    for (const Slot& slot : state->slots)
    {
        if (slot.processor == nullptr || slot.bypass)
        {
            continue;
        }
        total += slot.processor->latencySamples();
    }
    return total;
}

std::uint32_t Vst3Chain::chainLatencyQuery(void* context)
{
    auto* chain = static_cast<Vst3Chain*>(context);
    return chain != nullptr ? chain->totalLatencySamples() : 0u;
}

void Vst3Chain::processMono(const float* input, float* output, std::size_t frames, void* context)
{
    auto* self = static_cast<Vst3Chain*>(context);
    if (self == nullptr)
    {
        return;
    }
    self->m_processedBlocks.fetch_add(1, std::memory_order_relaxed);

    State* state = self->m_state.load(std::memory_order_acquire);
    if (state == nullptr || state->wholeChainBypass)
    {
        return; // in-place path: input == output, chain writes nothing
    }
    if (frames == 0)
    {
        return;
    }
    // Empty/all-bypassed snapshots are a no-op under the in-place passthrough
    // contract (the host pre-materialized the source; a non-aliased call must
    // NOT write the output).
    bool anyActive = false;
    for (const Slot& slot : state->slots)
    {
        if (slot.processor != nullptr && !slot.bypass)
        {
            anyActive = true;
            break;
        }
    }
    if (!anyActive)
    {
        return;
    }

    // Materialize the source into the output buffer once so every slot reads
    // the RUNNING value (slot N sees slot N-1's result) regardless of whether
    // the call aliases input==output (engine/routing in-place, no copy) or uses
    // separate buffers. Empty/bypassed handling above preserves the legacy
    // non-aliased "do not write" pin for the no-processing case.
    if (input != output && input != nullptr && output != nullptr)
    {
        std::memcpy(output, input, frames * sizeof(float));
    }

    const bool canAdapt = frames <= self->m_adapterLeft.size() && frames <= self->m_adapterRight.size();
    for (const Slot& slot : state->slots)
    {
        if (slot.processor == nullptr || slot.bypass)
        {
            continue;
        }
        // VST-006: the effective layout is the layout the processor negotiated
        // (prepare() may have fallen back to the opposite of what was requested,
        // e.g. a stereo-only effect on the mono mic chain).
        if (slot.processor->layout() == BusLayout::Mono)
        {
            Vst3Processor::chainProcess(output, output, frames, slot.processor);
            continue;
        }
        if (!canAdapt)
        {
            // No mono->stereo staging (chain unconfigured or oversized call):
            // the running value passes through untouched for this slot.
            continue;
        }
        // VST-006 mono drive of a stereo processor: dup the running mono onto
        // L/R staging, run the stereo processor, downmix to the mono output.
        std::memcpy(self->m_adapterLeft.data(), output, frames * sizeof(float));
        std::memcpy(self->m_adapterRight.data(), output, frames * sizeof(float));
        Vst3Processor::chainProcessStereo(self->m_adapterLeft.data(), self->m_adapterRight.data(),
                                          self->m_adapterLeft.data(), self->m_adapterRight.data(), frames,
                                          slot.processor);
        for (std::size_t i = 0; i < frames; ++i)
        {
            output[i] = 0.5f * (self->m_adapterLeft[i] + self->m_adapterRight[i]);
        }
    }
}

void Vst3Chain::processStereo(const float* leftIn, const float* rightIn, float* leftOut, float* rightOut,
                              std::size_t frames, void* context)
{
    auto* self = static_cast<Vst3Chain*>(context);
    if (self == nullptr)
    {
        return;
    }
    self->m_processedBlocks.fetch_add(1, std::memory_order_relaxed);

    State* state = self->m_state.load(std::memory_order_acquire);
    if (state == nullptr || state->wholeChainBypass)
    {
        return; // in-place path: outputs == inputs, chain writes nothing
    }
    if (frames == 0)
    {
        return;
    }
    bool anyActive = false;
    for (const Slot& slot : state->slots)
    {
        if (slot.processor != nullptr && !slot.bypass)
        {
            anyActive = true;
            break;
        }
    }
    if (!anyActive)
    {
        return;
    }

    if (leftOut != leftIn && leftIn != nullptr && leftOut != nullptr)
    {
        std::memcpy(leftOut, leftIn, frames * sizeof(float));
    }
    if (rightOut != rightIn && rightIn != nullptr && rightOut != nullptr)
    {
        std::memcpy(rightOut, rightIn, frames * sizeof(float));
    }

    const bool canAdapt = frames <= self->m_adapterMono.size();
    for (const Slot& slot : state->slots)
    {
        if (slot.processor == nullptr || slot.bypass)
        {
            continue;
        }
        if (slot.processor->layout() == BusLayout::Stereo)
        {
            Vst3Processor::chainProcessStereo(leftOut, rightOut, leftOut, rightOut, frames, slot.processor);
            continue;
        }
        if (!canAdapt || leftOut == nullptr || rightOut == nullptr)
        {
            // No stereo->mono staging (or missing running channel): slot is a
            // no-op; the running stereo passes through.
            continue;
        }
        // VST-006 stereo drive of a mono processor: downmix L/R to mono,
        // run the mono processor, duplicate the result back to L/R.
        for (std::size_t i = 0; i < frames; ++i)
        {
            self->m_adapterMono[i] = 0.5f * (leftOut[i] + rightOut[i]);
        }
        Vst3Processor::chainProcess(self->m_adapterMono.data(), self->m_adapterMono.data(), frames, slot.processor);
        for (std::size_t i = 0; i < frames; ++i)
        {
            leftOut[i] = self->m_adapterMono[i];
            rightOut[i] = self->m_adapterMono[i];
        }
    }
}

} // namespace audient::vst3