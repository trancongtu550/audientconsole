#pragma once

#include "asio/AsioBackend.h"
#include "asio/StreamFade.h"
#include "channel/ChannelTypes.h"
#include "engine/AudioRamp.h"
#include "engine/EngineGraph.h"
#include "engine/GraphConfigSnapshot.h"
#include "engine/Meters.h"
#include "engine/SyntheticDownlink.h"
#include "routing/RoutingCore.h"
#include "routing/RoutingTypes.h"
#include "transport/TransportLinks.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audient::transport
{
class VirtualCaptureTransport;
}

namespace audient::asio
{

// Slice P + Phase B B2 — ASIO <-> RoutingCore adapter (project guidelines §4, §7, §8).
//
// Maps the configured ASIO channel plan onto the routing core's LOGICAL buses
// and completes the ASIO render branch:
//
//   inputs   : for each configured input binding (runtime slot ch), ASIO
//              request slot ch -> RoutingCore runtime slot ch -> that slot's
//              input chain -> inputProcessed[ch] (+ inputMonitorFeed[ch]).
//              The slot selected for the virtual mic (slot 0 in Phase B) is
//              also faded and published to the capture transport.
//   downlink : Windows/synthetic/system-mix source -> PlaybackBus L/R ->
//              output chain -> outputProcessed -> physical Output 1/2.
//   monitor  : only slot 0's processed-monitor feed is MIXED into the physical
//              Output 1/2 at the render branch (the canonical "Processed mic
//              monitor | ASIO render branch" path). Other slots' feeds exist
//              per channel (independent raw/processed/monitor buffers so B4
//              meter taps and the Phase C mixer have correct seams) but are
//              NOT mixed in B2. RoutingCore stays pure: it only taps feeds;
//              this adapter owns the mix/gain stage.
//
// The number of live input channels and the ASIO-index -> runtime-slot mapping
// are fixed by the configured ChannelPlan binding table (AsioChannelMap) at
// attach() time. The adapter binds by REQUEST SLOT (request slot == runtime
// slot); it never re-derives identity/index on the audio thread.
//
// Slice Q1 — virtual capture transport: the processed mic uplink staging
// (post stream-fade) is PUBLISHED into a transport::VirtualCaptureTransport
// (setCaptureTransport).
//
// Realtime contract (AGENTS §7): processAsio() performs no allocation, no
// locking, no filesystem/registry/console I/O, no plug-in create/destroy, no
// graph rebuild, no latency query/reconfigure. It only binds/copies buffers,
// runs the prepared chain hooks, ramps gains, and mixes. All per-slot staging
// is sized at construction/attach (maxBlockSamples x routing::kMaxInputChannels);
// a block larger than max is zeroed+ignored. Chain hook changes belong to the
// control thread BEFORE streaming starts.
class AsioRoutingAdapter
{
public:
    // Render-branch state: control-thread published, revision-snapped at block
    // boundaries like engine::EngineGraph. Gains are linear.
    //
    // The MONITOR output (the PRIMARY requested ASIO pair, Output 1/2) keeps the
    // established mix: processed-mic monitor (slot 0) + processed downlink, with
    // monitor (mic feed), downlink and final output gain/mute stages plus an
    // added dim stage on the final Monitor level.
    //
    // The HEADPHONE output (the SECONDARY requested ASIO pair, Output 3/4, when
    // present) is an INDEPENDENT software-controlled stereo bus: same program
    // sources (processed-mic monitor routed via micToHeadphones, processed
    // downlink routed via downlinkToHeadphones) but its OWN gain/mute. Changing
    // Monitor or Headphone level/mute/dim never changes the other bus and never
    // touches the Mic -> VST chain -> VirtualMicFeeder capture path (published
    // separately from m_micUplink).
    struct RenderConfig
    {
        float monitorGain = 1.0f;          // processed-mic-monitor feed level (into Monitor) - legacy CH0 fallback
        float downlinkGain = 1.0f;         // processed-downlink level (into both routed buses)
        float physicalOutputGain = 1.0f;   // MONITOR bus final gain
        float headphoneGain = 1.0f;        // HEADPHONE bus final gain
        bool monitorMute = false;          // mic monitor routed into the MONITOR bus
        bool downlinkMute = false;
        bool physicalOutputMute = false;   // MONITOR bus mute
        bool headphoneMute = true;         // HEADPHONE bus mute (start safe)
        bool monitorDim = false;           // extra attenuation on the MONITOR bus
        float monitorDimGain = 0.1f;       // -20 dB while dim is engaged
        bool micToHeadphones = false;      // processed-mic monitor routed into HEADPHONES
        bool downlinkToHeadphones = true;  // processed downlink routed into HEADPHONES
        bool panicMute = false;            // abrupt hard mute of both render buses
        bool outputMono = false;           // MONO fold: 0.5*(L+R) -> both L/R (before clamp/output ramp)
        std::uint64_t revision = 0;
    };

