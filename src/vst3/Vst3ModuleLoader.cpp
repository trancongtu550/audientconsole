#include "vst3/Vst3ModuleLoader.h"

#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace audient::vst3
{

namespace
{

using GetPluginFactoryFn = Steinberg::IPluginFactory* (*)();

std::wstring toWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string toUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

bool pathExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

bool isDirectory(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool isRegularFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool endsWithDotVst3(const std::wstring& value)
{
    const std::wstring suffix = L".vst3";
    const std::size_t length = value.size();
    const std::size_t suffixLength = suffix.size();
    if (length < suffixLength)
    {
        return false;
    }
    // Windows filesystem lookups are case-insensitive; accept any casing.
    std::wstring tail = value.substr(length - suffixLength);
    for (wchar_t& ch : tail)
    {
        ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    }
    return tail == suffix;
}

bool endsWithSeparator(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

// Returns the resolved architecture module path, or false with a reason.
bool resolveBundleModule(const std::wstring& bundleDir, std::wstring& resolved, std::wstring& error)
{
    std::wstring base = bundleDir;
    while (!base.empty() && endsWithSeparator(base.back()))
    {
        base.pop_back();
    }

    const std::wstring contentsDir = base + L"\\Contents";
    if (!isDirectory(contentsDir))
    {
        error = L"bundle has no Contents directory: " + base;
        return false;
    }

    const std::wstring archDir = contentsDir + L"\\x86_64-win";
    if (!isDirectory(archDir))
    {
        error = L"bundle has no Contents\\x86_64-win directory: " + base;
        return false;
    }

    std::vector<std::wstring> modules;
    WIN32_FIND_DATAW findData{};
    HANDLE findHandle = FindFirstFileW((archDir + L"\\*.vst3").c_str(), &findData);
    if (findHandle != INVALID_HANDLE_VALUE)
    {
        do
        {
            // Ignore subdirectories; accept .vst3 module files only.
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && endsWithDotVst3(findData.cFileName))
            {
                modules.push_back(archDir + L"\\" + findData.cFileName);
            }
        } while (FindNextFileW(findHandle, &findData) != 0);
        FindClose(findHandle);
    }

    if (modules.empty())
    {
        error = L"bundle has no *.vst3 module in Contents\\x86_64-win: " + archDir;
        return false;
    }
    if (modules.size() > 1)
    {
        error = L"bundle arch directory contains multiple *.vst3 modules (ambiguous): " + archDir;
        return false;
    }

    resolved = modules[0];
    return true;
}

} // namespace

Vst3ModuleLoader::~Vst3ModuleLoader()
{
    unload();
}

bool Vst3ModuleLoader::resolveModulePath(const std::string& modulePath, std::string& resolvedPath, std::string& error)
{
    resolvedPath.clear();
    error.clear();

    if (modulePath.empty())
    {
        error = "empty module path";
        return false;
    }

    const std::wstring widePath = toWide(modulePath);
    if (!pathExists(widePath))
    {
        error = "path does not exist: " + modulePath;
        return false;
    }

    if (isRegularFile(widePath))
    {
        resolvedPath = modulePath;
        return true;
    }

    std::wstring resolvedWide;
    std::wstring resolveError;
    if (!resolveBundleModule(widePath, resolvedWide, resolveError))
    {
        error = toUtf8(resolveError);
        return false;
    }
    resolvedPath = toUtf8(resolvedWide);
    return true;
}

bool Vst3ModuleLoader::load(const std::string& modulePath)
{
    unload();

    if (modulePath.empty())
    {
        m_lastError = "empty module path";
        return false;
    }

    std::string resolvedPath;
    std::string resolveError;
    if (!resolveModulePath(modulePath, resolvedPath, resolveError))
    {
        m_lastError = resolveError;
        return false;
    }

    const std::wstring widePath = toWide(resolvedPath);
    HMODULE module = LoadLibraryW(widePath.c_str());
    if (module == nullptr)
    {
        m_lastError = "LoadLibraryW failed for " + resolvedPath;
        return false;
    }

    const auto getFactory = reinterpret_cast<GetPluginFactoryFn>(GetProcAddress(module, "GetPluginFactory"));
    if (getFactory == nullptr)
    {
        m_lastError = "module does not export GetPluginFactory: " + resolvedPath;
        FreeLibrary(module);
        return false;
    }

    Steinberg::IPluginFactory* factory = nullptr;
    factory = getFactory();
    if (factory == nullptr)
    {
        m_lastError = "GetPluginFactory returned null for " + resolvedPath;
        FreeLibrary(module);
        return false;
    }

    m_module = module;
    m_factory = factory;
    m_modulePath = resolvedPath;
    m_lastError.clear();
    return true;
}

void Vst3ModuleLoader::unload()
{
    if (m_module == nullptr)
    {
        m_factory = nullptr;
        m_modulePath.clear();
        return;
    }
    // The factory is module-owned (attached via Vst3Host). Unloading here would
    // invalidate any borrowed factory pointer; callers must detach before this.
    if (m_factory != nullptr)
    {
        m_factory = nullptr;
    }
    FreeLibrary(static_cast<HMODULE>(m_module));
    m_module = nullptr;
    m_modulePath.clear();
}

bool Vst3ModuleLoader::loaded() const
{
    return m_module != nullptr && m_factory != nullptr;
}

Steinberg::IPluginFactory* Vst3ModuleLoader::factory() const
{
    return m_factory;
}

const std::string& Vst3ModuleLoader::modulePath() const
{
    return m_modulePath;
}

const std::string& Vst3ModuleLoader::lastError() const
{
    return m_lastError;
}

} // namespace audient::vst3