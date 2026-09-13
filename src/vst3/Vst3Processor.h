#pragma once

#include "vst3/IParameterEditSink.h"
#include "vst3/ParameterEditQueue.h"
#include "vst3/StreamingParameterChanges.h"
#include "vst3/Vst3StateStream.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace audient::vst3
{

enum class BusLayout
{
    Mono,
    Stereo,
};

class Vst3Processor : public IParameterEditSink
{
public:
    enum class PrepareResult
    {
        Ok,
        Failed,
    };

    ~Vst3Processor();
    Vst3Processor(const Vst3Processor&) = delete;
    Vst3Processor& operator=(const Vst3Processor&) = delete;

    PrepareResult prepare(double sampleRate, long maxBlockSamples, BusLayout layout = BusLayout::Mono,
                          Steinberg::FUnknown* hostContext = nullptr);
    bool valid() const;
    std::uint32_t latencySamples() const;

    // VST-006: the layout actually negotiated with the plug-in. When the
    // requested layout is rejected (e.g. a stereo-only effect in the mono mic
    // chain), prepare() retries the opposite arrangement and reports it here.
    BusLayout layout() const { return m_layout; }

    // VST-018: deterministic trace of the prepare-time negotiation (bus
    // discovery, setBusArrangements arguments, retained arrangements). Used by
    // the compatibility investigation; valid even when prepare() throws.
    const std::string& prepareTrace() const { return m_prepareTrace; }

    // VST-016: re-query the plug-in's reported latency (getLatencySamples) on
    // the control/UI thread. Returns true when the reported value changed from
    // the previously cached value, in which case the caller should rebuild the
    // prepared chain snapshot safely on the control thread (Vst3Chain::publish).
    // Never call from the audio callback.
    bool refreshLatencySamples();
    const std::string& lastPrepareError() const;

    // Borrowed component (the same FUnknown the processor was created from).
    // Only for UI/editor/scan code on the UI thread; never the audio callback.
    Steinberg::FUnknown* component() const;

    // VST-009: UI/controller thread queues a normalized parameter change. The
    // audio callback drains the bounded queue and delivers it to the plug-in as
    // inputParameterChanges. Non-blocking; returns false when the queue is full
    // (edit dropped and counted). Never call from the audio callback.
    bool enqueueParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue normalizedValue);

    // IParameterEditSink: routes host-context performEdit into the same queue.
    void onParameterEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override;

    // Diagnostics/test counters (relaxed).
    std::uint64_t parameterDrops() const;
    std::uint64_t parameterDelivered() const;

    // VST-012: capture/restore the plug-in's component state as an opaque,
    // bounded, checksummed blob via IComponent::getState/setState.
    //  - saveState: returns true on success; outBlob is the persisted wrapper
    //    (magic/version/size/checksum/payload). When a plug-in writes more than
    //    kMaxStateBytes, save fails (overflow) and the blob is rejected.
    //  - restoreState: validates the blob (magic, version, length, checksum)
    //    and the kMaxStateBytes bound before calling setState; a malformed or
    //    oversized blob is rejected without touching the processor.
    // Control/UI-thread only (session/editor code); never the audio callback.
    bool saveState(std::vector<std::uint8_t>& outBlob);
    bool restoreState(const std::vector<std::uint8_t>& blob);

    static void chainProcess(const float* input, float* output, std::size_t frames, void* context);
    static void chainProcessStereo(const float* leftIn, const float* rightIn, float* leftOut, float* rightOut,
                                   std::size_t frames, void* context);

private:
    friend class Vst3Host;
    explicit Vst3Processor(Steinberg::FUnknown* component);

    void teardown();
    void deliverParameterChanges(); // drains pending edits into m_streamChanges (audio thread)

    ParameterEditQueue m_pendingEdits;
    static constexpr std::size_t kMaxDrainPerBlock = 32;
    std::array<ParameterEditQueue::Edit, kMaxDrainPerBlock> m_drainBuffer{};
    StreamingParameterChanges m_streamChanges;
    std::atomic<std::uint64_t> m_parameterDrops{0};
    std::atomic<std::uint64_t> m_parameterDelivered{0};

    Steinberg::FUnknown* m_component = nullptr;
    Steinberg::FUnknown* m_hostContext = nullptr; // addRef'd in prepare, released in teardown
    Steinberg::Vst::IComponent* m_componentIf = nullptr;
    Steinberg::Vst::IAudioProcessor* m_processorIf = nullptr;

    Steinberg::Vst::ProcessSetup m_setup{};
    Steinberg::Vst::ProcessData m_processData{};
    Steinberg::Vst::AudioBusBuffers m_inputBuffer{};
    Steinberg::Vst::AudioBusBuffers m_outputBuffer{};
    Steinberg::Vst::Sample32* m_inChannel = nullptr;
    Steinberg::Vst::Sample32* m_outChannel = nullptr;
    Steinberg::Vst::Sample32* m_inChannels[2] = {};
    Steinberg::Vst::Sample32* m_outChannels[2] = {};

    std::vector<float> m_staging;
    std::vector<float> m_stagingLeft;
    std::vector<float> m_stagingRight;
    std::uint32_t m_maxBlock = 0;
    std::uint32_t m_latency = 0;
    BusLayout m_layout = BusLayout::Mono;
    bool m_valid = false;
    std::string m_lastPrepareError;
    std::string m_prepareTrace;
};

} // namespace audient::vst3