    explicit AsioRoutingAdapter(AsioBackend& backend, std::size_t maxBlockSamples);

    AsioRoutingAdapter(const AsioRoutingAdapter&) = delete;
    AsioRoutingAdapter& operator=(const AsioRoutingAdapter&) = delete;

    void attach();
    void detach();

    // --- Control-thread wiring (before streaming) -------------------------
    void setChannelInputChain(std::size_t channel, routing::RoutingCore::MonoChainFn fn, void* context);
    void setMicChain(routing::RoutingCore::MonoChainFn fn, void* context); // legacy: channel 0
    void setOutputChain(routing::RoutingCore::StereoChainFn fn, void* context);
    void setSourceLatency(routing::BusId bus, std::uint32_t samples);
    void setChannelInputChainLatency(std::size_t channel, routing::LatencyQueryFn fn, void* context);
    void setInputChainLatency(routing::LatencyQueryFn fn, void* context); // legacy: channel 0
    void setOutputChainLatency(routing::LatencyQueryFn fn, void* context);

    // Realtime-clean diagnostic taps (default null = zero cost when absent).
    // Invoked from processAsio() ON THE AUDIO THREAD: the tapped callback MUST
    // be realtime-safe (no allocation/locking/blocking/I/O - e.g. a bounded SPSC
    // push). Install on a control thread before streaming.
    //  - MicInputTap: the decoded mono ASIO input for runtime slot 0 exactly as
    //    it enters the routing core (immediately after the ASIO->float sample
    //    conversion; no processing applied).
    //  - MicUplinkTap: the processed mic uplink exactly as it is published to
    //    the virtual capture transport (post mic-chain, post stream-fade).
    using SampleTapFn = void (*)(const float* mono, std::size_t frames, void* context);
    void setMicInputTap(SampleTapFn fn, void* context);
    void setMicUplinkTap(SampleTapFn fn, void* context);

    void publishRenderConfig(const RenderConfig& config);
    RenderConfig renderConfig() const;

    void publishChannelRuntime(std::size_t channel, const channel::ChannelRuntimeSnapshot& state);
    channel::ChannelRuntimeSnapshot channelRuntime(std::size_t channel) const;

    void enableSyntheticDownlink(engine::SyntheticDownlink* source);
    void setDownlinkTransport(transport::DownlinkTransport* downlink);

    // Slice Q1: publish the processed mic uplink into the virtual capture
    // transport. Control-thread wiring, set before streaming. Each
    // requestFadeIn() resets the transport so every stream start begins a
    // fresh capture epoch (no stale pre-restart samples, AGENTS §11).
    void setCaptureTransport(transport::VirtualCaptureTransport* capture);

    void configureFade(std::size_t fadeSamples);
    void requestFadeIn();
    void requestFadeOut();
    bool fadeIsMuted() const;
    bool fadeIsFull() const;

    // De-pop startup diagnostics for the PHYSICAL output path (ASCII only, no
    // realtime logging). Valid on the control thread after the stream is live;
    // each requestFadeIn() re-arms the measurement window.
    struct DepopSnapshot
    {
        bool active = false;
        bool published = false;
        float firstLeft = 0.0f;
        float firstRight = 0.0f;
        float maxAbs = 0.0f;
        float maxJump = 0.0f;
        std::size_t mutedSamples = 0;
        std::size_t rampSamples = 0;
    };
    DepopSnapshot depopSnapshot() const;

    // Diagnostic calibration input (default disabled; NEVER armed by production
    // paths). armMicInputProbe() arms a ONE-SHOT wire: on the next processed
    // callback block of size `frames`, the physical input handed to the mic tap
    // + RoutingCore for runtime slot 0 is REPLACED with `burst` and the QPC tick
    // at the moment of injection is captured (micInputProbeQpc). The burst is
    // copied into an adapter-owned buffer at arm time (control thread).
    // Realtime-clean: one atomic exchange + a pointer swap + QPC.
    void armMicInputProbe(const float* burst, std::size_t frames);
    void disarmMicInputProbe();
    std::uint64_t micInputProbeQpc() const;

