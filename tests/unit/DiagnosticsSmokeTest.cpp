#include "diagnostics/CrashHandler.h"
#include "diagnostics/DiagnosticBundle.h"

#include <gtest/gtest.h>

#include <string>

TEST(DiagnosticsSmoke, BundleCollectsVersionAndOsMetadata)
{
    const audient::diagnostics::DiagnosticBundle bundle = audient::diagnostics::collectDiagnosticBundle();

    EXPECT_FALSE(bundle.appVersion.empty());
    EXPECT_FALSE(bundle.gitSha.empty());
    EXPECT_FALSE(bundle.buildType.empty());
    EXPECT_FALSE(bundle.buildTime.empty());
    EXPECT_FALSE(bundle.osVersion.empty());

    const std::string json = bundle.toJson();
    EXPECT_NE(json.find("\"appVersion\""), std::string::npos);
    EXPECT_NE(json.find("\"osVersion\""), std::string::npos);
    EXPECT_NE(json.find("Windows"), std::string::npos);
}

TEST(DiagnosticsSmoke, BundleJsonCarriesNoSensitiveKeys)
{
    const audient::diagnostics::DiagnosticBundle bundle = audient::diagnostics::collectDiagnosticBundle();
    const std::string json = bundle.toJson();

    EXPECT_EQ(json.find("\"audio\""), std::string::npos);
    EXPECT_EQ(json.find("\"secret\""), std::string::npos);
    EXPECT_EQ(json.find("\"token\""), std::string::npos);
    EXPECT_EQ(json.find("\"password\""), std::string::npos);
}

TEST(DiagnosticsSmoke, CrashHandlerInstallUninstallRoundTrip)
{
    audient::diagnostics::CrashHandler::install();
    const std::wstring directory = audient::diagnostics::CrashHandler::dumpDirectory();
    audient::diagnostics::CrashHandler::uninstall();

#if defined(_WIN32)
    EXPECT_FALSE(directory.empty());
    EXPECT_NE(directory.find(L"AudientConsole"), std::wstring::npos);
#endif
}