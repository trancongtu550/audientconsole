# driver/ — Audient Console virtual microphone kernel endpoint (Phase 8)

This directory is the home of the capture-only WaveRT kernel endpoint derived from
the **Microsoft Simple Audio Sample Device Driver** (`microsoft/Windows-driver-samples`
`audio/simpleaudiosample`, pinned commit `197ba2156a60e2b76fcd4820bae594223e91a1e9`).

A3B slice status (2026-09-04): a capture-only WaveRT shell with a **shared
capture-ring consumer seam**, a **control plane that publishes a real pinned
shared section**, and a **float32→PCM32 boundary conversion** now live here:

- speaker/render endpoint, tone generation, and file-save (`savedata`) code are
  REMOVED (deterministic evidence: `tools/verify-capture-shell-strip.ps1`, exit 0);
- the mic-array capture descriptor advertises **48 kHz, 2 ch, 32-bit PCM**
  (A3B boundary format). The shared ring carries MONO float32; the DMA fill
  converts each sample to a clamped signed PCM32 L=R pair
  (`CaptureRingFloatToPcm32` / `CaptureRingConsumerFillPcm32Stereo`);
- a control plane (`audientcontrolplane.cpp` + `driver-protocol`
  `CaptureControlPlane.h` / `CaptureControlClient`) publishes a user-mode mapped
  shared section (MDL probe+lock) as the capture ring and serves
  CONNECT/DISCONNECT/FLUSH/QUERY_STATE IOCTLs (single producer). Verified
  end-to-end on the test VM with a WASAPI shared-mode PCM32 capture client
  (known-tone RMS/correlation, silence before/after connect, exact-silence
  underrun, no-stale-replay, generation reset, repeated connect cycles);
- native IEEE_FLOAT-only endpoint presentation is a DEFERRED investigation (a
  float-only capture pin was judged NOT_PRESENT on the test Windows build; A3B
  uses the proven-present PCM format). Q5-A4 (user scope correction) KEEPS the
  proven 2ch 32-bit PCM endpoint and the mono float32 internal transport; only
  the endpoint IDENTITY was changed (plain `KSNODETYPE_MICROPHONE` node +
  canonical friendly name `Microphone (Audient Console)` + device description
  `Audient Console`).

No kernel source is built or installed on the daily PC (AGENTS §18); all driver
development/test-signing happens on the disposable test VM.

## Q5-A3B capture source (this tree)

```
Control client (app) / probe			 [driver-protocol CaptureControlClient]
   ↓ CONNECT(userBase, userBytes)   METHOD_BUFFERED IOCTL, PASSIVE_LEVEL
audientcontrolplane.cpp  (control DO, stable interface GUID; single producer)
   ↓ IoAllocateMdl + MmProbeAndLockPages → system VA, region descriptor
CaptureRingContract.h region  (versioned ABI, mono float32)     [driver-protocol]
   ↓
audientcapturesource.cpp  refcounted descriptor manager + DMA fill
   ↓ CaptureRingConsumerFillPcm32Stereo (fresh/stale/underrun, exact silence)
WaveRT capture DMA buffer (mono float32 → clamped PCM32 L=R, 2ch)
```

The kernel does NOT depend on the user-mode `IDriverCaptureSink`; it reads the
plain memory contract. `CaptureRingContract.h` and `CaptureControlPlane.h` are
compiled into this driver build via `Main.vcxproj`'s include path (they are not
copied here). The control plane and the DMA fill never run on the realtime
callback; the app worker (`VirtualMicFeeder → SharedRingCaptureSink`) remains the
producer boundary.

## Selected base sample (user decision: "prioritize the minimal sample for a capture endpoint")

- Repo: `microsoft/Windows-driver-samples`, branch `main`
  https://github.com/microsoft/Windows-driver-samples
- Folder: `audio/simpleaudiosample` — the **Microsoft Simple Audio Sample Device
  Driver**: "a simple WDM audio driver that exposes support for two basic audio
  devices (speaker and microphone array)" using **WaveRT** and a **virtual audio
  device instead of actual hardware**. The mic-array capture endpoint is the minimal
  official capture base, smaller than `audio/sysvad` and deliberately not the ACX
  framework (port-class/WaveRT map directly onto the existing port-class knowledge
  and the interim two-cable Phase 7 path).
- License: **MS-PL** (`LICENSE` at sample repo root, © 2015 Microsoft). The adapted
  sources here retain the Microsoft copyright headers; a complete MS-PL license text
  is kept as `LICENSE-MSPL.txt` in this directory (MS-PL §3.C/D requirement for
  distributing derived source). See `NOTICE.md`.

