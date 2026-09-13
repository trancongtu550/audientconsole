#pragma once

#include "engine/AudioRamp.h"
#include "engine/EngineStreamData.h"
#include "engine/GraphConfig.h"
#include "engine/GraphConfigSnapshot.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audient::engine
{

class EngineGraph
{
public:
    explicit EngineGraph(std::size_t maxBlockSamples);

    std::size_t maxBlockSamples() const;

    void publishConfig(const EngineConfig& config); // control thread; applied at the next block boundary
    EngineConfig configSnapshot() const;

    using ChainProcess = void (*)(const float* input, float* output, std::size_t frames, void* context);
    using StereoChainProcess = void (*)(const float* leftIn, const float* rightIn, float* leftOut, float* rightOut,
                                        std::size_t frames, void* context);
    void setMicChain(ChainProcess process, void* context);
    void setOutputChain(StereoChainProcess process, void* context);

    void process(EngineStreamData& data);
    std::uint64_t processedBlocks() const;

private:
    void applyConfig(const EngineConfig& config);

    std::size_t m_maxBlock = 0;
    EngineConfigSnapshot m_config;
    bool m_applied = false;
    EngineConfig m_appliedConfig;

    AudioRamp m_inputGain;
    AudioRamp m_micSendGain;
    AudioRamp m_micMonitorGain;
    AudioRamp m_downlinkGain;
    AudioRamp m_physicalOutputGain;

    bool m_polarityInvert = false;
    bool m_panicMute = false;

    ChainProcess m_micChain = nullptr;
    void* m_micChainContext = nullptr;
    StereoChainProcess m_outputChain = nullptr;
    void* m_outputChainContext = nullptr;

    std::vector<float> m_micProcessed;
    std::vector<float> m_monitorBuffer;
    std::vector<float> m_outLeft;
    std::vector<float> m_outRight;

    std::atomic<std::uint64_t> m_processedBlocks{0};
};

float clampToUnit(float value);

} // namespace audient::engine