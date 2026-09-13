#include "engine/Meters.h"

#include <algorithm>
#include <cmath>

namespace audient::engine
{

void PeakRmsMeter::feed(const float* interleaved, std::size_t frames, std::size_t channels)
{
    if (interleaved == nullptr || frames == 0 || channels == 0 || channels > kMaxChannels)
    {
        return;
    }

    double sumSquares[kMaxChannels]{};
    float localPeaks[kMaxChannels]{};
    bool localClip[kMaxChannels]{};

    for (std::size_t i = 0; i < frames; ++i)
    {
        for (std::size_t c = 0; c < channels; ++c)
        {
            const float value = interleaved[i * channels + c];
            const float absValue = std::fabs(value);
            localPeaks[c] = std::max(localPeaks[c], absValue);
            sumSquares[c] += static_cast<double>(value) * static_cast<double>(value);
            if (value >= 1.0f || value <= -1.0f)
            {
                localClip[c] = true;
            }
        }
    }

    for (std::size_t c = 0; c < channels; ++c)
    {
        const float rms = static_cast<float>(std::sqrt(sumSquares[c] / static_cast<double>(frames)));
        m_rmsAbs[c].store(rms, std::memory_order_relaxed);
        m_peakAbs[c].store(localPeaks[c], std::memory_order_relaxed);
        m_clipping[c].store(localClip[c], std::memory_order_relaxed);
    }
}

void PeakRmsMeter::clear()
{
    for (std::size_t c = 0; c < kMaxChannels; ++c)
    {
        m_peakAbs[c].store(0.0f, std::memory_order_relaxed);
        m_rmsAbs[c].store(0.0f, std::memory_order_relaxed);
        m_clipping[c].store(false, std::memory_order_relaxed);
    }
}

void PeakRmsMeter::resetPeakHold()
{
    for (std::size_t c = 0; c < kMaxChannels; ++c)
    {
        m_peakAbs[c].store(0.0f, std::memory_order_relaxed);
        m_clipping[c].store(false, std::memory_order_relaxed);
    }
}

float PeakRmsMeter::peakAbs(std::size_t channel) const
{
    if (channel >= kMaxChannels)
    {
        return 0.0f;
    }
    return m_peakAbs[channel].load(std::memory_order_relaxed);
}

float PeakRmsMeter::rmsAbs(std::size_t channel) const
{
    if (channel >= kMaxChannels)
    {
        return 0.0f;
    }
    return m_rmsAbs[channel].load(std::memory_order_relaxed);
}

float PeakRmsMeter::peakDb(std::size_t channel) const
{
    return 20.0f * std::log10f(peakAbs(channel) + 1e-12f);
}

float PeakRmsMeter::rmsDb(std::size_t channel) const
{
    return 20.0f * std::log10f(rmsAbs(channel) + 1e-12f);
}

bool PeakRmsMeter::clipping(std::size_t channel) const
{
    if (channel >= kMaxChannels)
    {
        return false;
    }
    return m_clipping[channel].load(std::memory_order_relaxed);
}

MeterSnapshot PeakRmsMeter::snapshot(std::size_t channel) const
{
    MeterSnapshot result;
    result.peakAbs = peakAbs(channel);
    result.rmsAbs = rmsAbs(channel);
    result.peakDb = 20.0f * std::log10f(result.peakAbs + 1e-12f);
    result.rmsDb = 20.0f * std::log10f(result.rmsAbs + 1e-12f);
    result.clipping = clipping(channel);
    return result;
}

} // namespace audient::engine