# ADR-009 — WebView2 as the Daily GUI presentation layer (acquisition, static loader, RT boundary)

- Status: Accepted (2026-09-12, Daily GUI WebView2 baseline)
- Scope: the Daily GUI presentation layer only. The proven C2 audio/RT engine
  (ASIO, RoutingCore, VST chains, transport, hardware control) is unchanged.
- Related: `project guidelines` §4, §7, §9, §15, §16, §17, §20, §21; `project plan` Phase 6 worklog
  (Daily GUI WebView2 baseline); `designGUI/audient-console-webview-tailwind-project plan`;
  `docs/build.md`; `docs/dependency-inventory.json`.

## Context

The native Win32 `DailyGui` is functionally useful but was rejected on visual grounds.
The user directed a pivot of the Daily GUI presentation layer to a native C++ host plus
Microsoft WebView2, HTML, TypeScript, and Tailwind CSS. Constraints: no Electron, fully
offline at runtime, no persistence yet, existing `DailyGui` preserved as fallback/reference,
and — most importantly — no WebView/JS/JSON/render/filesystem work may enter the realtime
audio callback.

The Daily GUI control model already exists as `daily_gui::DailyGuiConfig` (a set of
`std::function` getters/setters) populated by `daily_demo_main.cpp` and invoked on the UI
thread. The migration must reuse that seam rather than invent a second control architecture.

## Decision

1. **Host.** A native C++ host (`DailyWebViewHost` + `DailyWebViewController`) creates the
   WebView2 environment and controller on the existing UI/STA thread. A timer at **33 ms
   (~30 Hz)** pulls the `DailyGuiConfig` getters into a bounded JSON snapshot and pushes it to
   the page; `chrome.webview.postMessage` commands are parsed and range/channel-validated
   before dispatching to the existing setters/callbacks. The ASIO callback is not touched.

2. **SDK acquisition.** `Microsoft.Web.WebView2` is pinned by exact version **1.0.4191.47** and
   integrity **SHA512** of the official NuGet flat-container nupkg, acquired in
   `cmake/CppDependencies.cmake:audient_add_webview2sdk()` via `FetchContent_Declare(... URL ...
   URL_HASH SHA512=...)`. It is extracted into the build tree (`_deps/webview2sdk-src`); nothing
   is vendored into the repository, and no locally installed SDK, and nothing under `iD/`, is
   used. This matches the project's existing FetchContent pinning practice and keeps the
   `FETCHCONTENT_FULLY_DISCONNECTED` build probe meaningful.

3. **Static loader.** The SDK's `WebView2LoaderStatic.lib` is linked into
   `audient_console_daily.exe`. Consequently `WebView2Loader.dll` is **not deployed** — it is
   not copied beside the executable, not installed, and not an import dependency. Runtime uses
   the separately installed **Evergreen** WebView2 Runtime.

4. **Offline assets.** The frontend is plain local `index.html` / CSS / JS staged beside the
   executable and loaded over `file:///`. No CDN, remote JS/CSS, remote fonts, remote icon
   libraries, or `fetch`; no locally running dev server is required at runtime.

5. **User data.** The WebView runtime profile is written to
   `%LOCALAPPDATA%\Audient Console\WebView2`, a deterministic per-user writable location,
   because the production install path (`C:\Program Files\...`) is not writable runtime state
   storage. This directory is WebView runtime state only and is not the app's session schema.

6. **VST editors.** Native VST3 editor windows remain plugin-controlled and are never embedded
   in the WebView surface.

## Consequences

- The Daily GUI can be rebuilt visually with standard web tooling (TypeScript/Tailwind) without
  re-validating the audio engine; control semantics come from the unchanged `DailyGuiConfig`
  seam.
- A machine that lacks the Evergreen WebView2 Runtime cannot render the Daily GUI; the product
  must detect/report this (a later branding/recovery slice). Build machines need network on the
  first configure to fetch the pinned nupkg.
- The static loader removes a deployment artifact and keeps `Program Files` write-free, at the
  cost of re-running the pinned download when the SDK version changes.
- The 33 ms snapshot cadence is presentation-only; meter ballistics remain a frontend concern,
  so no audio-thread timing or ballistics are affected.

## Alternatives considered

- **Full system VST3/Chromium style runtime (Electron/CEF):** rejected — violates the native,
  low-overhead product constraints.
- **Vendoring the SDK into the repo:** rejected — binary blobs in git, manual upgrade, and
  against the "no locally-copied SDK" hygiene requirement.
- **NuGet/Visual Studio package restore:** rejected — invisible to `cmake --preset`, requires
  VS/NuGet integration, and duplicates paths CMake already manages.
- **Dynamic loader (`WebView2Loader.dll`):** rejected — adds a deploy step and a writable-path
  concern; the static loader is sufficient for Evergreen.
