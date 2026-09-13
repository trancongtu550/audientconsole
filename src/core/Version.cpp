#include "audient_console_version.h"
#include "core/Version.h"

namespace audient::core {

const char* versionString()
{
    return AUDIENT_CONSOLE_VERSION;
}

int versionMajor()
{
    return AUDIENT_CONSOLE_VERSION_MAJOR;
}

int versionMinor()
{
    return AUDIENT_CONSOLE_VERSION_MINOR;
}

int versionPatch()
{
    return AUDIENT_CONSOLE_VERSION_PATCH;
}

const char* gitSha()
{
    return AUDIENT_CONSOLE_GIT_SHA;
}

const char* buildType()
{
    return AUDIENT_CONSOLE_BUILD_TYPE;
}

const char* buildTime()
{
    return AUDIENT_CONSOLE_BUILD_TIME;
}

std::string fullVersionString()
{
    std::string v = AUDIENT_CONSOLE_VERSION;
    v += " (";
    v += AUDIENT_CONSOLE_BUILD_TYPE;
    v += ", ";
    v += AUDIENT_CONSOLE_GIT_SHA;
    v += ", ";
    v += AUDIENT_CONSOLE_BUILD_TIME;
    v += ")";
    return v;
}

} // namespace audient::core