    // Diagnostic output-pair tone (safe channel identification). While armed,
    // the chosen output pair (0 = primary/Monitor pair at request slots 0/1,
    // 1 = secondary/Headphone pair at slots 2/3) is overwritten with a
    // LOW-LEVEL deterministic tone at `amplitudeLinear` (callers must keep it
    // well below full scale, e.g. 0.03 = -30 dBFS) with an on/off cadence so
    // the pair's physical destination (speakers vs headphones) can be named
    // safely. The other pair keeps its normal (default muted) mix. Control
    // thread only; the callback just reads the snapshot + advances a counter.
    void armOutputPairTone(std::uint32_t pair, float amplitudeLinear);
    void disarmOutputPairTone();

    // Control-thread diagnostics.
    routing::LatencySnapshot latencySnapshot() const;
    float micUplinkPeak() const;
    engine::MeterSnapshot inputRawMeter(std::size_t channel) const;
    engine::MeterSnapshot inputPostMeter(std::size_t channel) const;
    const std::vector<float>& debugMonitorSumBuffer() const { return m_monitorSum; }

    // Realtime entry (registered with the backend; also directly testable).
    void processAsio(const AsioCallbackInfo& info);

    std::size_t maxBlockSamples() const { return m_routing.maxBlockSamples(); }
    const routing::RoutingCore& routing() const { return m_routing; }
    std::size_t configuredInputChannels() const { return m_configuredInputs; }

    // Read-only per-slot staging taps for diagnostics/tests. Valid on the
    // control thread AFTER the stream has stopped (the callback is the only
    // writer). `channel` is a runtime slot; out-of-range reads clamp to slot 0.
    const std::vector<float>& inputRawBuffer(std::size_t channel) const;
    const std::vector<float>& inputProcessedBuffer(std::size_t channel) const;
    const std::vector<float>& inputMonitorFeedBuffer(std::size_t channel) const;

    // Legacy single-channel read accessors (runtime slot 0) kept for
    // compatibility with existing diagnostics/tests.
    const std::vector<float>& micRawBuffer() const { return m_inputRaw[0]; }
    const std::vector<float>& micProcessedBuffer() const { return m_inputProcessed[0]; }
    const std::vector<float>& micUplinkBuffer() const { return m_micUplink; }
    const std::vector<float>& outputProcessedLeftBuffer() const { return m_outProcessedL; }
    const std::vector<float>& outputProcessedRightBuffer() const { return m_outProcessedR; }
    const std::vector<float>& monitorScratchBuffer() const { return m_monitorFeed[0]; }

private:
    static void onAsioCallback(const AsioCallbackInfo& info, void* context);
    void fillDownlink(std::size_t frames, const float*& left, const float*& right);
    void zeroSlotSinks(std::size_t channel, std::size_t frames);
    void zeroUplink(std::size_t frames);

    AsioBackend& m_backend;
    bool m_attached = false;
    std::size_t m_maxBlock = 0;
    std::size_t m_configuredInputs = 1; // runtime slots processed this stream (set at attach)

    routing::RoutingCore m_routing;

    engine::SyntheticDownlink* m_synthetic = nullptr;
    transport::DownlinkTransport* m_downlinkTransport = nullptr;
    transport::VirtualCaptureTransport* m_captureTransport = nullptr;

    SampleTapFn m_micInputTap = nullptr;
    void* m_micInputTapContext = nullptr;
    SampleTapFn m_micUplinkTap = nullptr;
    void* m_micUplinkTapContext = nullptr;

    // One-shot calibration probe (see armMicInputProbe).
    std::atomic<bool> m_micProbeArmed{false};
    std::vector<float> m_micProbeBuf; // adapter-owned burst copy (filled on arm)
    std::size_t m_micProbeFrames = 0;
    std::atomic<std::uint64_t> m_micProbeQpc{0};

    engine::SeqLockSnapshot<RenderConfig> m_renderConfig;
    std::array<engine::SeqLockSnapshot<channel::ChannelRuntimeSnapshot>, routing::kMaxInputChannels> m_channelRuntime;
    std::array<channel::ChannelRuntimeSnapshot, routing::kMaxInputChannels> m_appliedChannelRuntime{};
    std::array<engine::AudioRamp, routing::kMaxInputChannels> m_virtualMicSendRamp;
    std::array<engine::AudioRamp, routing::kMaxInputChannels> m_localMonitorSendRamp;
    RenderConfig m_appliedRender{};
    bool m_configApplied = false;

