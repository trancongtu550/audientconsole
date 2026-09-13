# NOTICE — third-party software in this directory

This directory contains an adapted, capture-only kernel driver derived from:

  Microsoft Simple Audio Sample Device Driver
  Copyright (c) 2015 Microsoft Corporation
  https://github.com/microsoft/Windows-driver-samples
  Folder: `audio/simpleaudiosample`
  Pinned commit: `197ba2156a60e2b76fcd4820bae594223e91a1e9` (2026-09-04)

License: Microsoft Public License (MS-PL). A complete copy of the MS-PL text is kept
as `LICENSE-MSPL.txt` in this directory. Per MS-PL §3.C/D, the Microsoft copyright
and attribution notices are retained in every adapted source file, and redistribution
of these files in source form is only under the MS-PL.

What changed in Q5-A2 (capture-only shell):
- Removed the speaker/render endpoint, tone generation, and file-save (savedata)
  functionality from the sample.
- Kept the port-class/WaveRT capture (mic-array) shell.
- The capture stream writes digital silence until the transport-backed fresh source
  is wired in a later Phase 8 slice.
- Identity target: `Microphone (Audient Console)`. Format/identity finalization
  (48 kHz mono float32) is a later slice.

No other third-party code is used in this directory.