### Pinned base content (blob SHAs, fetched 2026-09-04; the A2-descended files are listed below)

| Sample file (pinned) | Blob SHA | Role → A2 capture shell in this directory |
|---|---|---|
| `Source/Main/minwavertstream.cpp` | `c189c9390838013b8b40183ecf874f5dacda82ae` | WaveRT stream; capture path now writes digital silence |
| `Source/Main/minwavert.cpp` | `1d92073d119b07aa4fab1907f1826294f9b75f20` | WaveRT miniport (capture-only branches) |
| `Source/Main/minwavertstream.h` | `0124cab2d387349dabe8add64701fcbd86d5abdb` | Stream class header (capture-only) |
| `Source/Main/adapter.cpp` | `a66ee5a29a74f39b8cfa38d920135c6f92f97d21` | Adapter/port driver registration (capture filters only) |
| `Source/Filters/micarraytopo.cpp` | `ebb9784d72f70e7c639e9ce232f8c7f89e3d2904` | Mic-array capture topology |
| `Source/Filters/micarraywavtable.h` | `536560382b3541486448b18b4ae49ac91fd4eb3e` | Mic-array wave filter table (pin/data-format descriptors) |
| `Source/Filters/micarray1toptable.h` | `3bbd80f34e1fdc8abae744902f62ab0e2af218e9` | Mic-array topology table |
| `Source/Main/SimpleAudioSample.inx` | `db2b62c1e0c161b9562c838c14f74e58543d6c2d` | INF template → adapted to capture-only `AudientConsoleMic.inx` |
| `README.md` | `f7d41b38f7f34fa3916e566b83296cc4f1b6ff5c` | Build/deploy/test instructions |

Files removed in A2 (render/tone/savedata, absent from this tree and verified by
`tools/verify-capture-shell-strip.ps1`):

- `Source/Filters/speaktopo.cpp/.h`, `speakertoptable.h`, `speakerwavtable.h`
- `Source/Inc/mintopo.h`, `Source/Main/mintopo.cpp`
- `Source/Utilities/savedata.cpp/.h`, `Source/Utilities/ToneGenerator.cpp/.h`

## Adapted layout (Q5-A2, capture-only shell)

```
driver/
  AudientConsoleMic.sln
  LICENSE-MSPL.txt
  NOTICE.md
  Package/package.VcxProj
  Source/
    Main/    adapter.cpp, basetopo.cpp, common.cpp, minwavert.cpp/.h,
             minwavertstream.cpp/.h, NewDelete.cpp, Main.vcxproj,
             AudientConsoleMic.inx, AudientConsoleMic.rc
    Filters/ micarraytopo.cpp/.h, micarray1toptable.h, micarraywavtable.h,
             minipairs.h, Filters.vcxproj
    Inc/     basetopo.h, common.h, definitions.h, endpoints.h, Inc.vcxproj,
             kshelper.h, NewDelete.h
    Utilities/ hw.cpp/.h, kshelper.cpp, Utilities.vcxproj
```

Electrical identity (Q5-A4): one virtual capture endpoint presented as a plain
microphone node (`KSNODETYPE_MICROPHONE`, NOT the array) whose Windows friendly
name is the canonical `Microphone (Audient Console)` (device description
`Audient Console`; stable ROOT\AudientConsoleMic hardware identity across
reinstall/reboot). Wire format stays 48 kHz, 2 ch, 32-bit PCM (A3B boundary
decision - mono float32 is internal-only and converted at the kernel DMA fill
via `CaptureRingFloatToPcm32` L=R). Capture DMA is fed from the shared
capture-ring transport when a ring is published (control plane) and serves
exact digital silence otherwise (no app / disconnected / crashed).

## Adaptation points (Phase 8 driver slices)

1. **DONE (Q5-A2):** keep port-class/WaveRT capture shell; strip speaker/render,
   tone generator, and savedata. Verified by `tools/verify-capture-shell-strip.ps1`.
2. **DONE (Q5-A3):** add the shared capture-ring consumer seam. The mic-array
   descriptor advertises 48 kHz / 2 ch / 32-bit IEEE float; `audientcapturesource.cpp`
   consumes the versioned `CaptureRingContract.h` region (mono float32 → L/R float32
   DMA) behind a region provider that returns NULL in A3, so the DMA path keeps
   serving exact digital silence. The same ring functions are proven on the host by
   `tests/unit/CaptureRingContractTest.cpp` and the parametrized
   `VirtualMicFeederProtocolTest` (mock + `SharedRingCaptureSink`).
   Transport bridges still candidates (measured/decided in Phase 7–8):
   - user-mode service thread pulls `transport::VirtualCaptureTransport` via
     `VirtualCaptureEndpoint::captureMono` (the Q2 contract) and pushes into the
     driver through a bounded shared-memory ring; or
   - driver-side capture pull reads a mapped user-mode ring written by the engine
     transport bridge.
   Both keep `src/routing|vst3|transport` free of driver code; the Q2 contract is the
   shared seam.
