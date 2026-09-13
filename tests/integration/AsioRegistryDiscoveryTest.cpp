#include "asio/AsioBackend.h"
#include "asio/AudientDriverMatcher.h"
#include "asio/RegistryAsioDriverProvider.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace
{

bool containsInsensitive(const std::string& haystack, const char* needle)
{
    const std::string lowerHaystack = [&]() {
        std::string lowered = haystack;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered;
    }();
    return lowerHaystack.find(needle) != std::string::npos;
}

} // namespace

TEST(AsioRegistry, EnumeratesRealDriversAndMatchesAudient)
{
    audient::asio::RegistryAsioDriverProvider provider;
    const std::vector<audient::asio::DriverIdentity> available = provider.available();

    const bool hasAudient = std::any_of(available.begin(), available.end(), [](const auto& identity) {
        return containsInsensitive(identity.name, "audient");
    });

    if (!hasAudient)
    {
        GTEST_SKIP() << "no Audient ASIO driver registered on this machine (HKLM\\SOFTWARE\\ASIO); full discovery cannot run";
    }

    audient::asio::AudientDriverMatcher matcher;
    audient::asio::DriverIdentity matched;
    ASSERT_TRUE(matcher.match(available, matched)) << "the Audient ASIO driver must be selectable from the real registry";
    EXPECT_TRUE(containsInsensitive(matched.name, "audient"));
    EXPECT_FALSE(matched.clsid.empty());
    EXPECT_EQ(matched.clsid.front(), '{');
    EXPECT_EQ(matched.clsid.back(), '}');
}

TEST(AsioRegistry, BackendSelectsAudientButOpenStaysSdkBlocked)
{
    audient::asio::RegistryAsioDriverProvider provider;
    const std::vector<audient::asio::DriverIdentity> available = provider.available();

    const bool hasAudient = std::any_of(available.begin(), available.end(), [](const auto& identity) {
        return containsInsensitive(identity.name, "audient");
    });
    if (!hasAudient)
    {
        GTEST_SKIP() << "no Audient ASIO driver registered on this machine; open-block behavior cannot run";
    }

    audient::asio::AsioBackend backend(provider);
    std::string error;
    EXPECT_FALSE(backend.selectAudientDevice(error)) << "driver open must stay blocked without the pinned ASIO SDK";
    EXPECT_TRUE(error.find("ASIO SDK") != std::string::npos) << error;
    EXPECT_NE(backend.state(), audient::asio::DeviceState::Ready);
}