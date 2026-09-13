#include "vst3/Vst3Processor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace audient::vst3
{

Vst3Processor::Vst3Processor(Steinberg::FUnknown* component)
    : m_component(component)
{
}

Vst3Processor::~Vst3Processor()
{
    teardown();
}

bool Vst3Processor::valid() const
{
    return m_valid;
}

std::uint32_t Vst3Processor::latencySamples() const
{
    return m_latency;
}

bool Vst3Processor::refreshLatencySamples()
{
    if (m_processorIf == nullptr || !m_valid)
    {
        return false;
    }
    const std::uint32_t reported = m_processorIf->getLatencySamples();
    if (reported == m_latency)
    {
        return false;
    }
    m_latency = reported;
    return true;
}

Steinberg::FUnknown* Vst3Processor::component() const
{
    return m_component;
}

Vst3Processor::PrepareResult Vst3Processor::prepare(double sampleRate, long maxBlockSamples, BusLayout layout,
                                                    Steinberg::FUnknown* hostContext)
{
    m_lastPrepareError.clear();

    if (m_component == nullptr || maxBlockSamples <= 0)
    {
        m_lastPrepareError = "invalid component";
        return PrepareResult::Failed;
    }

    if (hostContext != nullptr)
    {
        hostContext->addRef();
        m_hostContext = hostContext;
    }

    // VST-018 instrumentation: deterministic prepare-time trace, captured
    // regardless of the outcome (including a plug-in-thrown exception) and
    // printed when AUDIENT_VST3_TRACE is set.
    m_prepareTrace.clear();
    const auto trace = [&](const std::string& line) {
        if (!m_prepareTrace.empty())
        {
            m_prepareTrace += '\n';
        }
        m_prepareTrace += line;
        // MSVC treats plain getenv as C4996 (warnings-as-errors); use _dupenv_s.
        static const bool kTraceToConsole = []() {
            char* buffer = nullptr;
            size_t length = 0;
            const errno_t err = _dupenv_s(&buffer, &length, "AUDIENT_VST3_TRACE");
            const bool enabled = err == 0 && buffer != nullptr;
            free(buffer);
            return enabled;
        }();
        if (kTraceToConsole)
        {
            std::printf("%s\n", line.c_str());
        }
    };
    const auto arrangementText = [](Steinberg::Vst::SpeakerArrangement arrangement) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "0x%llx(%s)",
                      static_cast<unsigned long long>(arrangement),
                      arrangement == Steinberg::Vst::SpeakerArr::kMono   ? "mono"
                      : arrangement == Steinberg::Vst::SpeakerArr::kStereo ? "stereo"
                                                                             : "other");
        return std::string(buffer);
    };
    const auto busTypeText = [](Steinberg::Vst::BusType type) { return type == Steinberg::Vst::kMain ? "main" : "aux"; };
    const auto resultText = [](Steinberg::tresult result) { return result == Steinberg::kResultOk ? "ok" : "false"; };
    std::vector<Steinberg::Vst::SpeakerArrangement> inputRetained;
    std::vector<Steinberg::Vst::SpeakerArrangement> outputRetained;

    // VST-018: every plug-in call below is contained so a throwing plug-in
    // (e.g. FabFilter Pro-Q 4 observed throwing std::length_error) cannot
    // escape into the host / create a startup crash loop; the exception is
    // recorded into the same trace that captured the negotiation steps.
    try
    {
    if (m_component->queryInterface(Steinberg::Vst::IComponent::iid.toTUID(), reinterpret_cast<void**>(&m_componentIf)) != Steinberg::kResultOk || m_componentIf == nullptr)
    {
        m_lastPrepareError = "component does not implement IComponent";
        return PrepareResult::Failed;
    }

    // Pass the host context (IHostApplication/IComponentHandler) so real plug-ins
    // can initialize instead of failing on a null context (VST-009).
    if (m_componentIf->initialize(hostContext) != Steinberg::kResultOk)
    {
        m_lastPrepareError = "initialize failed";
        return PrepareResult::Failed;
    }

    if (m_componentIf->queryInterface(Steinberg::Vst::IAudioProcessor::iid.toTUID(), reinterpret_cast<void**>(&m_processorIf)) != Steinberg::kResultOk || m_processorIf == nullptr)
    {
        m_lastPrepareError = "component does not implement IAudioProcessor";
        return PrepareResult::Failed;
    }

    // ---- VST-018: deterministic bus discovery BEFORE any arrangement request.
    // Do NOT assume one input + one output: Pro-Q 4 may expose input 0 = main,
    // input 1 = aux/sidechain, output 0 = main. ----
    const Steinberg::int32 inputBuses = m_componentIf->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    const Steinberg::int32 outputBuses = m_componentIf->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
    if (inputBuses < 1 || outputBuses < 1)
    {
        m_lastPrepareError = "component has no audio input/output bus";
        return PrepareResult::Failed;
    }

    const auto discoverBuses = [&](Steinberg::Vst::BusDirection direction, Steinberg::int32 count, const char* side,
                                   std::vector<Steinberg::Vst::SpeakerArrangement>& retained) {
        retained.assign(static_cast<std::size_t>(count), 0);
        for (Steinberg::int32 index = 0; index < count; ++index)
        {
            Steinberg::Vst::BusInfo info{};
            const Steinberg::tresult infoResult = m_componentIf->getBusInfo(Steinberg::Vst::kAudio, direction, index, info);
            Steinberg::Vst::SpeakerArrangement arrangement = 0;
            const Steinberg::tresult arrangementResult = m_processorIf->getBusArrangement(direction, index, arrangement);
            if (static_cast<std::size_t>(index) < retained.size())
            {
                retained[static_cast<std::size_t>(index)] = arrangement;
            }
            char line[256]{};
            std::snprintf(line, sizeof(line),
                          "[vst3trace] %s bus idx=%lld type=%s ch=%lld arrangement=%s flags=0x%x defaultActive=%d "
                          "(getBusInfo=%s getBusArrangement=%s)",
                          side, static_cast<long long>(index), busTypeText(info.busType),
                          static_cast<long long>(info.channelCount), arrangementText(arrangement).c_str(),
                          static_cast<unsigned>(info.flags),
                          (info.flags & Steinberg::Vst::BusInfo::kDefaultActive) != 0 ? 1 : 0,
                          resultText(infoResult), resultText(arrangementResult));
            trace(line);
        }
    };
    discoverBuses(Steinberg::Vst::kInput, inputBuses, "input", inputRetained);
    discoverBuses(Steinberg::Vst::kOutput, outputBuses, "output", outputRetained);

    // Build the arrangement vectors to pass to setBusArrangements with the
    // REAL bus counts. Main buses get the requested mono/stereo layout; aux
    // (sidechain) buses keep their retained arrangement as a valid value and
    // are never activated (bus 0 = main is the only activated audio input).
    const auto buildArrangements = [&](bool inputSide, Steinberg::int32 count,
                                       const std::vector<Steinberg::Vst::SpeakerArrangement>& retained,
                                       bool stereo) {
        std::vector<Steinberg::Vst::SpeakerArrangement> result(static_cast<std::size_t>(count));
        for (Steinberg::int32 index = 0; index < count; ++index)
        {
            Steinberg::Vst::BusInfo info{};
            m_componentIf->getBusInfo(Steinberg::Vst::kAudio, inputSide ? Steinberg::Vst::kInput : Steinberg::Vst::kOutput,
                                      index, info);
            const Steinberg::Vst::SpeakerArrangement wanted = stereo ? Steinberg::Vst::SpeakerArr::kStereo
                                                                     : Steinberg::Vst::SpeakerArr::kMono;
            if (info.busType == Steinberg::Vst::kMain)
            {
                result[static_cast<std::size_t>(index)] = wanted;
            }
            else if (static_cast<std::size_t>(index) < retained.size())
            {
                result[static_cast<std::size_t>(index)] = retained[static_cast<std::size_t>(index)];
            }
            else
            {
                result[static_cast<std::size_t>(index)] = wanted;
            }
        }
        return result;
    };

    const auto tryArrangements = [&](bool stereo) -> bool {
        const std::vector<Steinberg::Vst::SpeakerArrangement> inputs =
            buildArrangements(true, inputBuses, inputRetained, stereo);
        const std::vector<Steinberg::Vst::SpeakerArrangement> outputs =
            buildArrangements(false, outputBuses, outputRetained, stereo);
        // Log the exact call we make.
        std::string call = stereo ? "stereo" : "mono";
        call += " ins=[";
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            if (i != 0)
            {
                call += ",";
            }
            call += arrangementText(inputs[i]);
        }
        call += "] outs=[";
        for (std::size_t i = 0; i < outputs.size(); ++i)
        {
            if (i != 0)
            {
                call += ",";
            }
            call += arrangementText(outputs[i]);
        }
        call += "]";
        const Steinberg::tresult result =
            m_processorIf->setBusArrangements(const_cast<Steinberg::Vst::SpeakerArrangement*>(inputs.data()),
                                              inputBuses,
                                              const_cast<Steinberg::Vst::SpeakerArrangement*>(outputs.data()),
                                              outputBuses);
        trace("[vst3trace] setBusArrangements(" + call + ") -> " + resultText(result));
        return result == Steinberg::kResultOk;
    };

    // Evidence that the MAIN input AND output currently hold the given layout.
    const auto retainedMainHolds = [&](bool inputSide, Steinberg::Vst::SpeakerArrangement which) {
        const Steinberg::int32 count = inputSide ? inputBuses : outputBuses;
        const std::vector<Steinberg::Vst::SpeakerArrangement>& retained = inputSide ? inputRetained : outputRetained;
        for (Steinberg::int32 index = 0; index < count; ++index)
        {
            Steinberg::Vst::BusInfo info{};
            m_componentIf->getBusInfo(Steinberg::Vst::kAudio, inputSide ? Steinberg::Vst::kInput : Steinberg::Vst::kOutput,
                                      index, info);
            if (info.busType == Steinberg::Vst::kMain)
            {
                return static_cast<std::size_t>(index) < retained.size() &&
                       retained[static_cast<std::size_t>(index)] == which;
            }
        }
        return false;
    };

    // VST-006/VST-018 negotiation: try the REQUESTED layout first; if the
    // plug-in rejects it, re-query its retained arrangements (getBusArrangement
    // + getBusInfo again) and log the actual layout it holds. Retry the opposite
    // only when that retained evidence shows it supports it; never blind-retry.
    bool stereo = layout == BusLayout::Stereo;
    trace(std::string("[vst3trace] requested layout: ") + (stereo ? "stereo" : "mono"));
    if (tryArrangements(stereo))
    {
        m_layout = stereo ? BusLayout::Stereo : BusLayout::Mono;
    }
    else
    {
        trace("[vst3trace] requested arrangement rejected; re-querying retained layout after the call");
        std::vector<Steinberg::Vst::SpeakerArrangement> inputAfter;
        std::vector<Steinberg::Vst::SpeakerArrangement> outputAfter;
        discoverBuses(Steinberg::Vst::kInput, inputBuses, "input-after", inputAfter);
        discoverBuses(Steinberg::Vst::kOutput, outputBuses, "output-after", outputAfter);
        // Evidence source = the plug-in's NATIVE (pre-call) retained arrangement:
        // the strongest proof of what it supports. Pro-Q 4 mutates its own buses
        // to the requested layout even when setBusArrangements returns false, so
        // the after-state is logged for the record but is not reliable evidence.
        if (stereo)
        {
            trace(std::string("[vst3trace] retained main mono (native evidence)=") + (retainedMainHolds(true, Steinberg::Vst::SpeakerArr::kMono) ? "in" : "-") +
                  (retainedMainHolds(false, Steinberg::Vst::SpeakerArr::kMono) ? "/out" : "/-"));
        }
        else
        {
            trace(std::string("[vst3trace] retained main stereo (native evidence)=") + (retainedMainHolds(true, Steinberg::Vst::SpeakerArr::kStereo) ? "in" : "-") +
                  (retainedMainHolds(false, Steinberg::Vst::SpeakerArr::kStereo) ? "/out" : "/-"));
        }
        const bool evidenceOpposite = stereo
            ? (retainedMainHolds(true, Steinberg::Vst::SpeakerArr::kMono) && retainedMainHolds(false, Steinberg::Vst::SpeakerArr::kMono))
            : (retainedMainHolds(true, Steinberg::Vst::SpeakerArr::kStereo) && retainedMainHolds(false, Steinberg::Vst::SpeakerArr::kStereo));
        if (evidenceOpposite)
        {
            stereo = !stereo;
            if (tryArrangements(stereo))
            {
                m_layout = stereo ? BusLayout::Stereo : BusLayout::Mono;
            }
            else
            {
                m_lastPrepareError = "setBusArrangements failed";
                return PrepareResult::Failed;
            }
        }
        else
        {
            m_lastPrepareError = "setBusArrangements failed: no retained-layout evidence to reverse";
            return PrepareResult::Failed;
        }
    }

    // Activate only the first audio input/output (the main bus). Any aux /
    // sidechain input stays inactive: the host does not use it in v1.
    for (Steinberg::int32 i = 0; i < inputBuses; ++i)
    {
        m_componentIf->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i, i == 0 ? Steinberg::TBool(1) : Steinberg::TBool(0));
    }
    for (Steinberg::int32 i = 0; i < outputBuses; ++i)
    {
        m_componentIf->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, i == 0 ? Steinberg::TBool(1) : Steinberg::TBool(0));
    }

    if (m_componentIf->setActive(Steinberg::TBool(1)) != Steinberg::kResultOk)
    {
        m_lastPrepareError = "setActive failed";
        return PrepareResult::Failed;
    }

    m_setup.processMode = Steinberg::Vst::kRealtime;
    m_setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    m_setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(maxBlockSamples);
    m_setup.sampleRate = sampleRate;

    if (m_processorIf->setupProcessing(m_setup) != Steinberg::kResultOk)
    {
        m_lastPrepareError = "setupProcessing failed";
        return PrepareResult::Failed;
    }
    if (m_processorIf->setProcessing(Steinberg::TBool(1)) != Steinberg::kResultOk)
    {
        m_lastPrepareError = "setProcessing failed";
        return PrepareResult::Failed;
    }

    m_latency = m_processorIf->getLatencySamples();
    m_maxBlock = static_cast<std::uint32_t>(maxBlockSamples);
    if (stereo)
    {
        m_stagingLeft.assign(m_maxBlock, 0.0f);
        m_stagingRight.assign(m_maxBlock, 0.0f);
        m_inChannels[0] = m_stagingLeft.data();
        m_inChannels[1] = m_stagingRight.data();
    }
    else
    {
        m_staging.assign(m_maxBlock, 0.0f);
        m_inChannel = m_staging.data();
    }
    m_valid = true;
    return PrepareResult::Ok;
    }
    catch (const std::exception& exception)
    {
        trace(std::string("[vst3trace] plugin threw during prepare: ") + exception.what());
        m_lastPrepareError = std::string("plugin exception during prepare: ") + exception.what();
        return PrepareResult::Failed;
    }
    catch (...)
    {
        trace("[vst3trace] plugin threw an unknown exception during prepare");
        m_lastPrepareError = "plugin exception during prepare (unknown)";
        return PrepareResult::Failed;
    }
}

