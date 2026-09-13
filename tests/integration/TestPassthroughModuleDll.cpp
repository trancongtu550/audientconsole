#include "TestPassthroughPlugin.h"

// Test-only .vst3 module DLL. Built only for the test suite (not shipped): it
// exports the VST3 GetPluginFactory entry point the host loader expects, so the
// loader can be exercised end-to-end against a real PE module with a factory.
// The module is a shared library whose OUTPUT_NAME + SUFFIX produce
// "TestPassthrough.vst3", the same shape as a real VST3 architecture module.

namespace
{
// One process-wide factory instance. It does no real device/audio I/O and keeps
// an internal refcount that never drops to zero; the loader only borrows it.
audient::vst3_test::TestPassthroughFactory g_factory(0.75f);
} // namespace

extern "C" __declspec(dllexport) Steinberg::IPluginFactory* GetPluginFactory()
{
    return &g_factory;
}