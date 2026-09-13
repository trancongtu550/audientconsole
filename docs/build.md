# Building Audient Console

Target platform: Windows 11 x64, Visual Studio 2022 C++ toolchain, C++20.

## Prerequisites

- Windows 11 x64
- Visual Studio 2022 with the **Desktop development with C++** workload
  (MSVC v143 + Windows 10/11 SDK), or the equivalent Build Tools
- CMake 3.25 or newer (shipped with Visual Studio under
  `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`)
- Node.js 18+ and npm (for the WebView2 frontend bundle)
- Network access on the first configure (CMake fetches GoogleTest, the
  Steinberg VST3 SDK, and the Microsoft WebView2 SDK; all pinned and
  hash-verified — see `cmake/CppDependencies.cmake` and
  `docs/dependency-inventory.json`)
- Optional: the Steinberg ASIO SDK (see below)

The `Visual Studio 17 2022` generator locates the toolchain itself; `cmake`
and `cl.exe` do not need to be on `PATH`.

## Configure, build, test

```powershell
cmake --preset configure-debug
cmake --build --preset build-debug
ctest --preset test-debug
```

The same three commands work for the `relwithdebinfo` and `release` preset
families.

| Preset family | Configuration | Purpose |
|---|---|---|
| `configure-debug` / `build-debug` / `test-debug` | Debug | Development; allocation detector active |
| `configure-relwithdebinfo` / ... | RelWithDebInfo | Optimized with symbols |
| `configure-release` / ... | Release | Release verification |

A single test binary can be run directly for verbose output:

```powershell
.\build\configure-debug\tests\unit\Debug\audient_console_unit_tests.exe --gtest_list_tests
```

### BUILD-002 — missing dependency fails configure

```powershell
ctest --preset test-debug -R Build002 --output-on-failure
```

It runs a clean configure with `FETCHCONTENT_FULLY_DISCONNECTED=ON` and an
empty dependency cache and asserts CMake fails with a clear,
dependency-naming error. Reproduce directly:

```powershell
powershell -File .\tools\verify-build-002.ps1 -SourceDir "<repo>" -BuildRoot "<temp>" -Cmake "<path-to-cmake>"
```

## ASIO SDK (optional)

`audient_asio` builds and is tested against a deterministic simulated driver,
so no SDK is required for the default build. Audient ASIO driver **enumeration
and matching** are SDK-free (`RegistryAsioDriverProvider` reads
`HKLM\SOFTWARE\ASIO`). With the SDK configured, the native adapter
(`src/asio/sdk/`) compiles and the real-driver tests run.

The Steinberg ASIO SDK is not redistributed with this repository. Download it
from Steinberg, then point the cache variable at its `ASIOSDK` directory:

```powershell
cmake --preset configure-debug -DAUDIENT_ASIO_SDK_DIR="<path-to-ASIOSDK>"
```

## VST3 SDK

The `audient_vst3` module uses the official Steinberg VST3 SDK pinned by commit
hash via CMake `FetchContent` (`vst3_pluginterfaces`, `vst3_base`,
`vst3_public_sdk`). It is fetched automatically at configure time. Only the
interface headers and two source files are compiled; the SDK is never built
wholesale.

## WebView2 SDK (Daily GUI)

The Daily GUI hosts its presentation layer in Microsoft WebView2.

- The SDK (`Microsoft.Web.WebView2`, pinned version) is downloaded and
  SHA512-verified at configure time by `cmake/CppDependencies.cmake`
  (`audient_add_webview2sdk()`), extracted under `build/<preset>/_deps/`.
  Nothing is vendored into the repository.
- The static loader (`WebView2LoaderStatic.lib`) is linked into the app, so
  **`WebView2Loader.dll` is not deployed**.
- At runtime the machine needs the Microsoft **Evergreen WebView2 Runtime**
  installed. No fixed-version runtime is bundled.
- WebView runtime profile data is written under
  `%LOCALAPPDATA%\Audient Console\WebView2`.
- Frontend assets live in `src/daily_gui/ui/` and are loaded over `file:///`;
  there is no CDN, remote JS/CSS, remote font, or `fetch` at runtime.

### Frontend bundle

```powershell
cd src\daily_gui\ui
npm ci
npm run typecheck
npm run build
```

The production bundle (`app.js`, `app.css`) is staged beside the built
executable.

## Installer

The Windows installer is built with Inno Setup 6:

```powershell
ISCC.exe packaging\AudientConsole.iss
```

It reads the Release build from `build\configure-release\src\app\Release` and
writes `dist\AudientConsole-<version>-Setup.exe`. See
`packaging/AudientConsole.iss` for the per-user install layout and the
WebView2 / VC++ runtime strategy.

## Benchmarks

The benchmark harness is a standalone executable, not part of the CTest suite:

```powershell
cmake --build --preset build-debug --target audient_console_bench
.\build\configure-debug\tests\bench\Debug\audient_console_bench.exe all
```

## Formatting and lint

If `clang-format` / `clang-tidy` are installed they are picked up
automatically; otherwise the targets print an informative message and do not
fail the build.

```powershell
cmake --build --preset build-debug --target format
cmake --build --preset build-debug --target format-check
cmake --build --preset build-debug --target lint
```

## Generated artifacts

- `build/<preset>/generated/include/audient_console_version.h` — version header
  (`VERSION` file + git short SHA + build type + build time).
- `build/<preset>/NOTICE.txt` — aggregated third-party notices.
- `build/<preset>/symbols/<config>/` — archived PDB files.
- `build/<preset>/compile_commands.json` — when
  `CMAKE_EXPORT_COMPILE_COMMANDS` is on.

Nothing under `build/`, `symbols/`, `dist/`, or generated directories is
committed.
