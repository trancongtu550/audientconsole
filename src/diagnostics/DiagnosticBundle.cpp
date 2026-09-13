#include "diagnostics/DiagnosticBundle.h"

#include "core/AllocationTracker.h"
#include "core/Version.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace audient::diagnostics
{

namespace
{

std::string osVersionString()
{
#if defined(_WIN32)
    HMODULE ntdll = ::LoadLibraryW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return "unknown";
    }

    using RtlGetVersionFn = LONG(WINAPI*)(void*);
    RtlGetVersionFn func = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));

    struct OsVersionInfo
    {
        unsigned long size;
        unsigned long major;
        unsigned long minor;
        unsigned long build;
        unsigned long platformId;
        wchar_t servicePack[128];
    };

    std::string result = "unknown";
    if (func != nullptr)
    {
        OsVersionInfo info{};
        info.size = sizeof(info);
        if (func(&info) == 0)
        {
            char buffer[64]{};
            snprintf(buffer, sizeof(buffer), "Windows %lu.%lu.%lu", info.major, info.minor, info.build);
            result = buffer;
        }
    }
    ::FreeLibrary(ntdll);
    return result;
#else
    return "unknown";
#endif
}

} // namespace

std::string DiagnosticBundle::toJson() const
{
    std::string json;
    json += "{\n";
    json += "  \"appVersion\": \"" + appVersion + "\",\n";
    json += "  \"gitSha\": \"" + gitSha + "\",\n";
    json += "  \"buildType\": \"" + buildType + "\",\n";
    json += "  \"buildTime\": \"" + buildTime + "\",\n";
    json += "  \"osVersion\": \"" + osVersion + "\",\n";
    json += "  \"allocationTrackerEnabled\": " + std::string(allocationTrackerEnabled ? "true" : "false") + "\n";
    json += "}";
    return json;
}

DiagnosticBundle collectDiagnosticBundle()
{
    DiagnosticBundle bundle;
    bundle.appVersion = core::versionString();
    bundle.gitSha = core::gitSha();
    bundle.buildType = core::buildType();
    bundle.buildTime = core::buildTime();
    bundle.osVersion = osVersionString();
    bundle.allocationTrackerEnabled = core::isAllocationTrackerEnabled();
    return bundle;
}

} // namespace audient::diagnostics