const std::string& Vst3Processor::lastPrepareError() const
{
    return m_lastPrepareError;
}

bool Vst3Processor::enqueueParameter(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue normalizedValue)
{
    const bool accepted = m_pendingEdits.push(id, normalizedValue);
    if (!accepted)
    {
        m_parameterDrops.fetch_add(1, std::memory_order_relaxed);
    }
    return accepted;
}

void Vst3Processor::onParameterEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized)
{
    (void)enqueueParameter(id, valueNormalized);
}

std::uint64_t Vst3Processor::parameterDrops() const
{
    return m_parameterDrops.load(std::memory_order_relaxed);
}

std::uint64_t Vst3Processor::parameterDelivered() const
{
    return m_parameterDelivered.load(std::memory_order_relaxed);
}

void Vst3Processor::deliverParameterChanges()
{
    m_streamChanges.reset();
    const std::size_t drained = m_pendingEdits.drain(m_drainBuffer.data(), m_drainBuffer.size());
    for (std::size_t i = 0; i < drained; ++i)
    {
        const ParameterEditQueue::Edit& edit = m_drainBuffer[i];
        // Sample offset 0: the change applies at the start of this block. The
        // plug-in reads the points in order and typically uses the last one.
        if (!m_streamChanges.addPoint(edit.id, 0, edit.value))
        {
            // Per-block exposure exceeded (too many distinct params or points).
            // Defined overflow: the remaining drained edits are dropped and
            // counted. The queue itself never grows.
            m_parameterDrops.fetch_add(static_cast<std::uint64_t>(drained - i), std::memory_order_relaxed);
            break;
        }
        m_parameterDelivered.fetch_add(1, std::memory_order_relaxed);
    }
}

