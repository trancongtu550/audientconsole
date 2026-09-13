#include "diagnostics/CrashHandler.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "ole32.lib")

namespace
{

constexpr std::size_t kMaxDumpFiles = 10;
std::wstring g_dumpDirectory;

std::wstring defaultDumpDirectory()
{
    wchar_t* buffer = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &buffer)))
    {
        std::wstring base(buffer);
        ::CoTaskMemFree(buffer);
        return base + L"\\AudientConsole\\crashdumps";
    }
    return L"";
}

void trimOldDumps(const std::wstring& directory)
{
    std::wstring pattern = directory + L"\\audient_console_*.dmp";
    WIN32_FIND_DATAW findData{};
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &findData);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    std::vector<std::pair<FILETIME, std::wstring>> files;
    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            files.emplace_back(findData.ftCreationTime, findData.cFileName);
        }
    } while (::FindNextFileW(handle, &findData) != 0);
    ::FindClose(handle);

    if (files.size() <= kMaxDumpFiles)
    {
        return;
    }

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first.dwHighDateTime != b.first.dwHighDateTime ? a.first.dwHighDateTime < b.first.dwHighDateTime : a.first.dwLowDateTime < b.first.dwLowDateTime; });

    const std::size_t excess = files.size() - kMaxDumpFiles;
    for (std::size_t i = 0; i < excess; ++i)
    {
        ::DeleteFileW((directory + L"\\" + files[i].second).c_str());
    }
}

LONG WINAPI unhandledExceptionFilter(LPEXCEPTION_POINTERS exceptionPointers)
{
    const std::wstring directory = g_dumpDirectory;
    if (!directory.empty())
    {
        ::CreateDirectoryW(directory.c_str(), nullptr);
        if (exceptionPointers != nullptr)
        {
            wchar_t path[MAX_PATH]{};
            const DWORD processId = ::GetCurrentProcessId();
            const ULONGLONG ticks = ::GetTickCount64();
            const int written = swprintf_s(path, MAX_PATH, L"%s\\audient_console_%lu_%llu.dmp",
                                           directory.c_str(), processId, ticks);
            if (written > 0)
            {
                HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
                    exceptionInfo.ThreadId = ::GetCurrentThreadId();
                    exceptionInfo.ExceptionPointers = exceptionPointers;
                    exceptionInfo.ClientPointers = FALSE;
                    ::MiniDumpWriteDump(::GetCurrentProcess(), processId, file, MiniDumpNormal,
                                        &exceptionInfo, nullptr, nullptr);
                    ::CloseHandle(file);
                    trimOldDumps(directory);
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace audient::diagnostics
{

void CrashHandler::install()
{
    if (g_dumpDirectory.empty())
    {
        g_dumpDirectory = defaultDumpDirectory();
    }
    ::SetUnhandledExceptionFilter(&unhandledExceptionFilter);
}

void CrashHandler::uninstall()
{
    ::SetUnhandledExceptionFilter(nullptr);
}

void CrashHandler::setDumpDirectory(std::wstring path)
{
    g_dumpDirectory = std::move(path);
}

const std::wstring& CrashHandler::dumpDirectory()
{
    return g_dumpDirectory;
}

} // namespace audient::diagnostics

#else

namespace audient::diagnostics
{

void CrashHandler::install() {}
void CrashHandler::uninstall() {}
void CrashHandler::setDumpDirectory(std::wstring) {}
const std::wstring& CrashHandler::dumpDirectory() { static const std::wstring empty; return empty; }

} // namespace audient::diagnostics

#endif