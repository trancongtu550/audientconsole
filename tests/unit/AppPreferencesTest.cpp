#include "preferences/AppPreferences.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace prefs = audient::preferences;

namespace
{

const prefs::AppPreferences kDefaults{};

} // namespace

TEST(AppPreferencesTest, DefaultsAreTheApprovedValues)
{
    EXPECT_EQ(kDefaults.virtualMicSource, 0);
    EXPECT_TRUE(kDefaults.closeToTray);
    EXPECT_FALSE(kDefaults.startMinimized);
}

TEST(AppPreferencesTest, RoundTripPreservesEveryField)
{
    prefs::AppPreferences value;
    value.virtualMicSource = 1;
    value.closeToTray = false;
    value.startMinimized = true;

    const std::string text = prefs::serializePreferences(value);
    EXPECT_EQ(text.find("theme8bit"), std::string::npos)
        << "the retired 8-bit key must never be written";
    const prefs::AppPreferences parsed = prefs::parsePreferences(text);
    EXPECT_EQ(parsed, value);
}

TEST(AppPreferencesTest, MissingOrEmptyDocumentUsesDefaults)
{
    std::string warning;
    EXPECT_EQ(prefs::parsePreferences(std::string(), &warning), kDefaults);
    EXPECT_FALSE(warning.empty());
}

TEST(AppPreferencesTest, CorruptDocumentUsesDefaults)
{
    EXPECT_EQ(prefs::parsePreferences("{ this is not json", nullptr), kDefaults);
    EXPECT_EQ(prefs::parsePreferences("{\"closeToTray\": tru}", nullptr), kDefaults);
    EXPECT_EQ(prefs::parsePreferences("[1,2,3]", nullptr), kDefaults);
    EXPECT_EQ(prefs::parsePreferences("{}", nullptr), kDefaults);
}

TEST(AppPreferencesTest, UnknownKeysAndTypesAreIgnoredForForwardCompatibility)
{
    const std::string document = R"({
        "schema": 99,
        "futureFeature": { "nested": [1, 2, {"x": true}], "y": null },
        "another": "string",
        "virtualMicSource": 1,
        "closeToTray": false,
        "startMinimized": true
    })";
    const prefs::AppPreferences parsed = prefs::parsePreferences(document);
    EXPECT_EQ(parsed.virtualMicSource, 1);
    EXPECT_FALSE(parsed.closeToTray);
    EXPECT_TRUE(parsed.startMinimized);
}

// Migration: a settings.json written by a build that had the 8-bit theme must
// still load. The stale key is accepted and ignored; every surviving key is
// applied and startup is never failed.
TEST(AppPreferencesTest, RetiredTheme8bitKeyIsIgnoredAndOtherKeysStillLoad)
{
    const std::string document =
        R"({"schema":1,"theme8bit":true,"virtualMicSource":1,"closeToTray":false,"startMinimized":true})";
    prefs::AppPreferences parsed;
    ASSERT_NO_THROW(parsed = prefs::parsePreferences(document, nullptr));
    EXPECT_EQ(parsed.virtualMicSource, 1);
    EXPECT_FALSE(parsed.closeToTray);
    EXPECT_TRUE(parsed.startMinimized);
}

TEST(AppPreferencesTest, OutOfRangeOrWrongTypeValuesKeepDefaults)
{
    const prefs::AppPreferences parsed = prefs::parsePreferences(
        R"({"virtualMicSource": 7, "closeToTray": 0})");
    EXPECT_EQ(parsed, kDefaults);
}

TEST(AppPreferencesTest, RetiredTheme8bitFileLoadsWithoutErrorOrBlank)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "audient_prefs_migration_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "settings.json").string();
    {
        std::ofstream old(path, std::ios::binary | std::ios::trunc);
        old << R"({"schema":1,"theme8bit":true,"virtualMicSource":1,"closeToTray":true,"startMinimized":false})";
    }
    prefs::AppPreferences loaded;
    ASSERT_TRUE(prefs::loadPreferencesFromFile(path, loaded, nullptr));
    EXPECT_EQ(loaded.virtualMicSource, 1);
    EXPECT_TRUE(loaded.closeToTray);
    EXPECT_FALSE(loaded.startMinimized);
    std::filesystem::remove_all(dir, ec);
}

TEST(AppPreferencesTest, SaveAndLoadFileRoundTripAndRecover)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "audient_prefs_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const std::string path = (dir / "settings.json").string();

    prefs::AppPreferences value;
    value.virtualMicSource = 1;
    value.closeToTray = false;
    value.startMinimized = true;
    ASSERT_TRUE(prefs::savePreferencesToFile(path, value, nullptr));

    prefs::AppPreferences loaded;
    ASSERT_TRUE(prefs::loadPreferencesFromFile(path, loaded, nullptr));
    EXPECT_EQ(loaded, value);

    // Corrupt file -> safe defaults, still succeeds (audio startup unaffected).
    {
        std::ofstream bad(path, std::ios::binary | std::ios::trunc);
        bad << "{ corrupt";
    }
    prefs::AppPreferences recovered;
    EXPECT_TRUE(prefs::loadPreferencesFromFile(path, recovered, nullptr));
    EXPECT_EQ(recovered, kDefaults);

    // Missing file -> safe defaults.
    std::filesystem::remove(path, ec);
    prefs::AppPreferences missing;
    EXPECT_TRUE(prefs::loadPreferencesFromFile(path, missing, nullptr));
    EXPECT_EQ(missing, kDefaults);

    std::filesystem::remove_all(dir, ec);
}