void Vst3Processor::teardown()
{
    if (m_hostContext != nullptr)
    {
        m_hostContext->release();
        m_hostContext = nullptr;
    }
    if (m_processorIf != nullptr)
    {
        m_processorIf->setProcessing(Steinberg::TBool(0));
    }
    if (m_componentIf != nullptr)
    {
        m_componentIf->setActive(Steinberg::TBool(0));
        m_componentIf->terminate();
    }
    if (m_component != nullptr)
    {
        m_component->release();
        m_component = nullptr;
    }
    m_componentIf = nullptr;
    m_processorIf = nullptr;
    m_valid = false;
}

bool Vst3Processor::saveState(std::vector<std::uint8_t>& outBlob)
{
    outBlob.clear();
    if (m_componentIf == nullptr || !m_valid)
    {
        m_lastPrepareError = "processor not prepared";
        return false;
    }

    Vst3StateStream stream;
    const Steinberg::tresult result = m_componentIf->getState(&stream);
    if (result != Steinberg::kResultOk || stream.overflow())
    {
        m_lastPrepareError = "getState failed or wrote more than kMaxStateBytes";
        return false;
    }

    const std::vector<std::uint8_t>& payload = stream.view();
    outBlob = Vst3StateBlob::encode(payload);
    if (outBlob.empty())
    {
        m_lastPrepareError = "state blob encode failed (payload exceeds kMaxStateBytes)";
        return false;
    }
    m_lastPrepareError.clear();
    return true;
}

