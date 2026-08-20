# Interaction trace (Session 1)

Session 1 adds an opt-in, privacy-safe recorder to the existing QWidget canvas.
It is enabled by the renderer's `PDFRenderer::DisplayTimes` feature flag; normal
builds and normal interaction do not emit a trace. The recorder is private to
`LoupeLibWidgets` and reports aggregate timing only.

## Output contract

When tracing is enabled and the recorder is stopped, Loupe logs one compact JSON
line prefixed with `LOUPE_INTERACTION_TRACE_V1`. The payload contains:

- input-to-frame acknowledgement latency and bounded input/frame identifiers;
- frame-time percentiles, FPS, refresh-rate-derived budgets, and late-frame count;
- exclusive stage timing for interaction, hit testing, page rendering, overlays,
  composition, and unknown/external work;
- cache hit/miss totals, visible-page count, document-revision ordinal, and
  pending asynchronous queue depth;
- explicit `available`/`unavailable` markers and an `evidence_state` value.

No document text, pixels, paths, event payloads, or user identifiers are stored
or serialized. Sampling is bounded (default 2,048 samples) and can be adjusted
for a local diagnostic run with:

```text
LOUPE_INTERACTION_TRACE_MAX_SAMPLES=2048
LOUPE_INTERACTION_TRACE_SAMPLE_EVERY=1
LOUPE_INTERACTION_TRACE_REFRESH_HZ=0
```

The refresh-rate value is optional. If it is unknown, the summary keeps the
60 Hz and 120 Hz reference budgets but marks the actual budget unavailable.
Percentiles use nearest-rank over the bounded sample reservoir.

## Local Windows test binaries

The generated test executables must have their Qt and Loupe/vcpkg runtime DLLs beside
them. Running a test from a build tree without that dependency set can appear
to hang while Windows reports a missing-DLL system error. After a Release build,
deploy the runtime into the executable directory before running tests:

```powershell
& "$env:QT_ROOT\bin\windeployqt.exe" --release --no-translations `
  --dir build-gh-140/bin/Release build-gh-140/bin/Release/UnitTestsInteractionTrace.exe
& "$env:QT_ROOT\bin\windeployqt.exe" --release --no-translations `
  --dir build-gh-140/bin/Release build-gh-140/bin/Release/UnitTestsJobScheduler.exe
Copy-Item vcpkg_installed/x64-windows/bin/*.dll build-gh-140/bin/Release/ -Force
Copy-Item "$env:QT_ROOT/plugins/platforms/qoffscreen.dll" `
  build-gh-140/bin/Release/platforms/ -Force
```

For widget-linked tests, run the same deployment against
`LoupeLibWidgets.dll` as well so Qt Widgets/Concurrent and their plugins are
present. Include `qoffscreen.dll` when using `QT_QPA_PLATFORM=offscreen`;
`windeployqt` normally selects the desktop platform plugin only. Keep this
generated runtime output untracked; the source dependency of truth remains
CMake, vcpkg, and the configured Qt installation.

Session 2 adds `UnitTestsInteractionState.exe` and
`UnitTestsInteractionWidget.exe`; deploy both with the same complete dependency
set before running the focused interaction suite. In particular, a local test
directory without the vcpkg/Loupe DLLs can report `gt6gui.dll` or `lcm2-2.dll`
as missing and may look hung while Windows displays the system error. The
dependency set is part of every local Windows test run, not an optional cleanup
step.
