#pragma once

#include "asio/AsioBackend.h"
#include "asio/StreamFade.h"
#include "engine/EngineGraph.h"
#include "engine/EngineStreamData.h"
#include "engine/SyntheticDownlink.h"
#include "transport/TransportLinks.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace audient::asio
{

class AsioStreamBridge
{
public:
    explicit AsioStreamBridge(AsioBackend& backend);

    void attach(engine::EngineGraph& graph, std::size_t maxBlockSamples);
    void enableSyntheticDownlink(engine::SyntheticDownlink* source);
    void setUplinkTransport(transport::UplinkTransport* uplink);
    void setDownlinkTransport(transport::DownlinkTransport* downlink);
    void detach();

    void configureStreamFade(std::size_t fadeSamples);
    void requestFadeIn();
    void requestFadeOut();
    bool fadeIsMuted() const;
    bool fadeIsFull() const;

    const std::vector<float>& micUplinkBuffer() const;

    // Diagnostic peak of the processed mic uplink for the last processed block.
    // Written lock-free from the realtime callback, readable from UI/control threads.
    float micUplinkPeak() const;

private:
    static void onAsioCallback(const AsioCallbackInfo& info, void* context);
    void handle(const AsioCallbackInfo& info);
    void fillDownlink(std::size_t frames, const float*& left, const float*& right);

    AsioBackend& m_backend;
    engine::EngineGraph* m_graph = nullptr;
    engine::SyntheticDownlink* m_synthetic = nullptr;
    transport::UplinkTransport* m_uplinkTransport = nullptr;
    transport::DownlinkTransport* m_downlinkTransport = nullptr;

    StreamFade m_streamFade;

    std::vector<float> m_micUplink;
    std::vector<float> m_stageLeft;
    std::vector<float> m_stageRight;
    std::vector<float> m_downlinkScratch;

    std::atomic<float> m_micUplinkPeak{0.0f};
};

} // namespace audient::asio