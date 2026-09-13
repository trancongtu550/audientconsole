#include "core/Version.h"

#include <gtest/gtest.h>

#include <regex>
#include <string>

namespace core = audient::core;

TEST(CoreVersionSmoke, VersionStringMatchesSemanticVersionShape)
{
    const std::string version = core::versionString();
    ASSERT_FALSE(version.empty());
    EXPECT_TRUE(std::regex_match(version, std::regex(R"(\d+\.\d+\.\d+)")));
}

TEST(CoreVersionSmoke, ComponentsAreNonNegativeIntegers)
{
    EXPECT_GE(core::versionMajor(), 0);
    EXPECT_GE(core::versionMinor(), 0);
    EXPECT_GE(core::versionPatch(), 0);
    EXPECT_EQ(core::versionString(), std::to_string(core::versionMajor()) + "." +
                                         std::to_string(core::versionMinor()) + "." +
                                         std::to_string(core::versionPatch()));
}

TEST(CoreVersionSmoke, BuildMetadataIsPresent)
{
    EXPECT_FALSE(std::string(core::buildType()).empty());
    EXPECT_FALSE(std::string(core::gitSha()).empty());
    EXPECT_FALSE(std::string(core::buildTime()).empty());
}

TEST(CoreVersionSmoke, FullVersionStringCarriesMetadata)
{
    const std::string full = core::fullVersionString();
    ASSERT_FALSE(full.empty());
    EXPECT_NE(full.find('('), std::string::npos);
    EXPECT_NE(full.find(')'), std::string::npos);
    EXPECT_EQ(full.find(core::versionString()), 0);
}