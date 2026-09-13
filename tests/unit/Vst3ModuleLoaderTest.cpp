#include "vst3/Vst3ModuleLoader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace
{

// RAII temp directory under the system temp path; removed on destruction.
class TempDir
{
public:
    explicit TempDir(const char* prefix)
        : m_path([](const char* p) {
              std::filesystem::path base = std::filesystem::temp_directory_path();
              std::random_device rd;
              std::uniform_int_distribution<unsigned long long> dist(1, 99999999);
              std::filesystem::path path = base / (std::string(p) + std::to_string(dist(rd)));
              std::filesystem::create_directories(path);
              return path;
          }(prefix))
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

    std::filesystem::path createFile(const std::filesystem::path& relative, const std::string& bytes = {})
    {
        std::filesystem::path full = m_path / relative;
        if (full.has_parent_path())
        {
            std::filesystem::create_directories(full.parent_path());
        }
        std::ofstream stream(full, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        return full;
    }

    void createDir(const std::filesystem::path& relative) { std::filesystem::create_directories(m_path / relative); }

private:
    std::filesystem::path m_path;
};

// --- filesystem-resolution boundary: resolveModulePath(...) only -------------

void expectResolve(const std::filesystem::path& input, const std::filesystem::path& expected)
{
    std::string resolved;
    std::string error;
    ASSERT_TRUE(audient::vst3::Vst3ModuleLoader::resolveModulePath(input.string(), resolved, error))
        << "resolution must succeed for " << input.string() << ": " << error;
    // Compare as path components (Windows separator/case tolerant);
    // resolveModulePath returns OS-native separators.
    EXPECT_EQ(std::filesystem::path(resolved), expected);
}

void expectResolveFailure(const std::filesystem::path& input, const char* errorNeedle)
{
    std::string resolved;
    std::string error;
    EXPECT_FALSE(audient::vst3::Vst3ModuleLoader::resolveModulePath(input.string(), resolved, error))
        << "resolution must fail for " << input.string();
    EXPECT_TRUE(resolved.empty());
    EXPECT_FALSE(error.empty());
    if (errorNeedle != nullptr)
    {
        EXPECT_NE(error.find(errorNeedle), std::string::npos) << "error must mention " << errorNeedle << ": " << error;
    }
}

} // namespace

// --- loader contract (empty/missing/invalid PE) ------------------------------

TEST(Vst3ModuleLoaderTest, RejectsEmptyPath)
{
    audient::vst3::Vst3ModuleLoader loader;
    EXPECT_FALSE(loader.load(""));
    EXPECT_FALSE(loader.loaded());
    EXPECT_FALSE(loader.lastError().empty());
    EXPECT_EQ(loader.factory(), nullptr);
}

TEST(Vst3ModuleLoaderTest, NonexistentPathFailsResolutionBeforeLoading)
{
    TempDir dir("vst3loader-ne");
    // Failure boundary #1: the path does not exist -> resolution itself fails;
    // LoadLibrary is never reached.
    expectResolveFailure(dir.path() / "NoSuch.vst3", "does not exist");
}

// --- filesystem resolution: direct file -------------------------------------

TEST(Vst3ModuleLoaderTest, ResolvesExistingDirectFile)
{
    TempDir dir("vst3loader-direct");
    const std::filesystem::path file = dir.createFile("Direct.vst3", "seed");

    // A direct module file resolves to itself.
    expectResolve(file, file);
}

// --- filesystem resolution: Windows bundle layout ----------------------------

TEST(Vst3ModuleLoaderTest, ResolvesValidWindowsBundle)
{
    TempDir dir("vst3loader-bundle");
    dir.createFile("Plugin.vst3/Contents/x86_64-win/Plugin.vst3", "seed");

    expectResolve(dir.path() / "Plugin.vst3", dir.path() / "Plugin.vst3/Contents/x86_64-win/Plugin.vst3");
}

TEST(Vst3ModuleLoaderTest, BundleMissingContentsFails)
{
    TempDir dir("vst3loader-contents");
    dir.createFile("Plugin.vst3/placeholder.txt", "seed");
    expectResolveFailure(dir.path() / "Plugin.vst3", "Contents");
}

TEST(Vst3ModuleLoaderTest, BundleMissingArchDirFails)
{
    TempDir dir("vst3loader-arch");
    dir.createFile("Plugin.vst3/Contents/placeholder.txt", "seed");
    expectResolveFailure(dir.path() / "Plugin.vst3", "x86_64-win");
}

TEST(Vst3ModuleLoaderTest, BundleEmptyArchDirFails)
{
    TempDir dir("vst3loader-emptyarch");
    // Arch dir exists but contains no *.vst3 at all.
    dir.createDir("Plugin.vst3/Contents/x86_64-win");
    expectResolveFailure(dir.path() / "Plugin.vst3", "no *.vst3 module");
}

TEST(Vst3ModuleLoaderTest, BundleArchDirWithOnlyNonModuleFilesFails)
{
    TempDir dir("vst3loader-nonmodule");
    dir.createFile("Plugin.vst3/Contents/x86_64-win/resources.bin", "seed");
    expectResolveFailure(dir.path() / "Plugin.vst3", "no *.vst3 module");
}

TEST(Vst3ModuleLoaderTest, BundleMultipleCandidateModulesIsAmbiguous)
{
    TempDir dir("vst3loader-multi");
    dir.createFile("Plugin.vst3/Contents/x86_64-win/One.vst3", "seed");
    dir.createFile("Plugin.vst3/Contents/x86_64-win/Two.vst3", "seed");
    expectResolveFailure(dir.path() / "Plugin.vst3", "ambiguous");
}

TEST(Vst3ModuleLoaderTest, BundleSoleCandidateMayDifferFromBundleName)
{
    TempDir dir("vst3loader-wrongname");
    // A single module candidate whose file name does NOT match the bundle name
    // must still be selected. The resolver must not hard-code "<bundle>.vst3".
    dir.createFile("Foo.vst3/Contents/x86_64-win/ActualBinary.vst3", "seed");

    expectResolve(dir.path() / "Foo.vst3", dir.path() / "Foo.vst3/Contents/x86_64-win/ActualBinary.vst3");
}

// --- module-loading boundary: file exists but is not a loadable module -------

TEST(Vst3ModuleLoaderTest, NonModuleBinaryLoadsFailAfterResolve)
{
    TempDir dir("vst3loader-badpe");
    const std::filesystem::path file = dir.createFile("NotAPe.vst3", "this is not a PE executable");

    // Failure boundary #2: resolution succeeds (file exists) but LoadLibrary
    // fails for a non-PE payload.
    audient::vst3::Vst3ModuleLoader loader;
    EXPECT_FALSE(loader.load(file.string()));
    EXPECT_FALSE(loader.loaded());
    EXPECT_NE(loader.lastError().find("LoadLibraryW failed"), std::string::npos) << loader.lastError();
}

TEST(Vst3ModuleLoaderTest, RejectsModuleWithoutGetPluginFactory)
{
    // Failure boundary #3: LoadLibrary succeeds for a real PE (kernel32) but
    // the module does not export GetPluginFactory; load must fail with a clear
    // factory-lookup error.
    audient::vst3::Vst3ModuleLoader loader;
    EXPECT_FALSE(loader.load("C:\\Windows\\System32\\kernel32.dll"));
    EXPECT_FALSE(loader.loaded());
    EXPECT_NE(loader.lastError().find("GetPluginFactory"), std::string::npos) << loader.lastError();
}

TEST(Vst3ModuleLoaderTest, UnloadOnEmptyIsIdempotent)
{
    audient::vst3::Vst3ModuleLoader loader;
    loader.unload();
    loader.unload();
    EXPECT_FALSE(loader.loaded());
}
