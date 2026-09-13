# Contributing

Thanks for your interest in Audient Console. This is an unofficial community
project and is not affiliated with Audient Ltd.

## Ground rules

- Keep each change to the smallest coherent slice; do not reformat unrelated
  files.
- Preserve existing behavior in the realtime audio path. The ASIO callback must
  never allocate, block, log, or touch the UI.
- Never weaken a test or threshold to make code pass. If a requirement changes,
  record it in an ADR under `docs/adr/`.
- Do not edit third-party sources; prefer wrappers or patch files.
- Do not commit generated binaries, symbols, certificates, machine-local paths,
  captured USB/audio data, or personal data.

## Build and test

Windows 11 x64, Visual Studio 2022 C++ toolchain:

```powershell
cmake --preset configure-debug
cmake --build --preset build-debug
ctest --preset test-debug
```

See `docs/build.md` for prerequisites, the ASIO SDK, the VST3/WebView2
dependencies, and the installer build.

## Safety boundaries

- `Release hardware control` and coexistence with the official iD Mixer are
  safety boundaries; never fake an unsupported control in the UI.
- Do not enable Windows Test Mode, disable Secure Boot, or install test-signed
  kernel drivers on a daily-use machine.

## Pull requests

Describe the change, which tests you ran, and any hardware you validated it on
(interface model, driver/firmware, sample rate, buffer size).
