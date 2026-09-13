#pragma once

#include "vst3/Vst3HostContext.h"
#include "vst3/Vst3Processor.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace Steinberg
{
class IPluginFactory;
}

namespace audient::vst3
{

struct PluginClassInfo
{
    std::string name;
    std::string category;
    std::string classId;          // hex string form, for display/logging
    Steinberg::TUID cid{};        // raw 16-byte class ID (VST3 ABI form)
    bool isEffect() const;
};

class Vst3Host
{
public:
    Vst3Host() = default;
    ~Vst3Host();
    Vst3Host(const Vst3Host&) = delete;
    Vst3Host& operator=(const Vst3Host&) = delete;

    bool attachFactory(Steinberg::IPluginFactory* factory);
    void detachFactory();
    bool hasFactory() const;

    // Comfort accessors (UI/scan threads only).
    std::vector<PluginClassInfo> classes() const;
    std::unique_ptr<Vst3Processor> createEffectProcessor(const PluginClassInfo& info, double sampleRate, long maxBlockSamples,
                                                         BusLayout layout = BusLayout::Mono);
    // Borrowed host context (IHostApplication/IComponentHandler) used for
    // processors; hands it to Vst3Editor so controller+component share context.
    Steinberg::FUnknown* hostContext() const;

    // VST-009: routes this host context's performEdit into `sink`'s bounded
    // queue. In this host one context can serve one active edit sink (the demo
    // sets the editor's target processor while its editor is open; cleared on
    // close). Null sink restores the pass-through no-op.
    void setParameterEditSink(IParameterEditSink* sink);

    const std::string& lastError() const;

    // VST-018: last prepare-time negotiation trace (bus discovery +
    // arrangement attempts + retained layout). Preserved even when
    // createEffectProcessor() fails and destroys the processor, so a failed
    // plug-in can be diagnosed deterministically.
    const std::string& prepareTrace() const { return m_prepareTrace; }

private:
    Steinberg::IPluginFactory* m_factory = nullptr;
    Vst3HostContext* m_hostContext = nullptr;
    std::string m_lastError;
    std::string m_prepareTrace;
};

} // namespace audient::vst3