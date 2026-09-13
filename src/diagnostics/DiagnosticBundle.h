#pragma once

#include <string>

namespace audient::diagnostics
{

struct DiagnosticBundle
{
    std::string appVersion;
    std::string gitSha;
    std::string buildType;
    std::string buildTime;
    std::string osVersion;
    bool allocationTrackerEnabled = false;
    std::string toJson() const;
};

DiagnosticBundle collectDiagnosticBundle();

} // namespace audient::diagnostics