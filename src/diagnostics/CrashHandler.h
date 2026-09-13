#pragma once

#include <string>

namespace audient::diagnostics
{

class CrashHandler
{
public:
    static void install();
    static void uninstall();
    static void setDumpDirectory(std::wstring path);
    static const std::wstring& dumpDirectory();
};

} // namespace audient::diagnostics