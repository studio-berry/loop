# Session 5 handoff — S21 canvas hosting benchmark

**Status:** complete locally; hosted and external release gates remain open
**Branch:** `gh-247`
**Base:** `c58e2679aba02e8e8c13694dac0a57440e80a67b`
**Implementation commit:** `6964a2a3b44b3642eea68212635420bbe6bcda51`
**Handoff date:** 2026-08-20

## Objective

Complete the S21 canvas-hosting benchmark opened by Session 4 and record the
ADR-009 outcome before any product Quick canvas or root migration work.

## Delivered

- Added the opt-in `CanvasBenchmark` Qt 6.11.1 target and its synthetic QML
  qualification surface.
- Compared the Widgets baseline, `QQuickWidget`, WindowContainer, and direct
  `QQuickItem` candidates for resize, key delivery, focus, color, DPI, and
  graphics API behavior.
- Added `scripts/run-canvas-benchmark.ps1`, which fails closed on a non-pass
  candidate result.
- Recorded ADR-009 and updated ADR-010 to retain the Widgets/hybrid production
  canvas while leaving Quick PDF-canvas migration conditional.
- Added the required `changes/gh-247.md` fragment.

## Decision

The existing `PDFDrawWidget` and QWidget/hybrid hosting model remain the
production PDF canvas. The synthetic probe is a **GO** for retaining that
baseline, a **CONDITIONAL** future-hosting result for WindowContainer and
direct `QQuickItem` on non-PDF surfaces, and a **NO-GO** for replacing the PDF
canvas with any Quick candidate without separate fidelity and
color-management evidence.

No product QML, product root migration, `QQuickPaintedItem` bridge, or PDF
renderer change was made.

## Evidence ledger

| Evidence | Result | Notes |
| --- | --- | --- |
| Dedicated CMake configure | PASS | Qt 6.11.1; `LOUPE_BUILD_CANVAS_BENCHMARK=ON`; generated artifacts retained under ignored `build/` |
| `CanvasBenchmark` Release build | PASS | MSVC 19.44; local duplicate `Path`/`PATH` environment normalized for MSBuild |
| Native Windows benchmark | PASS | All four candidates; `QT_QPA_PLATFORM=windows`; D3D11 for Quick candidates; DPI 1.5 |
| Windows Qt Quick software benchmark | PASS | All four candidates; `QT_QUICK_BACKEND=software`; Quick candidates reported `software`; DPI 1.5 |
| Windows offscreen probe | PARTIAL | Widgets, QQuickWidget, and direct QQuickItem passed; WindowContainer focus was unavailable under offscreen |
| Architecture catalog check | PASS | Bundled Python `scripts/generate-architecture-catalogs.py --check` |
| Cached diff check | PASS | `git diff --cached --check` before implementation commit |
| Full `check-change.py` | PENDING | Run against `origin/dev` after the handoff commit; toolchain availability may leave the result incomplete |
| Hosted Linux benchmark | NOT RUN | Requires a hosted display/backend matrix; offscreen is not equivalent evidence |

Native Windows resize samples were 22 ms / 193 ms / 79 ms / 74 ms for the
Widgets baseline / QQuickWidget / WindowContainer / QQuickItem candidates.
Software samples were 18 ms / 35 ms / 32 ms / 26 ms. These are diagnostic
measurements, not release thresholds.

## Onboarding for the next agent

1. Start from `gh-247` at the pushed tip and read this handoff, ADR-009, and
   ADR-010 before touching product Quick code.
2. Treat the benchmark JSON as synthetic host-mechanics evidence only. Do not
   use it to claim PDF geometry, ICC/blend, accessibility, packaging, or
   Windows/Linux parity.
3. Re-run the native and software commands with the Qt runtime on the host:

   ```powershell
   $qt = 'C:\.dev\repos\frisket\qt\6.11.1\msvc2022_64'
   $env:PATH = "$qt\bin;build\vcpkg_installed\x64-windows\bin;$env:PATH"
   $env:QT_PLUGIN_PATH = "$qt\plugins"
   $env:QT_QPA_PLATFORM = 'windows'
   pwsh -NoProfile -File scripts/run-canvas-benchmark.ps1 -BuildDir .\build -Candidate all
   $env:QT_QUICK_BACKEND = 'software'
   pwsh -NoProfile -File scripts/run-canvas-benchmark.ps1 -BuildDir .\build -Candidate all
   ```

4. Before product Quick work, close hosted Windows/Linux native/software
   smoke evidence, final-package/SBOM/notices/LGPL gates, and the executable
   QWidget-to-Quick-to-QWidget focus/accessibility bridge.

## Landmines and fresh-eyes refutation

- A passing `QuickShellSmoke` result does not establish a canvas-hosting
  decision; S21 measures four candidates separately.
- A normal QWidget is not a QQuickItem. Do not cast, wrap, or silently treat
  `PDFDrawWidget` as a scene-graph item.
- `QT_QPA_PLATFORM=offscreen` selects a headless platform. It is not renderer
  proof and it exposed the WindowContainer focus limitation in this probe.
- `QT_QUICK_BACKEND=software` is the Qt Quick software-backend contract;
  `QSG_RHI_BACKEND` selects an RHI and is a different axis.
- The local MSBuild environment contains duplicate case variants of PATH.
  Normalize the child environment if the same `MSB6001` error recurs.
- The global Git ignore warning is inaccessible local configuration and is not
  repository content.

## Remaining gates

Session 5 is locally complete and pushed, but this is not a 0.1.2 release-ready
claim. The remaining gates are hosted Linux/Windows evidence, package/SBOM/
licensing proof, product accessibility and focus-bridge runtime proof, and
later dedicated PDF-canvas fidelity/color-management evidence.
