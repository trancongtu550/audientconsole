#include "vst3/Vst3Host.h"
#include "vst3/Vst3ModuleLoader.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace
{

// RAII temp directory under the system temp path; removed on destruction.
class TempDir
{
public:
    explicit TempDir(const char* prefix)
        : m_path(makeUniqueDir(prefix))
    {
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return m_path; }

private:
    static std::filesystem::path makeUniqueDir(const char* prefix)
    {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        std::random_device rd;
        std::uniform_int_distribution<unsigned long long> dist(1, 99999999);
        const std::filesystem::path dir = base / (std::string(prefix) + std::to_string(dist(rd)));
        std::filesystem::create_directories(dir);
        return dir;
    }

    std::filesystem::path m_path;
};

// Locates a test-only module built next to the integration test executable.
std::filesystem::path testModulePath(const char* moduleFileName)
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    const std::filesystem::path exeDir =
        length > 0 ? std::filesystem::path(modulePath).parent_path() : std::filesystem::current_path();
    return exeDir / moduleFileName;
}

// Copies a source file into a temporary Windows bundle layout as the sole
// architecture module; returns the bundle directory.
std::filesystem::path makeBundleWith(const TempDir& root, const std::filesystem::path& source, const char* bundleName,
                                     const char* moduleLeaf)
{
    const std::filesystem::path bundleDir = root.path() / bundleName;
    const std::filesystem::path archDir = bundleDir / "Contents" / "x86_64-win";
    std::filesystem::create_directories(archDir);
    std::filesystem::copy_file(source, archDir / moduleLeaf, std::filesystem::copy_options::overwrite_existing);
    return bundleDir;
}

} // namespace

TEST(Vst3ModuleLoaderBundleTest, LoadsValidDirectModule)
{
    const std::filesystem::path module = testModulePath(TEST_PASSTHROUGH_VST3_MODULE);
    ASSERT_TRUE(std::filesystem::exists(module)) << "test module DLL must be built: " << module;

audient::vst3::Vst3ModuleLoader loader;
    ASSERT_TRUE(loader.load(module.string())) << loader.lastError();
    ASSERT_TRUE(loader.loaded());
    EXPECT_NE(loader.factory(), nullptr);
    EXPECT_EQ(std::filesystem::path(loader.modulePath()), module);

    // The factory must be usable by the host and expose the expected class.
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(loader.factory()));
    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    ASSERT_EQ(classes.size(), 1u);
    EXPECT_EQ(classes[0].name, "Test Passthrough");
    EXPECT_EQ(classes[0].category, "Audio Effect");
}

TEST(Vst3ModuleLoaderBundleTest, LoadsValidWindowsBundle)
{
    const std::filesystem::path module = testModulePath(TEST_PASSTHROUGH_VST3_MODULE);
    ASSERT_TRUE(std::filesystem::exists(module));

    TempDir root("vst3-bundle-load");
    const std::filesystem::path bundleDir = makeBundleWith(root, module, "RealPlugin.vst3", "RealPlugin.vst3");

    // The user hands the loader the bundle directory; it must derive the module.
audient::vst3::Vst3ModuleLoader loader;
    ASSERT_TRUE(loader.load(bundleDir.string())) << loader.lastError();
    ASSERT_TRUE(loader.loaded());
    EXPECT_NE(loader.factory(), nullptr);
    EXPECT_EQ(std::filesystem::path(loader.modulePath()), bundleDir / "Contents/x86_64-win/RealPlugin.vst3");

    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(loader.factory()));
    EXPECT_EQ(host.classes().size(), 1u);
}

TEST(Vst3ModuleLoaderBundleTest, BundleModuleNameDoesNotNeedToMatchBundle)
{
    const std::filesystem::path module = testModulePath(TEST_PASSTHROUGH_VST3_MODULE);
    ASSERT_TRUE(std::filesystem::exists(module));

    // Module file name differs from the bundle name; resolution must enumerate
    // the arch dir, not guess "<bundleName>.vst3".
    TempDir root("vst3-bundle-mismatch");
    const std::filesystem::path bundleDir = makeBundleWith(root, module, "BundleName.vst3", "TotallyDifferent.vst3");

    audient::vst3::Vst3ModuleLoader loader;
    ASSERT_TRUE(loader.load(bundleDir.string())) << loader.lastError();
    ASSERT_TRUE(loader.loaded());
    EXPECT_EQ(std::filesystem::path(loader.modulePath()), bundleDir / "Contents/x86_64-win/TotallyDifferent.vst3");
}

TEST(Vst3ModuleLoaderBundleTest, InvalidBundleRejectedWithNoModule)
{
    TempDir root("vst3-bundle-invalid");
    const std::filesystem::path bad = root.path() / "Bad.vst3";
    std::filesystem::create_directories(bad / "Contents");

    audient::vst3::Vst3ModuleLoader loader;
    EXPECT_FALSE(loader.load(bad.string()));
    EXPECT_FALSE(loader.loaded());
    EXPECT_NE(loader.lastError().find("x86_64-win"), std::string::npos) << loader.lastError();
}

TEST(Vst3ModuleLoaderBundleTest, BundleWithNonFactoryModuleResolvesButFailsToLoad)
{
    // The test-only non-factory module is a real loadable PE that deliberately
    // lacks GetPluginFactory. Placed as the sole bundle candidate, filesystem
    // resolution must succeed while the factory lookup must fail — clearly
    // distinct from a missing module or missing arch dir.
    const std::filesystem::path nonFactory = testModulePath(TEST_NONFACTORY_VST3_MODULE);
    ASSERT_TRUE(std::filesystem::exists(nonFactory));

    TempDir root("vst3-bundle-nofactory");
    const std::filesystem::path bundleDir =
        makeBundleWith(root, nonFactory, "NotAPlugin.vst3", "NotAPlugin.vst3");

    std::string resolved;
    std::string error;
    ASSERT_TRUE(audient::vst3::Vst3ModuleLoader::resolveModulePath(bundleDir.string(), resolved, error))
        << "filesystem resolution must succeed for a real PE inside a bundle: " << error;
    EXPECT_EQ(std::filesystem::path(resolved), bundleDir / "Contents/x86_64-win/NotAPlugin.vst3");

    audient::vst3::Vst3ModuleLoader loader;
    EXPECT_FALSE(loader.load(bundleDir.string()));
    EXPECT_FALSE(loader.loaded());
    // Same factory-lookup error as the direct-file non-factory case: the module
    // loaded fine, only GetPluginFactory is missing.
    EXPECT_NE(loader.lastError().find("GetPluginFactory"), std::string::npos) << loader.lastError();
}