bool Vst3Processor::restoreState(const std::vector<std::uint8_t>& blob)
{
    if (m_componentIf == nullptr || !m_valid)
    {
        m_lastPrepareError = "processor not prepared";
        return false;
    }

    std::vector<std::uint8_t> payload;
    if (!Vst3StateBlob::decode(blob, payload))
    {
        m_lastPrepareError = "state blob rejected (magic/version/length/checksum)";
        return false;
    }

    Vst3StateStream stream;
    if (!payload.empty())
    {
        const Steinberg::int32 numBytes = static_cast<Steinberg::int32>(payload.size());
        if (stream.write(payload.data(), numBytes, nullptr) != Steinberg::kResultTrue)
        {
            m_lastPrepareError = "state payload cannot be staged for setState";
            return false;
        }
    }
    if (stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr) != Steinberg::kResultTrue)
    {
        m_lastPrepareError = "state payload cannot be rewound for setState";
        return false;
    }

    const Steinberg::tresult result = m_componentIf->setState(&stream);
    if (result != Steinberg::kResultOk)
    {
        m_lastPrepareError = "setState rejected the restored state";
        return false;
    }
    m_lastPrepareError.clear();
    return true;
}

void Vst3Processor::chainProcess(const float* input, float* output, std::size_t frames, void* context)
{
    auto* self = static_cast<Vst3Processor*>(context);
    if (self == nullptr || self->m_valid == false || self->m_layout != BusLayout::Mono || input == nullptr || output == nullptr || frames == 0)
    {
        if (input != nullptr && output != nullptr && frames > 0)
        {
            std::memcpy(output, input, frames * sizeof(float));
        }
        return;
    }
    if (frames > self->m_maxBlock)
    {
        std::memcpy(output, input, frames * sizeof(float));
        return;
    }

    std::memcpy(self->m_staging.data(), input, frames * sizeof(float));
    self->m_inChannel = self->m_staging.data();
    self->m_outChannel = output; // float* == Sample32*

    self->m_inputBuffer.numChannels = 1;
    self->m_inputBuffer.silenceFlags = 0;
    self->m_inputBuffer.channelBuffers32 = &self->m_inChannel;

    self->m_outputBuffer.numChannels = 1;
    self->m_outputBuffer.silenceFlags = 0;
    self->m_outputBuffer.channelBuffers32 = &self->m_outChannel;

    self->m_processData.processMode = Steinberg::Vst::kRealtime;
    self->m_processData.numSamples = static_cast<Steinberg::int32>(frames);
    self->m_processData.numInputs = 1;
    self->m_processData.numOutputs = 1;
    self->m_processData.inputs = &self->m_inputBuffer;
    self->m_processData.outputs = &self->m_outputBuffer;

    // VST-009: drain pending UI/automation edits into this block's parameter
    // changes and hand them to the plug-in before processing. Bounded, no
    // allocation, no locking.
    self->deliverParameterChanges();
    self->m_processData.inputParameterChanges = &self->m_streamChanges;

    (void)self->m_processorIf->process(self->m_processData);
    self->m_processData.inputParameterChanges = nullptr;
}

