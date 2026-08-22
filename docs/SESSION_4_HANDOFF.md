# Session 4 handoff — S22 Quick-root admission

**Status:** complete locally; external release gates remain open
**Branch:** `0.1.2` (legacy branch name; orchestration milestone **0.2.0**)
**Implementation commit:** `f857cf25901c6931b8e426ac13c57ea81e3efb6f`
**Base:** `6bb1df833c1621b8b9bc32793c7c4449f70c69a2`
**Handoff date:** 2026-08-20

## Delivered

- Recorded the Quick-root directive, `QWidget`/`QQuickItem` boundary, and
  measured-only WindowContainer candidate in ADR-010.
- Added the Quick shell threat model, static QML boundary policy, design-token
  contract, accessibility contract, and policy test.
- Added the optional `QuickShellSmoke` target and runner. The harness uses the
  shell imports, observes `scene_graph_initialized`, and records the selected
  `GraphicsApi`.
- Wired Windows and Linux CI to build the qualification target and run native
  and `QT_QUICK_BACKEND=software` smoke modes.
- Reconciled the shell JSON/schema and supporting Quick, packaging, and
  accessibility documentation after the public 0.1.1 G0 release.

This commit does not migrate the product root, PDF canvas, dialogs, plugins,
or accessibility bridge to product QML. The QML file in this change is a
qualification harness only.

## Evidence ledger

| Evidence | Result | Notes |
| --- | --- | --- |
| `QuickShellSmoke` Release build | PASS | Existing Windows Qt 6.11.1 build; target compiled successfully |
| Native backend smoke | PASS | D3D11; `scene_graph_initialized` observed |
| Qt Quick software smoke | PASS | `QT_QUICK_BACKEND=software`; software API observed |
| WARP preference smoke | PASS | `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`; D3D11 API observed |
| Quick policy and token verifier | PASS | Imports, boundaries, contrast, focus, motion, and accessibility contract |
| Quick policy unittest | PASS | 1 test |
| Shell contract verifier | PASS | 7 workspaces, 107 Editor actions, 11 plugin policies |
| Architecture catalog check | PASS | ADR metadata and generated catalog remain valid |
| Focused CTest suite | PASS | `UnitTests`, `UnitTestsOcrContract`, `UnitTestsPdfToolContract`, `UnitTestsPreflightCorpus` |
| Source integrity and policy adapters | PASS | `check-change.py` evidence |
| Full `check-change.py` status | INCOMPLETE | `clang-format` and `clang-tidy-18` unavailable; broad MSBuild builds hit local duplicate `PATH`/`Path` environment failure |

## Landmines and constraints

- Use the bundled Codex Python runtime when the default Windows Python launcher
  fails to create a process.
- `QT_QPA_PLATFORM=offscreen` is headless platform selection, not renderer
  proof. Backend evidence requires the harness scene-graph line and a known
  `GraphicsApi`.
- `QT_QUICK_BACKEND=software` is the Qt Quick software-backend contract.
  `QSG_RHI_BACKEND` selects an RHI; WARP preference is
  `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`.
- Do not wrap `PDFDrawWidget` in `QQuickPaintedItem` or treat a `QWidget` as a
  `QQuickItem`. The S21 canvas benchmark must admit a dedicated canvas
  candidate before product Quick migration.
- An inaccessible global Git ignore file may emit warnings; it is unrelated to
  repository content.

## Open gates for the next session

1. Complete the S21 canvas benchmark and record the ADR-009 outcome.
2. Collect successful Windows and Linux GitHub CI native/software smoke runs
   from the pushed `0.1.2` branch (milestone **0.2.0**).
3. Inventory the final linked Qt/QML runtime, generate SBOM/notices, and prove
   clean-machine Windows/Linux package behavior and the Qt LGPL route.
4. Implement and test the product Quick accessibility and
   QWidget-to-Quick-to-QWidget focus bridge.
5. Only after those gates pass, begin the product Quick canvas/root sessions.

## Fresh-eyes closeout

Session 4 is locally complete and pushed, but it is not a 0.2.0 release-ready
claim. The remaining gates above are intentional NO-GO conditions, not implied
by the passing qualification harness or focused tests.
