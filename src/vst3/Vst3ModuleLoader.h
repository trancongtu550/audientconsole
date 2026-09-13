#pragma once

#include <string>

#include "pluginterfaces/base/ipluginbase.h"

namespace audient::vst3
{

// Loads a native .vst3 module from an explicit path and exposes its class factory.
//
// Path resolution (Phase 4 slice: bundle resolution):
//  - A direct module file may be given, e.g. ".../CachedPlugin.vst3".
//  - A Windows .vst3 bundle directory may be given, e.g. ".../Plugin.vst3".
//    The architecture module is auto-derived from the bundle layout:
//    ".../Plugin.vst3/Contents/x86_64-win/<module>.vst3". The module file name is
//    NOT hard-coded to match the bundle name; the arch directory is enumerated and
//    a single *.vst3 module is accepted. Zero modules is an error; more than one
//    is an ambiguity error (never guess).
//
// Host contract / lifetime (project guidelines §9, §20):
//  - The module is resolved explicitly; nothing is scanned implicitly by this class.
//  - The returned factory is owned by the module. Vst3Host::attachFactory addRefs it;
//    callers must detach every processor from the host and destroy all processors
//    BEFORE Vst3ModuleLoader goes out of scope (module unload must never race a
//    live component). `unload()` therefore refuses while a factory is attached.
//  - Loading/unloading/scanning happens off the audio thread only.
class Vst3ModuleLoader
{
public:
    Vst3ModuleLoader() = default;
    ~Vst3ModuleLoader();

    Vst3ModuleLoader(const Vst3ModuleLoader&) = delete;
    Vst3ModuleLoader& operator=(const Vst3ModuleLoader&) = delete;

    // Accepts either a direct module file (".../Plugin.vst3") or a Windows bundle
    // directory (".../Plugin.vst3/"). Resolves to the architecture module and
    // loads it. Returns false with a clear lastError() on any resolution failure.
    bool load(const std::string& modulePath);

    // Static, pure path resolution (no module is loaded). Given either a direct
    // module file or a Windows .vst3 bundle directory, resolves to the actual
    // architecture module file. On failure returns false and sets a reason.
    static bool resolveModulePath(const std::string& modulePath, std::string& resolvedPath, std::string& error);
    void unload();
    bool loaded() const;

    // Borrowed pointer; valid while loaded(). Do not call Release() on it.
    Steinberg::IPluginFactory* factory() const;

    const std::string& modulePath() const;
    const std::string& lastError() const;

private:
    void* m_module = nullptr;              // HMODULE
    Steinberg::IPluginFactory* m_factory = nullptr;
    std::string m_modulePath;
    std::string m_lastError;
};

} // namespace audient::vst3