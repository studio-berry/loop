# Phase 5 Widgets deletion handoff

**Status:** prepared by P4-S12 product cutover
**Owner:** 0.2.0 Phase 5
**Updated:** 2026-08-24

Phase 4 closes with an installed Qt Quick `LoupeEditor` product shell. Phase 5 owns
mechanical Widgets removal from the maintained graph. This document lists remaining
Widgets residue, oracle targets, and deletion gates so Phase 5 is deletion and
boundary proof rather than unfinished product migration.

## Installed product boundary (Phase 4 exit)

| Target | Phase 4 state | Phase 5 action |
| --- | --- | --- |
| `LoupeEditor` | Installed Quick Controls 2 root via `LoupeEditorQuick` | Keep; prove no Widgets link regression |
| `LoupeEditorWidgetsOracle` | Built, **not installed** migration oracle | Delete after parity evidence archived |
| `LoupeLibQuick` | Installed with product | Keep |
| `LoupeLibInteraction` | STATIC, non-installed seam | Keep |

Evidence: `scripts/verify-installed-product-graph.py`; `UnitTestsProductOperatorLoop`.

## Libraries to remove from the maintained product graph

| Library | Role today | Deletion gate |
| --- | --- | --- |
| `LoupeLibWidgets` | Widgets canvas, dialogs, annotation helpers | No installed target links it; Quick workspaces cover required operator surfaces per `docs/loupe-shell.json` |
| Widgets-bound `LoupeLibGui` | Editor/viewer windows, dialogs, chrome | Quick shell replaces `pdfeditormainwindow.ui` and related surfaces classified in `legacy_surface_disposition` |

## Widgets executables still in default build graph

| Executable | Disposition | Phase 5 route |
| --- | --- | --- |
| `LoupeViewer` | ABSORB into Document workspace | Headlessify, retire, or developer-only |
| `LoupePageMaster` | ABSORB into Pages / Production workspace | Headlessify or route to `PDFPageMasterExport` CLI |
| `LoupeDiff` | OPEN Compare product decision | Headlessify until Compare workspace approved |
| `LoupeLaunchPad` | HIDE / retire hub | Delete launcher surfaces (`RETIRE` in ledger) |

See `docs/product-surface.json` and `docs/loupe-shell.json` workspace IDs.

## Plugin directories (12)

All plugin policies in `docs/loupe-shell.json` include `deletion_condition` metadata.
Widgets plugin binaries may remain installed until Fix/Inspect/Production Quick
surfaces absorb the capability or the plugin is explicitly retired (`STOP-SHIPPING`).

| Plugin | Target workspace | Notes |
| --- | --- | --- |
| `LoupePreflightPlugin` | Preflight | Absorbed by `PreflightPane.qml` + `PreflightController` |
| `ObjectInspectorPlugin` | Inspect | Advanced; contextual `InspectorModel` is product dispatcher |
| `DimensionsPlugin` | Inspect / Pages | Consolidate geometry into Inspect/Pages |
| `OutputPreviewPlugin` | Production Preview | `PreviewStateModel` owns authority semantics |
| `SoftProofingPlugin` | Production Preview | Same workspace |
| `EditorPlugin` | Document / Fix | Bounded fix primitives only |
| `ActionListPlugin` | Fix | Batch operations contextual in Fix workspace |
| `SignaturePlugin` | Inspect | Advanced workflow |
| `ScannerPlugin` | Document | Advanced workflow |
| `RedactPlugin` | Fix | OPEN product decision #66 |
| `AudioBookPlugin` | — | STOP-SHIPPING; delete in Phase 5 |
| `OcrPlugin` | CLI | STOP-SHIPPING; PdfTool owns OCR |

## Legacy `.ui` inventory (48 forms)

Authoritative ledger: `docs/loupe-shell.json` → `legacy_surface_disposition`.

| Disposition | Count | Phase 5 action |
| --- | --- | --- |
| `MIGRATE` | 5 | Delete after Quick replacement proven on merged SHA |
| `CONSOLIDATE` | 35 | Delete after workspace/panel absorbs capability |
| `HEADLESS` | 6 | Retain only if CLI/developer path still needs form; otherwise delete |
| `RETIRE` | 3 | Delete with LaunchPad / AudioBook surfaces |

Verifier: `scripts/verify-loupe-shell-contract.ps1` (fail-closed inventory).

## Configure and package proof required in Phase 5

1. Root CMake must not require `Qt6::Widgets` for the Loupe release profile.
2. `LoupeEditor` install tree must not load `Qt6Widgets` at runtime.
3. Clean-machine package smoke with Widgets unavailable in the product graph:
   - Linux: `scripts/smoke-test-appimage.sh`
   - Windows: `scripts/Invoke-MsiSmokeTest.ps1`, `scripts/smoke-test-install.ps1`
4. Inspect installed artifacts for forbidden `Qt6Widgets` linkage.

## Explicitly not Phase 5 scope from P4-S12

- Trust envelope gates T-01–T03
- Resource envelope R-01 and lifecycle L-01
- Release Gate E-01 exact-SHA promotion
- Phase 6 screen-reader certification (P4-S10 supplies architecture hooks only)

## Evidence crosswalk

| Artifact | Purpose |
| --- | --- |
| `docs/loupe-shell.json` | Single disposition authority |
| `docs/PHASE5_WIDGETS_DELETION_HANDOFF.md` | This handoff |
| `docs/0.2.0-closeout-matrix.md` | Gate ledger |
| `scripts/verify-installed-product-graph.py` | Installed editor Quick-only proof |
| `UnitTestsProductOperatorLoop` | Operator loop on `EditorHost` |
