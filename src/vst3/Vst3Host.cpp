#include "vst3/Vst3Host.h"

#include "vst3/Vst3Processor.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include <cctype>
#include <utility>

namespace audient::vst3
{

bool PluginClassInfo::isEffect() const
{
    // Accept component classes whose category indicates an audio processor and
    // reject controller-only and instrument classes. Real-world bundles deviate
    // from the exact VST3 "Audio Effect" convention (e.g. UADx uses
    // "Audio Module Class" for the processor and "Component Controller Class"
    // for the editor controller), so match on content, not exact equality.
    const auto lower = [](std::string value) {
        for (char& ch : value)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }(category);

    if (lower.find("controller") != std::string::npos)
    {
        return false; // component/editor controller class, not the processor
    }
    if (lower.find("instrument") != std::string::npos)
    {
        return false; // instrument plug-in (v1 out of scope)
    }

    const bool audioProcessor = lower.find("audio effect") != std::string::npos ||
                                lower.find("audio module") != std::string::npos ||
                                lower.find("fx") != std::string::npos ||
                                lower.rfind("audio", 0) == 0 ||
                                lower.find("effect") != std::string::npos;
    return audioProcessor;
}

Vst3Host::~Vst3Host()
{
    detachFactory();
    if (m_hostContext != nullptr)
    {
        m_hostContext->release();
        m_hostContext = nullptr;
    }
}

bool Vst3Host::attachFactory(Steinberg::IPluginFactory* factory)
{
    if (factory == nullptr)
    {
        m_lastError = "null factory";
        return false;
    }
    detachFactory();
    m_factory = factory;
    m_factory->addRef();
    m_lastError.clear();
    return true;
}

void Vst3Host::detachFactory()
{
    if (m_factory != nullptr)
    {
        m_factory->release();
        m_factory = nullptr;
    }
}

bool Vst3Host::hasFactory() const
{
    return m_factory != nullptr;
}

const std::string& Vst3Host::lastError() const
{
    return m_lastError;
}

Steinberg::FUnknown* Vst3Host::hostContext() const
{
    // Vst3HostContext derives FUnknown through IHostApplication and
    // IComponentHandler; disambiguate via the IHostApplication subobject (the
    // same pointer createEffectProcessor hands to processors).
    return static_cast<Steinberg::Vst::IHostApplication*>(m_hostContext);
}

void Vst3Host::setParameterEditSink(IParameterEditSink* sink)
{
    if (m_hostContext != nullptr)
    {
        m_hostContext->setParameterEditSink(sink);
    }
}

std::vector<PluginClassInfo> Vst3Host::classes() const
{
    std::vector<PluginClassInfo> result;
    if (m_factory == nullptr)
    {
        return result;
    }

    const Steinberg::int32 count = m_factory->countClasses();
    for (Steinberg::int32 i = 0; i < count; ++i)
    {
        Steinberg::PClassInfo info{};
        if (m_factory->getClassInfo(i, &info) != Steinberg::kResultOk)
        {
            continue;
        }

        Steinberg::FUID fuid = Steinberg::FUID::fromTUID(info.cid);
        Steinberg::char8 idString[33]{};
        fuid.toString(idString);

        PluginClassInfo cls;
        cls.name = info.name;
        cls.category = info.category;
        cls.classId = idString;
        std::memcpy(cls.cid, info.cid, sizeof(Steinberg::TUID));
        result.push_back(std::move(cls));
    }
    return result;
}

std::unique_ptr<Vst3Processor> Vst3Host::createEffectProcessor(const PluginClassInfo& info, double sampleRate, long maxBlockSamples,
                                                               BusLayout layout)
{
    if (m_factory == nullptr)
    {
        m_lastError = "no factory attached";
        return {};
    }
    if (!info.isEffect())
    {
        m_lastError = "class is not a supported effect category";
        return {};
    }

    if (m_hostContext == nullptr)
    {
        // Host context must outlive any processor. It is created eagerly per host
        // (not shared across hosts) and released in ~Vst3Host; processors addRef it.
        m_hostContext = new Vst3HostContext();
    }

    // VST3 createInstance contract: cid and interface id are RAW 16-byte TUIDs
    // (see pluginterfaces/base/doc.h and the SDK reference CPluginFactory which
    // matches class IDs with memcmp(..., sizeof(TUID))). Never pass the hex
    // string form; real plug-in factories compare raw bytes.
    void* unknownVoid = nullptr;
    const Steinberg::tresult instanceResult = m_factory->createInstance(info.cid, Steinberg::Vst::IComponent::iid.toTUID(),
                                                                       &unknownVoid);
    if (instanceResult != Steinberg::kResultOk || unknownVoid == nullptr)
    {
        m_lastError = "createInstance(" + info.name + ") failed hr=0x" + [](Steinberg::tresult result) {
            char buffer[16]{};
            std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(result));
            return std::string(buffer);
        }(instanceResult) + (unknownVoid == nullptr ? " null-object" : "");
        return {};
    }

    auto processor = std::unique_ptr<Vst3Processor>(new Vst3Processor(static_cast<Steinberg::FUnknown*>(unknownVoid)));
    if (processor->prepare(sampleRate, maxBlockSamples, layout, static_cast<Steinberg::Vst::IHostApplication*>(m_hostContext)) != Vst3Processor::PrepareResult::Ok)
    {
        // VST-018: keep the deterministic prepare trace even though the
        // processor is destroyed on failure (the trace names the exact step).
        m_prepareTrace = processor->prepareTrace();
        m_lastError = "processor prepare failed: " + processor->lastPrepareError();
        return {};
    }

    m_lastError.clear();
    return processor;
}

} // namespace audient::vst3