3. **DONE (Q5-A3B):** control plane publishes a real region. A user-mode client
   maps a shared section and CONNECTs it through the stable control-interface
   GUID; the driver locks the pages (MDL) and the DMA fill reads the ring and
   converts mono float32 → clamped 2ch PCM32 (endpoint descriptor is now
   2ch 32-bit PCM). Verified end-to-end on the VM (known readPattern 1000 Hz
   reaches the WASAPI capture client; silence before/after connect; exact-silence
   underrun; no-stale replay on reconnect; generation reset; repeated cycles).
4. **DONE (Q5-A4):** endpoint identity finalized. The topology pin category is a
   plain `KSNODETYPE_MICROPHONE` (the mic-array node type and the array-only
   `KSPROPERTY_AUDIO_MIC_ARRAY_GEOMETRY` advertisement are removed) and the
   INF strings are finalized so Windows presents exactly
   `Microphone (Audient Console)` (`AUDIENTCONSOLEMIC_SA.DeviceDesc = "Audient
   Console"`, `DriverVer 09/05/2026 1.0.0.2`, hardware ID `ROOT\AudientConsoleMic`
   unchanged). The wire format is intentionally UNCHANGED: 48 kHz / 2 ch /
   32-bit PCM endpoint, mono float32 internal, conversion at the kernel boundary
   (user decision - no native mono/IEEE_FLOAT endpoint). The control plane now
   clears `REGION_FLAG_CONNECTED` on DISCONNECT/handle-close/device-STOP so the
   app producer observes `WRITE_DISCONNECTED` (counted) instead of filling an
   orphaned region, and the app bridge aligns the feeder epoch with the driver
   CONNECT generation (no kernel-epoch clobber). Reinstall/reboot/reconnect and
   Windows Sound/Discord/WASAPI visibility verified on the test VM.
   (The mono float32 NATIVE endpoint presentation stays deferred per the A3B
   finding and the user's A4 scope correction.)
5. Recapture endpoint rules from the Q2 contract: digital silence on absent app,
   no stale/uninitialized replay, bounded/drop accounted on the producer side,
   no client-blocking.

## WDK / VM build-and-test gate (AGENTS §11/§18)

- Kernel driver source is developed/test-signed ONLY in a disposable VM or a
  dedicated test Windows installation — never installed on this daily PC.
- Toolchain pin (Slice Q4, 2026-09-04): **WDK 10.0.26100.6584** + Windows SDK
  **10.0.26100.x** + Visual Studio 2022 **17.10+** (project uses 17.14.37614.0 /
  MSVC 14.44.35207). The WDK and SDK **build numbers must match** (26100). The
  28000-series WDK belongs to Visual Studio 2026 and is NOT selected here. Full
  component checklist + test-signing workflow: `docs/driver-test-environment.md`.
- Build requires a WDK machine (no WDK on this daily machine as of 2026-09-04:
  Windows SDK 10.0.26100.0 present, km libs/toolsets absent). The read-only verifier
  `tools/verify-driver-test-environment.ps1` checks every prerequisite on the test VM
  before any Phase-8 build.
- Build command shape (VM, from this driver directory, after sync):
  `MSBuild.exe AudientConsoleMic.sln /m /p:Configuration=Release /p:Platform=x64 /t:Rebuild`
- Capture-shell evidence (host, no build needed):
  `powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify-capture-shell-strip.ps1`
- Sequence once available: run the env verifier (must exit 0) -> build sample ->
  adapt -> test-sign -> Driver Verifier + WDK audio/HLK subset -> install in VM
  -> verify capture with a recorder -> only then prepare the production Dev
  Portal signing path (Secure Boot stays ON).
- Production driver (Phase 8/11) must carry the Microsoft/Dev-Portal signature;
  unsigned/test-signed builds are never a production substitute.

## Gates that must pass before the kernel production integration

- Phase 0 signing feasibility record (open in project plan §1).
- Phase 7 transport contract gates (driver-agnostic user-space transports
  already exist: Q1 capture transport + P downlink transport).
- Q2 endpoint contract proven by the software endpoint + integration tests.