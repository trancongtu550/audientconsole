#include "TestPassthroughPlugin.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <cstring>

namespace audient::vst3_test
{

namespace
{
bool sameId(const Steinberg::TUID a, const Steinberg::TUID b)
{
    return std::memcmp(a, b, sizeof(Steinberg::TUID)) == 0;
}

void copyText(Steinberg::char8* destination, std::size_t capacity, const char* source)
{
    if (capacity == 0)
    {
        return;
    }
    const std::size_t sourceLength = std::strlen(source);
    const std::size_t toCopy = sourceLength < capacity - 1 ? sourceLength : capacity - 1;
    std::memcpy(destination, source, toCopy);
    destination[toCopy] = 0;
}
} // namespace

TestPassthroughProcessor::TestPassthroughProcessor(Steinberg::Vst::IComponent* owner, float gain)
    : m_owner(owner)
    , m_gain(gain)
{
}

Steinberg::tresult TestPassthroughProcessor::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (sameId(requestId, Steinberg::Vst::IAudioProcessor::iid.toTUID()))
    {
        *obj = this;
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 TestPassthroughProcessor::addRef()
{
    return m_owner != nullptr ? m_owner->addRef() : 1;
}

Steinberg::uint32 TestPassthroughProcessor::release()
{
    return m_owner != nullptr ? m_owner->release() : 1;
}

Steinberg::tresult TestPassthroughProcessor::setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                                                Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts)
{
    if (m_stereoOnly)
    {
        const bool inputStereo = numIns > 0 && (inputs[0] & Steinberg::Vst::SpeakerArr::kStereo) == Steinberg::Vst::SpeakerArr::kStereo;
        const bool outputStereo = numOuts > 0 && (outputs[0] & Steinberg::Vst::SpeakerArr::kStereo) == Steinberg::Vst::SpeakerArr::kStereo;
        if (!inputStereo || !outputStereo)
        {
            return Steinberg::kResultFalse; // stereo-only: reject any mono request
        }
    }
    m_stereo = (numIns > 0 &&
                (inputs[0] & Steinberg::Vst::SpeakerArr::kStereo) == Steinberg::Vst::SpeakerArr::kStereo) ||
               (numOuts > 0 &&
                (outputs[0] & Steinberg::Vst::SpeakerArr::kStereo) == Steinberg::Vst::SpeakerArr::kStereo);
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughProcessor::getBusArrangement(Steinberg::Vst::BusDirection dir, Steinberg::int32 index,
                                                               Steinberg::Vst::SpeakerArrangement& arrangement)
{
    (void)dir;
    if (index != 0)
    {
        return Steinberg::kResultFalse;
    }
    // A stereo-only plug-in always reports its retained arrangement as stereo,
    // even before it has accepted a stereo setBusArrangements (the evidence the
    // host uses to reverse a rejected mono request — VST-018).
    arrangement = (m_stereoOnly || m_stereo) ? Steinberg::Vst::SpeakerArr::kStereo : Steinberg::Vst::SpeakerArr::kMono;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughProcessor::canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
{
    return symbolicSampleSize == Steinberg::Vst::kSample32 ? Steinberg::kResultTrue : Steinberg::kResultFalse;
}

Steinberg::uint32 TestPassthroughProcessor::getLatencySamples()
{
    return m_latency;
}

Steinberg::uint32 TestPassthroughProcessor::getTailSamples()
{
    return 0;
}

Steinberg::tresult TestPassthroughProcessor::setupProcessing(Steinberg::Vst::ProcessSetup& setup)
{
    (void)setup;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughProcessor::setProcessing(Steinberg::TBool state)
{
    m_processing = state != 0;
    return Steinberg::kResultOk;
}

bool TestPassthroughProcessor::stereoArrangement() const
{
    return m_stereo;
}

int TestPassthroughProcessor::receivedParameterCount() const
{
    return m_receivedCount;
}

const ReceivedParameter& TestPassthroughProcessor::receivedParameterAt(int index) const
{
    static const ReceivedParameter kEmpty{};
    if (index < 0 || index >= m_receivedCount || index >= kMaxReceived)
    {
        return kEmpty;
    }
    return m_received[index];
}

void TestPassthroughProcessor::clearReceivedParameters()
{
    m_receivedCount = 0;
    for (ReceivedParameter& parameter : m_received)
    {
        parameter = ReceivedParameter{};
    }
}

void TestPassthroughProcessor::recordReceivedParameters(Steinberg::Vst::ProcessData& data)
{
    if (data.inputParameterChanges == nullptr)
    {
        return;
    }
    const Steinberg::int32 queues = data.inputParameterChanges->getParameterCount();
    for (Steinberg::int32 q = 0; q < queues; ++q)
    {
        Steinberg::Vst::IParamValueQueue* queue = data.inputParameterChanges->getParameterData(q);
        if (queue == nullptr)
        {
            continue;
        }
        const Steinberg::Vst::ParamID id = queue->getParameterId();
        const Steinberg::int32 pointCount = queue->getPointCount();
        Steinberg::Vst::ParamValue lastValue = 0.0;
        for (Steinberg::int32 p = 0; p < pointCount; ++p)
        {
            Steinberg::int32 sampleOffset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queue->getPoint(p, sampleOffset, value) == Steinberg::kResultOk)
            {
                lastValue = value;
            }
        }
        int found = -1;
        for (int i = 0; i < m_receivedCount; ++i)
        {
            if (m_received[i].id == id)
            {
                found = i;
                break;
            }
        }
        if (found < 0)
        {
            if (m_receivedCount >= kMaxReceived)
            {
                continue;
            }
            found = m_receivedCount++;
        }
        m_received[found].id = id;
        m_received[found].value = lastValue;
        m_received[found].points = static_cast<std::uint32_t>(pointCount);
    }
}

Steinberg::tresult TestPassthroughProcessor::process(Steinberg::Vst::ProcessData& data)
{
    if (!m_processing)
    {
        return Steinberg::kResultOk;
    }
    recordReceivedParameters(data);
    const Steinberg::int32 buses = data.numInputs < data.numOutputs ? data.numInputs : data.numOutputs;
    for (Steinberg::int32 bus = 0; bus < buses; ++bus)
    {
        Steinberg::Vst::AudioBusBuffers& inBus = data.inputs[bus];
        Steinberg::Vst::AudioBusBuffers& outBus = data.outputs[bus];
        const Steinberg::int32 channels = inBus.numChannels < outBus.numChannels ? inBus.numChannels : outBus.numChannels;
        for (Steinberg::int32 ch = 0; ch < channels; ++ch)
        {
            const float* in = inBus.channelBuffers32 != nullptr ? inBus.channelBuffers32[ch] : nullptr;
            float* out = outBus.channelBuffers32 != nullptr ? outBus.channelBuffers32[ch] : nullptr;
            if (in == nullptr || out == nullptr)
            {
                continue;
            }
            for (Steinberg::int32 i = 0; i < data.numSamples; ++i)
            {
                out[i] = in[i] * m_gain;
            }
        }
    }
    return Steinberg::kResultOk;
}

TestPassthroughComponent::TestPassthroughComponent(float gain, bool stereoOnly)
    : m_processor(this, gain)
    , m_gain(gain)
{
    m_processor.setStereoOnly(stereoOnly);
}

Steinberg::tresult TestPassthroughComponent::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (sameId(requestId, Steinberg::Vst::IComponent::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IComponent*>(this);
        return Steinberg::kResultOk;
    }
    if (sameId(requestId, Steinberg::Vst::IAudioProcessor::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IAudioProcessor*>(&m_processor);
        return Steinberg::kResultOk;
    }
    if (m_singleObjectController && sameId(requestId, Steinberg::Vst::IEditController::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IEditController*>(this);
        return Steinberg::kResultOk;
    }
    if (sameId(requestId, Steinberg::Vst::IConnectionPoint::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IConnectionPoint*>(this);
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 TestPassthroughComponent::addRef()
{
    return ++m_references;
}

Steinberg::uint32 TestPassthroughComponent::release()
{
    if (--m_references == 0)
    {
        delete this;
        return 0;
    }
    return m_references;
}

Steinberg::tresult TestPassthroughComponent::initialize(Steinberg::FUnknown* context)
{
    (void)context;
    m_initialized = true;
    ++m_initializeCount;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::terminate()
{
    m_initialized = false;
    ++m_terminateCount;
    return Steinberg::kResultOk;
}

void TestPassthroughComponent::setControllerClassId(const Steinberg::TUID& classId)
{
    std::memcpy(m_controllerClassId, classId, sizeof(Steinberg::TUID));
}

Steinberg::tresult TestPassthroughComponent::getControllerClassId(Steinberg::TUID classId)
{
    bool empty = true;
    for (const unsigned char byte : m_controllerClassId)
    {
        if (byte != 0)
        {
            empty = false;
            break;
        }
    }
    if (empty)
    {
        std::memset(classId, 0, sizeof(Steinberg::TUID));
        return Steinberg::kResultOk;
    }
    std::memcpy(classId, m_controllerClassId, sizeof(Steinberg::TUID));
    return Steinberg::kResultTrue;
}

Steinberg::tresult TestPassthroughComponent::setIoMode(Steinberg::Vst::IoMode mode)
{
    (void)mode;
    return Steinberg::kResultOk;
}

Steinberg::int32 TestPassthroughComponent::getBusCount(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir)
{
    (void)dir;
    return type == Steinberg::Vst::kAudio ? 1 : 0;
}

Steinberg::tresult TestPassthroughComponent::getBusInfo(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir,
                                                        Steinberg::int32 index, Steinberg::Vst::BusInfo& bus)
{
    (void)index;
    if (type != Steinberg::Vst::kAudio)
    {
        return Steinberg::kInvalidArgument;
    }
    bus.mediaType = type;
    bus.direction = dir;
    bus.channelCount = m_processor.stereoArrangement() ? 2 : 1;
    bus.busType = Steinberg::Vst::kMain;
    bus.flags = 0;
    std::memset(bus.name, 0, sizeof(bus.name));
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::getRoutingInfo(Steinberg::Vst::RoutingInfo& inInfo, Steinberg::Vst::RoutingInfo& outInfo)
{
    (void)inInfo;
    (void)outInfo;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::activateBus(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir,
                                                         Steinberg::int32 index, Steinberg::TBool state)
{
    (void)type;
    (void)dir;
    (void)index;
    (void)state;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::setActive(Steinberg::TBool state)
{
    m_active = state != 0;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::setState(Steinberg::IBStream* state)
{
    if (state == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    // Layout: magic 'TPSS' (u32 LE), version (u16 LE), stateValue (float LE),
    // stateSeq (u32 LE), gain (float LE).
    struct StatePayload
    {
        Steinberg::int32 magic = 0;
        Steinberg::int16 version = 0;
        float stateValue = 0.0f;
        Steinberg::uint32 stateSeq = 0;
        float gain = 0.0f;
    };
    StatePayload payload{};
    Steinberg::int32 read = 0;
    if (state->read(&payload, static_cast<Steinberg::int32>(sizeof(payload)), &read) != Steinberg::kResultTrue ||
        read != static_cast<Steinberg::int32>(sizeof(payload)))
    {
        return Steinberg::kResultFalse;
    }
    if (payload.magic != 0x53535054 || payload.version != 1)
    {
        return Steinberg::kResultFalse;
    }
    m_stateValue = payload.stateValue;
    m_stateSeq = payload.stateSeq;
    m_gain = payload.gain;
    m_processor.setStateGain(payload.gain);
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::getState(Steinberg::IBStream* state)
{
    if (state == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    struct StatePayload
    {
        Steinberg::int32 magic = 0;
        Steinberg::int16 version = 0;
        float stateValue = 0.0f;
        Steinberg::uint32 stateSeq = 0;
        float gain = 0.0f;
    };
    static_assert(sizeof(StatePayload) == 20, "TPSS payload size is part of the state blob contract");
    StatePayload payload{};
    std::memset(&payload, 0, sizeof(payload)); // zero alignment padding so the blob is deterministic
    payload.magic = 0x53535054;
    payload.version = 1;
    payload.stateValue = m_stateValue;
    payload.stateSeq = m_stateSeq;
    payload.gain = m_gain;
    Steinberg::int32 written = 0;
    const Steinberg::tresult result =
        state->write(&payload, static_cast<Steinberg::int32>(sizeof(payload)), &written);
    if (result != Steinberg::kResultTrue || written != static_cast<Steinberg::int32>(sizeof(payload)))
    {
        return Steinberg::kResultFalse;
    }
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::setComponentState(Steinberg::IBStream* state)
{
    (void)state;
    return Steinberg::kResultOk;
}

Steinberg::int32 TestPassthroughComponent::getParameterCount()
{
    return 0;
}

Steinberg::tresult TestPassthroughComponent::getParameterInfo(Steinberg::int32 paramIndex, Steinberg::Vst::ParameterInfo& info)
{
    (void)paramIndex;
    (void)info;
    return Steinberg::kResultFalse;
}

Steinberg::tresult TestPassthroughComponent::getParamStringByValue(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized,
                                                                   Steinberg::Vst::String128 string)
{
    (void)id;
    (void)valueNormalized;
    (void)string;
    return Steinberg::kResultFalse;
}

Steinberg::tresult TestPassthroughComponent::getParamValueByString(Steinberg::Vst::ParamID id, Steinberg::Vst::TChar* string,
                                                                   Steinberg::Vst::ParamValue& valueNormalized)
{
    (void)id;
    (void)string;
    (void)valueNormalized;
    return Steinberg::kResultFalse;
}

Steinberg::Vst::ParamValue TestPassthroughComponent::normalizedParamToPlain(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized)
{
    (void)id;
    return valueNormalized;
}

Steinberg::Vst::ParamValue TestPassthroughComponent::plainParamToNormalized(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue plainValue)
{
    (void)id;
    return plainValue;
}

Steinberg::Vst::ParamValue TestPassthroughComponent::getParamNormalized(Steinberg::Vst::ParamID id)
{
    (void)id;
    return 0.0;
}

Steinberg::tresult TestPassthroughComponent::setParamNormalized(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
{
    (void)id;
    (void)value;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::setComponentHandler(Steinberg::Vst::IComponentHandler* handler)
{
    (void)handler;
    return Steinberg::kResultOk;
}

Steinberg::IPlugView* TestPassthroughComponent::createView(Steinberg::FIDString name)
{
    (void)name;
    return nullptr;
}

Steinberg::tresult TestPassthroughComponent::connect(Steinberg::Vst::IConnectionPoint* other)
{
    m_connectionPeer = other;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::disconnect(Steinberg::Vst::IConnectionPoint* other)
{
    if (m_connectionPeer == other)
    {
        m_connectionPeer = nullptr;
    }
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughComponent::notify(Steinberg::Vst::IMessage* message)
{
    (void)message;
    return Steinberg::kResultOk;
}

TestPassthroughFactory::TestPassthroughFactory(float gain, bool singleObjectController, bool stereoOnly)
    : m_gain(gain)
    , m_singleObjectController(singleObjectController)
    , m_stereoOnly(stereoOnly)
{
}

Steinberg::tresult TestPassthroughFactory::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (sameId(requestId, Steinberg::IPluginFactory::iid.toTUID()))
    {
        *obj = this;
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 TestPassthroughFactory::addRef()
{
    return 1;
}

Steinberg::uint32 TestPassthroughFactory::release()
{
    return 1;
}

Steinberg::tresult TestPassthroughFactory::getFactoryInfo(Steinberg::PFactoryInfo* info)
{
    std::memset(info, 0, sizeof(Steinberg::PFactoryInfo));
    copyText(info->vendor, sizeof(info->vendor), "Audient Console Tests");
    info->flags = Steinberg::PFactoryInfo::kNoFlags;
    return Steinberg::kResultOk;
}

Steinberg::int32 TestPassthroughFactory::countClasses()
{
    return 1;
}

Steinberg::tresult TestPassthroughFactory::getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info)
{
    if (index != 0)
    {
        return Steinberg::kResultFalse;
    }
    std::memset(info, 0, sizeof(Steinberg::PClassInfo));
    const Steinberg::TUID& tuid = m_cid.toTUID();
    std::memcpy(info->cid, tuid, sizeof(Steinberg::TUID));
    info->cardinality = Steinberg::PClassInfo::kManyInstances;
    copyText(info->category, sizeof(info->category), "Audio Effect");
    copyText(info->name, sizeof(info->name), "Test Passthrough");
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughFactory::createInstance(Steinberg::FIDString cid, Steinberg::FIDString requestId, void** obj)
{
    (void)requestId;
    // VST3 ABI: cid arrives as the RAW 16-byte TUID (see doc.h usage
    // createInstance(ci.cid, FUnknown::iid, ...)), not the hex string form.
    const Steinberg::TUID& tuid = m_cid.toTUID();
    if (std::memcmp(cid, tuid, sizeof(Steinberg::TUID)) != 0)
    {
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    *obj = static_cast<void*>(new TestPassthroughComponent(m_gain, m_stereoOnly));
    if (m_singleObjectController)
    {
        static_cast<TestPassthroughComponent*>(*obj)->enableSingleObjectController();
    }
    return Steinberg::kResultOk;
}

const Steinberg::FUID& TestPassthroughFactory::classId() const
{
    return m_cid;
}

TestPassthroughController::TestPassthroughController()
{
}

Steinberg::tresult TestPassthroughController::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (sameId(requestId, Steinberg::Vst::IEditController::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IEditController*>(this);
        return Steinberg::kResultOk;
    }
    if (sameId(requestId, Steinberg::Vst::IConnectionPoint::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::Vst::IConnectionPoint*>(this);
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 TestPassthroughController::addRef()
{
    return ++m_references;
}

Steinberg::uint32 TestPassthroughController::release()
{
    if (--m_references == 0)
    {
        delete this;
        return 0;
    }
    return m_references;
}

Steinberg::tresult TestPassthroughController::initialize(Steinberg::FUnknown* context)
{
    (void)context;
    m_initialized = true;
    ++m_initializeCount;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughController::terminate()
{
    m_initialized = false;
    ++m_terminateCount;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughController::getState(Steinberg::IBStream* state)
{
    if (state == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    struct ControllerPayload
    {
        Steinberg::int32 magic = 0;
        Steinberg::int16 version = 0;
        float stateValue = 0.0f;
        Steinberg::uint32 stateSeq = 0;
    };
    ControllerPayload payload{};
    static_assert(sizeof(ControllerPayload) == 16, "TPCT payload size is part of the controller blob contract");
    std::memset(&payload, 0, sizeof(payload)); // zero alignment padding so the blob is deterministic
    payload.magic = 0x54435054; // 'TPCT'
    payload.version = 1;
    payload.stateValue = m_stateValue;
    payload.stateSeq = m_stateSeq;
    Steinberg::int32 written = 0;
    const Steinberg::tresult result =
        state->write(&payload, static_cast<Steinberg::int32>(sizeof(payload)), &written);
    if (result != Steinberg::kResultTrue || written != static_cast<Steinberg::int32>(sizeof(payload)))
    {
        return Steinberg::kResultFalse;
    }
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughController::setState(Steinberg::IBStream* state)
{
    if (state == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    struct ControllerPayload
    {
        Steinberg::int32 magic = 0;
        Steinberg::int16 version = 0;
        float stateValue = 0.0f;
        Steinberg::uint32 stateSeq = 0;
    };
    ControllerPayload payload{};
    Steinberg::int32 read = 0;
    if (state->read(&payload, static_cast<Steinberg::int32>(sizeof(payload)), &read) != Steinberg::kResultTrue ||
        read != static_cast<Steinberg::int32>(sizeof(payload)))
    {
        return Steinberg::kResultFalse;
    }
    if (payload.magic != 0x54435054 || payload.version != 1)
    {
        return Steinberg::kResultFalse;
    }
    m_stateValue = payload.stateValue;
    m_stateSeq = payload.stateSeq;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughController::setComponentState(Steinberg::IBStream* state)
{
    (void)state;
    return Steinberg::kResultOk;
}

Steinberg::int32 TestPassthroughController::getParameterCount()
{
    return 0;
}

Steinberg::tresult TestPassthroughController::getParameterInfo(Steinberg::int32 paramIndex, Steinberg::Vst::ParameterInfo& info)
{
    (void)paramIndex;
    (void)info;
    return Steinberg::kResultFalse;
}

Steinberg::tresult TestPassthroughController::getParamStringByValue(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized,
                                                                    Steinberg::Vst::String128 string)
{
    (void)id;
    (void)valueNormalized;
    (void)string;
    return Steinberg::kResultFalse;
}

Steinberg::tresult TestPassthroughController::getParamValueByString(Steinberg::Vst::ParamID id, Steinberg::Vst::TChar* string,
                                                                    Steinberg::Vst::ParamValue& valueNormalized)
{
    (void)id;
    (void)string;
    (void)valueNormalized;
    return Steinberg::kResultFalse;
}

Steinberg::Vst::ParamValue TestPassthroughController::normalizedParamToPlain(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized)
{
    (void)id;
    return valueNormalized;
}

Steinberg::Vst::ParamValue TestPassthroughController::plainParamToNormalized(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue plainValue)
{
    (void)id;
    return plainValue;
}

Steinberg::Vst::ParamValue TestPassthroughController::getParamNormalized(Steinberg::Vst::ParamID id)
{
    (void)id;
    return 0.0;
}

Steinberg::tresult TestPassthroughController::setParamNormalized(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)
{
    (void)id;
    (void)value;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughController::setComponentHandler(Steinberg::Vst::IComponentHandler* handler)
{
    (void)handler;
    return Steinberg::kResultOk;
}

Steinberg::IPlugView* TestPassthroughController::createView(Steinberg::FIDString name)
{
    (void)name;
    return nullptr;
}

Steinberg::tresult TestPassthroughController::connect(Steinberg::Vst::IConnectionPoint* other)
{
    (void)other;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughController::disconnect(Steinberg::Vst::IConnectionPoint* other)
{
    (void)other;
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughController::notify(Steinberg::Vst::IMessage* message)
{
    (void)message;
    return Steinberg::kResultOk;
}

TestPassthroughSeparateControllerFactory::TestPassthroughSeparateControllerFactory()
{
}

Steinberg::tresult TestPassthroughSeparateControllerFactory::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (sameId(requestId, Steinberg::IPluginFactory::iid.toTUID()))
    {
        *obj = this;
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 TestPassthroughSeparateControllerFactory::addRef()
{
    return 1;
}

Steinberg::uint32 TestPassthroughSeparateControllerFactory::release()
{
    return 1;
}

Steinberg::tresult TestPassthroughSeparateControllerFactory::getFactoryInfo(Steinberg::PFactoryInfo* info)
{
    std::memset(info, 0, sizeof(Steinberg::PFactoryInfo));
    copyText(info->vendor, sizeof(info->vendor), "Audient Console Tests");
    info->flags = Steinberg::PFactoryInfo::kNoFlags;
    return Steinberg::kResultOk;
}

Steinberg::int32 TestPassthroughSeparateControllerFactory::countClasses()
{
    return 2;
}

Steinberg::tresult TestPassthroughSeparateControllerFactory::getClassInfo(Steinberg::int32 index, Steinberg::PClassInfo* info)
{
    if (index < 0 || index > 1)
    {
        return Steinberg::kResultFalse;
    }
    std::memset(info, 0, sizeof(Steinberg::PClassInfo));
    const Steinberg::TUID& tuid = index == 0 ? m_componentCid.toTUID() : m_controllerCid.toTUID();
    std::memcpy(info->cid, tuid, sizeof(Steinberg::TUID));
    info->cardinality = Steinberg::PClassInfo::kManyInstances;
    copyText(info->category, sizeof(info->category), index == 0 ? "Audio Effect" : "Component Controller Class");
    copyText(info->name, sizeof(info->name), index == 0 ? "Test Passthrough Separate Controller" : "Test Passthrough Controller");
    return Steinberg::kResultOk;
}

Steinberg::tresult TestPassthroughSeparateControllerFactory::createInstance(Steinberg::FIDString cid, Steinberg::FIDString requestId, void** obj)
{
    (void)requestId;
    const Steinberg::TUID& componentTuid = m_componentCid.toTUID();
    if (std::memcmp(cid, componentTuid, sizeof(Steinberg::TUID)) == 0)
    {
        auto* component = new TestPassthroughComponent(1.0f);
        component->setControllerClassId(m_controllerCid.toTUID());
        *obj = static_cast<void*>(component);
        return Steinberg::kResultOk;
    }
    const Steinberg::TUID& controllerTuid = m_controllerCid.toTUID();
    if (std::memcmp(cid, controllerTuid, sizeof(Steinberg::TUID)) == 0)
    {
        *obj = static_cast<void*>(new TestPassthroughController());
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

const Steinberg::FUID& TestPassthroughSeparateControllerFactory::componentClassId() const
{
    return m_componentCid;
}

const Steinberg::FUID& TestPassthroughSeparateControllerFactory::controllerClassId() const
{
    return m_controllerCid;
}

} // namespace audient::vst3_test