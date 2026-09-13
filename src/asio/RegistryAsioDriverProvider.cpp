#include "asio/RegistryAsioDriverProvider.h"

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace audient::asio
{

namespace
{

std::string toUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::vector<DriverIdentity> enumerateAsioRoot(const wchar_t* root)
{
    std::vector<DriverIdentity> found;
    HKEY hRoot = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
    {
        return found;
    }

    DWORD index = 0;
    for (;;)
    {
        wchar_t subkeyName[256]{};
        DWORD nameSize = static_cast<DWORD>(std::size(subkeyName));
        const LONG status = RegEnumKeyExW(hRoot, index, subkeyName, &nameSize, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        ++index;
        if (status != ERROR_SUCCESS || nameSize == 0)
        {
            continue;
        }

        HKEY hSub = nullptr;
        if (RegOpenKeyExW(hRoot, subkeyName, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
        {
            continue;
        }

        wchar_t clsid[128]{};
        DWORD clsidBytes = static_cast<DWORD>(sizeof(clsid));
        const LONG clsidStatus = RegQueryValueExW(hSub, L"CLSID", nullptr, nullptr, reinterpret_cast<LPBYTE>(clsid), &clsidBytes);
        RegCloseKey(hSub);
        if (clsidStatus != ERROR_SUCCESS || clsid[0] == 0)
        {
            continue;
        }

        DriverIdentity identity;
        identity.name = toUtf8(std::wstring(subkeyName, nameSize));
        identity.clsid = toUtf8(clsid);
        for (const DriverIdentity& existing : found)
        {
            if (existing.clsid == identity.clsid)
            {
                clsid[0] = 0;
                break;
            }
        }
        if (clsid[0] != 0)
        {
            found.push_back(std::move(identity));
        }
    }
    RegCloseKey(hRoot);
    return found;
}

} // namespace

std::vector<DriverIdentity> RegistryAsioDriverProvider::available() const
{
    std::vector<DriverIdentity> result = enumerateAsioRoot(L"SOFTWARE\\ASIO");
    std::vector<DriverIdentity> from32 = enumerateAsioRoot(L"SOFTWARE\\WOW6432Node\\ASIO");
    for (DriverIdentity& identity : from32)
    {
        bool duplicate = false;
        for (const DriverIdentity& existing : result)
        {
            if (existing.clsid == identity.clsid)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            result.push_back(std::move(identity));
        }
    }
    return result;
}

std::unique_ptr<IAsioDriver> RegistryAsioDriverProvider::open(const DriverIdentity& identity, std::string& error)
{
    (void)identity;
    error = "native ASIO driver open is blocked: the pinned Steinberg ASIO SDK is required and not configured "
            "(AUDIENT_ASIO_SDK_DIR). Enumeration and identity matching are complete; no IASIO vtable call is made.";
    return nullptr;
}

} // namespace audient::asio