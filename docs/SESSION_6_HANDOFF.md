# Session 6 handoff — S22 Quick bridge and runtime admission evidence

**Status:** implementation complete locally; hosted, package, and product GUI gates remain open
**Branch:** `gh-247`
**Base:** `21c251b1cef26080051afeaa4a82b08e7f3f9fb8`
**Handoff date:** 2026-08-20

## Objective

Execute the next admitted S22 surface after the S21 canvas decision: make the
QWidget/Quick focus and accessibility boundary executable, and make the
qualification-only Qt runtime/package boundary fail closed. Do not begin
product Quick root or PDF canvas migration.

## Delivered

- Extended the synthetic `CanvasBenchmark` QML surface with an invisible,
  semantic Quick control and a `--focus-bridge` probe.
- Added bidirectional keyboard focus checks across QWidget → Quick → QWidget,
  including reverse traversal, plus Quick name/description/role assertions.
- Added `scripts/run-quick-focus-bridge.ps1` for native and
  `QT_QUICK_BACKEND=software` qualification runs.
- Added `docs/quick-runtime-manifest.json` and
  `scripts/verify-quick-runtime-contract.py`. Both optional Quick targets are
  asserted to remain non-installed qualification artifacts; Qt LGPL/relink,
  SBOM, notices, and clean-machine package gates remain explicit.
- Wired the runtime/package contract verifier into the reusable Windows and
  Linux CI paths.

## Fresh-eyes refutation

**Assumption tested:** a successful Quick process start is enough to establish
the mixed-mode bridge.

**Method:** the new probe sends Tab and Shift+Tab through a real QWidget host
containing `QQuickWidget`, then checks both external QWidget focus targets and
the active Quick control. It separately checks the QML accessibility contract
and records native accessibility-backend activation.

**Verdict:** **REFUTED.** The process-start assumption does not establish the
bridge; the explicit probe was required. The probe passes on this Windows host,
but proves only the qualification boundary and synthetic control semantics. It
does not prove the product shell, PDF canvas, screen-reader runtime, or
packaged Qt behavior.

## Evidence ledger

| Evidence | Result | Notes |
| --- | --- | --- |
| Quick runtime contract verifier | PASS | Static manifest and staged `build/usr/bin` scan |
| Quick shell policy verifier | PASS | Worktree-safe scan; `qml_files=2` |
| CanvasBenchmark Release build | PASS | Existing opt-in build; CMake auto-regenerated once because its dependency stamp was stale |
| `check-change.py --base origin/dev --build-dir build` | NOT PROVEN | Static policy/catalog checks pass, but inherited `LoupeLibCore`/`PdfTool` builds fail in `LoupeLibCore/sources/pdfpagemasterexport.cpp`; test targets are absent with tests disabled, and clang-format/clang-tidy are unavailable |
| QWidget/Quick focus bridge, native | PASS | Windows host; D3D11; all four focus directions and accessibility contract |
| QWidget/Quick focus bridge, software | PASS | Windows host; `QT_QUICK_BACKEND=software`; all four focus directions and accessibility contract |
| S21 benchmark regression, native | PASS | All four candidates; D3D11 Quick paths; DPI 1.5 |
| S21 benchmark regression, software | PASS | All four candidates; software Quick paths; DPI 1.5 |
| `UnitTestsAccessibility` | NOT RUN | Existing build cache has `LOUPE_BUILD_TESTS=OFF`; no reconfigure performed |
| Windows/Linux hosted smoke | OPEN | Requires hosted CI evidence on a PR/stable run |
| Final-artifact SBOM/notices/LGPL relink | OPEN | Qualification targets are not installed |
| Product Quick accessibility | NOT STARTED | Blocked by the remaining G4/package gates |

## Commands

```powershell
python scripts/verify-quick-runtime-contract.py
python scripts/verify-quick-shell-policy.py
pwsh -NoProfile -File scripts/run-quick-focus-bridge.ps1 -BuildDir .\build -Mode native
pwsh -NoProfile -File scripts/run-quick-focus-bridge.ps1 -BuildDir .\build -Mode software
```

The bridge script is intentionally fail-closed. `QT_QPA_PLATFORM=offscreen`
is a headless platform choice, not native accessibility or renderer evidence.

## Remaining gates

Session 6 does not make a 0.1.2 release-ready claim. The next authority-gated
work is hosted Windows/Linux native/software evidence, final-artifact Qt
module/SBOM/notices and LGPL replacement/relink proof, clean-machine package
smoke, and product-level Quick accessibility once those gates are admitted.