void Vst3Processor::chainProcessStereo(const float* leftIn, const float* rightIn, float* leftOut, float* rightOut,
                                       std::size_t frames, void* context)
{
    auto* self = static_cast<Vst3Processor*>(context);
    const bool passthrough = self == nullptr || self->m_valid == false || self->m_layout != BusLayout::Stereo ||
                             leftIn == nullptr || rightIn == nullptr || leftOut == nullptr || rightOut == nullptr ||
                             frames == 0 || frames > self->m_maxBlock;
    if (passthrough)
    {
        if (leftIn != nullptr && leftOut != nullptr && frames > 0)
        {
            std::memcpy(leftOut, leftIn, frames * sizeof(float));
        }
        if (rightIn != nullptr && rightOut != nullptr && frames > 0)
        {
            std::memcpy(rightOut, rightIn, frames * sizeof(float));
        }
        return;
    }

    std::memcpy(self->m_stagingLeft.data(), leftIn, frames * sizeof(float));
    std::memcpy(self->m_stagingRight.data(), rightIn, frames * sizeof(float));

    self->m_inputBuffer.numChannels = 2;
    self->m_inputBuffer.silenceFlags = 0;
    self->m_inputBuffer.channelBuffers32 = self->m_inChannels;

    self->m_outputBuffer.numChannels = 2;
    self->m_outputBuffer.silenceFlags = 0;
    self->m_outputBuffer.channelBuffers32 = self->m_outChannels;

    self->m_outChannels[0] = leftOut;
    self->m_outChannels[1] = rightOut;

    self->m_processData.processMode = Steinberg::Vst::kRealtime;
    self->m_processData.numSamples = static_cast<Steinberg::int32>(frames);
    self->m_processData.numInputs = 1;
    self->m_processData.numOutputs = 1;
    self->m_processData.inputs = &self->m_inputBuffer;
    self->m_processData.outputs = &self->m_outputBuffer;

    self->deliverParameterChanges();
    self->m_processData.inputParameterChanges = &self->m_streamChanges;

    (void)self->m_processorIf->process(self->m_processData);
    self->m_processData.inputParameterChanges = nullptr;
}

} // namespace audient::vst3
