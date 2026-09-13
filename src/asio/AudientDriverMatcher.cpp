#include "asio/AudientDriverMatcher.h"

#include <algorithm>
#include <cctype>

namespace audient::asio
{

namespace
{

bool containsInsensitive(const std::string& haystack, const char* needle)
{
    return std::search(haystack.begin(), haystack.end(), needle, needle + std::char_traits<char>::length(needle),
                       [](char lhs, char rhs) { return std::tolower(static_cast<unsigned char>(lhs)) == static_cast<unsigned char>(rhs); }) != haystack.end();
}

} // namespace

void AudientDriverMatcher::rememberPreferredIdentity(std::string clsid)
{
    m_preferredClsid = std::move(clsid);
}

bool AudientDriverMatcher::match(const std::vector<DriverIdentity>& available, DriverIdentity& out) const
{
    out = DriverIdentity{};
    if (available.empty())
    {
        return false;
    }

    if (!m_preferredClsid.empty())
    {
        for (const DriverIdentity& identity : available)
        {
            if (identity.clsid == m_preferredClsid)
            {
                out = identity;
                return true;
            }
        }
    }

    for (const DriverIdentity& identity : available)
    {
        if (containsInsensitive(identity.name, "audient"))
        {
            out = identity;
            return true;
        }
    }

    return false;
}

} // namespace audient::asio