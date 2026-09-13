// Test-only DLL that is a real loadable PE module but does NOT export the VST3
// GetPluginFactory entry point. Used by the loader tests to prove the
// module-loading boundary vs the factory-lookup boundary: LoadLibrary succeeds,
// GetProcAddress("GetPluginFactory") must fail with a clear error. Never shipped.

extern "C" __declspec(dllexport) int TestNonFactoryModuleDummyExport()
{
    return 42;
}