    engine::AudioRamp m_monitorRamp;          // legacy CH0 mic-monitor feed (fallback when CH0 rev==0)
    engine::AudioRamp m_downlinkRamp;         // downlink feed (shared)
    engine::AudioRamp m_outputRamp;           // MONITOR bus final gain (incl. dim/mute)
    engine::AudioRamp m_headphoneRamp;        // HEADPHONE bus final gain (incl. mute)
    engine::AudioRamp m_micToHeadphonesRamp;  // mic-monitor feed into the HEADPHONE bus
    engine::AudioRamp m_outputMonoRamp;       // output mono-fold amount [0,1] (ramped mix, click-free)
    std::vector<float> m_monitorSum;          // local-monitor mono sum (preallocated maxBlock)

    // Diagnostic output-pair tone snapshot (safe channel identification).
    struct PairTone
    {
        bool active = false;
        std::uint32_t pair = 0;      // 0 = primary pair, 1 = secondary pair
        float amplitude = 0.0f;      // linear, callers keep well below full scale
    };
    engine::SeqLockSnapshot<PairTone> m_pairTone;
    std::uint64_t m_toneElapsedSamples = 0; // callback-only cumulative counter
    std::array<float, 32> m_toneTable{};    // one 1500 Hz cycle, filled at attach()

    // Preallocated per-slot staging (sized maxBlockSamples x routing::kMaxInputChannels at
    // construction). Independent buffers per runtime slot: never shared between channels.
    std::array<std::vector<float>, routing::kMaxInputChannels> m_inputRaw;       // pre-chain raw tap per slot
    std::array<std::vector<float>, routing::kMaxInputChannels> m_inputProcessed; // post-input-chain per slot
    std::array<engine::PeakRmsMeter, routing::kMaxInputChannels> m_inputRawMeters;
    std::array<engine::PeakRmsMeter, routing::kMaxInputChannels> m_inputPostMeters;
    std::array<std::vector<float>, routing::kMaxInputChannels> m_monitorFeed;    // processed-monitor feed per slot
    std::vector<float> m_micUplink;      // processed capture bus (mono; published + faded)
    std::vector<float> m_outProcessedL;  // downlink AFTER the output chain
    std::vector<float> m_outProcessedR;
    std::vector<float> m_outScratchL;    // RoutingCore physical-output staging
    std::vector<float> m_outScratchR;
    std::vector<float> m_stageLeft;      // downlink deinterleave / synthetic fill
    std::vector<float> m_stageRight;
    std::vector<float> m_downlinkScratch; // interleaved transport space (2 * maxBlock)

    StreamFade m_streamFade;
    std::atomic<float> m_micUplinkPeak{0.0f};

    // De-pop startup gate (physical outputs only; the virtual-mic/input/meter
    // path is deliberately NOT delayed). Armed by requestFadeIn(), which runs on
    // the control thread before ASIOStart; the callback then holds the physical
    // outputs at exact zero for a short priming window, then ramps 0 -> 1 over a
    // fixed sample count. This guarantees a fresh session can never emit a
    // transient unrelated to the program material. RT-owned after arming.
    std::size_t m_startupPrimeRemaining = 0;
    std::size_t m_depopMeasureRemaining = 0;
    bool m_depopMeasuredFirst = false;
    float m_depopFirstL = 0.0f;
    float m_depopFirstR = 0.0f;
    float m_depopMaxAbs = 0.0f;
    float m_depopMaxJump = 0.0f;
    float m_depopPrevL = 0.0f;
    float m_depopPrevR = 0.0f;
    std::size_t m_depopMutedSamples = 0;
    // Published once at the end of the measurement window (no per-sample atomic).
    std::atomic<bool> m_depopActive{false};
    std::atomic<bool> m_depopPublished{false};
    std::atomic<float> m_pubFirstL{0.0f};
    std::atomic<float> m_pubFirstR{0.0f};
    std::atomic<float> m_pubMaxAbs{0.0f};
    std::atomic<float> m_pubMaxJump{0.0f};
    std::atomic<unsigned> m_pubMuted{0u};
};

} // namespace audient::asio
