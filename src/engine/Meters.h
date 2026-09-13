#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::engine
{

struct MeterSnapshot
{
    float peakAbs = 0.0f;
    float rmsAbs = 0.0f;
    float peakDb = -240.0f;
    float rmsDb = -240.0f;
    bool clipping = false;
};

class PeakRmsMeter
{
public:
    static constexpr std::size_t kMaxChannels = 2;

    void feed(const float* interleaved, std::size_t frames, std::size_t channels);
    void resetPeakHold();
    void clear();

    float peakAbs(std::size_t channel) const;
    float rmsAbs(std::size_t channel) const;
    float peakDb(std::size_t channel) const;
    float rmsDb(std::size_t channel) const;
    bool clipping(std::size_t channel) const;
    MeterSnapshot snapshot(std::size_t channel = 0) const;

private:
    std::array<std::atomic<float>, kMaxChannels> m_peakAbs{};
    std::array<std::atomic<float>, kMaxChannels> m_rmsAbs{};
    std::array<std::atomic<bool>, kMaxChannels> m_clipping{};
};

} // namespace audient::engine