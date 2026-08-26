# ADR-009: Canvas hosting benchmark and admission

**Status:** accepted
**Implemented-at:** S21 qualification target
**Last-verified:** 2026-08-20 @ c58e2679aba02e8e8c13694dac0a57440e80a67b
**Superseded-by:** none
**Amended:** 2026-08-21 — see "Amendment" section below
**Date:** 2026-08-20
**Deciders:** Loupe 0.1.2 execution directive / #247

## Context

The 0.1.2 Quick-root direction does not select a PDF canvas implementation.
The existing `PDFDrawWidget` is a QWidget-backed renderer with established
input, color, and document-session behavior. The migration candidates have
different ownership and scene-graph semantics, so a passing Quick shell smoke
test cannot establish that any of them can host the production PDF canvas.

S21 therefore needs a small, repeatable qualification surface that measures
hosting mechanics without fabricating PDF-rendering evidence.

## Decision

Add the optional `CanvasBenchmark` target and compare four explicitly named
hosting candidates:

1. `widget-baseline`: a synchronous QWidget paint surface representing the
   current hosting model;
2. `qquickwidget`: a Qt Quick scene hosted inside a QWidget;
3. `window-container`: a QQuickWindow hosted through
   `QWidget::createWindowContainer()`; and
4. `quick-item`: a direct scene-graph `QQuickItem` surface.

Each candidate reports resize timing, synthetic key delivery, focus, a known
surface color, device pixel ratio, and the selected graphics API as one JSON
record per candidate. The benchmark is a qualification probe, not a PDF
fidelity, color-management, accessibility, packaging, or cross-platform
renderer proof.

The S21 admission outcome is:

- **GO:** retain `PDFDrawWidget` and the existing QWidget/hybrid hosting model
  as the current production PDF canvas;
- **CONDITIONAL:** keep `WindowContainer` and direct `QQuickItem` available as
  future Quick-host candidates for non-PDF shell surfaces, subject to hosted
  Windows/Linux evidence and the later focus, accessibility, DPI, backend, and
  color-management gates; and
- **NO-GO:** do not replace the production PDF canvas with any Quick candidate
  based on this synthetic benchmark, and do not introduce `QQuickPaintedItem`
  as a compatibility shortcut.

This decision leaves product QML and the product root migration gated. It also
means a future Quick PDF canvas must provide a dedicated adapter and separate
fidelity/color-management evidence against the Widgets baseline.

## Amendment (2026-08-21)

The binding operator decision recorded in
[ADR-010](adr-010-quick-root-admission.md) supersedes this ADR's GO
conclusion. The 0.2.0 product root is Quick-only: Qt Quick Controls 2 owns
the application root, and a purpose-built direct `QQuickItem` is the shipped
PDF canvas. `PDFDrawWidget`/the Widgets hosting model is retained only as a
temporary, non-installed migration oracle for fidelity/behavior comparison —
it is not a candidate for the production canvas outcome. `WindowContainer`
and `QQuickWidget` are **prohibited** as shipped product architecture (they
may still appear as non-installed qualification/oracle targets, never as an
installed root, product canvas, or operator shell); they are not "future
candidates for non-PDF shell surfaces" as the original CONDITIONAL clause
below allowed.

The original decision's four-way comparison data is retained as diagnostic
reference evidence only — resize timing, focus, DPI, and backend-selection
mechanics for each hosting strategy — not as a vote between architectures.
The benchmark harness itself remains reusable qualification infrastructure
for measuring direct-`QQuickItem` hosting mechanics going forward.

As gh-247's own session handoff already acknowledged, this benchmark never
proved PDF rendering fidelity, color-management correctness, accessibility
tree/runtime behavior, packaging/SBOM completeness, or Windows/Linux hosted
parity. This amendment does not resolve any of those absences either; they
remain explicit later gates under ADR-010's S22 admission contracts and the
Phase 4 hard exit gate.

**Corrected S21 admission outcome:**

- **GO (amended):** admit a dedicated direct `QQuickItem` scene-graph adapter
  as the S21 qualification target for the production PDF canvas migration.
  The `widget-baseline` measurement is retained as the comparison baseline
  used by future fidelity/color-management evidence, not as the winning
  architecture.
- **NO-GO (reaffirmed and extended):** do not host the production PDF canvas,
  or any other shipped product surface, through `WindowContainer`,
  `QQuickWidget`, or `QQuickPaintedItem`. This extends the original
  `QQuickPaintedItem` exclusion to the `window-container`/`qquickwidget`
  candidates that the superseded CONDITIONAL clause had left open.

The original CONDITIONAL clause above is superseded by the corrected outcome
and is retained verbatim only as the historical record of what this ADR
concluded on 2026-08-20, under the architecture assumptions active at that
time.

**CI admission scope (2026-08-21):** confirmed by a real CI run
(`window-container`'s reported `focus_reached: false` under Linux
`QT_QPA_PLATFORM=offscreen` — its `QWindowContainer` focus proxying does not
work without a real display/compositor, while `widget-baseline` and
`quick-item` both pass `status: pass` under the same headless environment;
`qquickwidget` also passed). Since the corrected outcome above already
excludes `window-container` and `qquickwidget` from the shipped product
architecture regardless of their comparison numbers, CI gates on
`widget-baseline` (comparison baseline) and `quick-item` (the corrected S21
target) reaching `status: pass`. `qquickwidget` and `window-container` are
still run and their JSON recorded for diagnostic reference, but a headless
`window-container` focus limitation does not block S21 admission.

## Verification contract

The target is opt-in through `LOUPE_BUILD_CANVAS_BENCHMARK=ON`. The runner
fails unless every requested candidate reports `status: pass`; the CI
admission scope above narrows which candidates gate the pipeline without
changing this underlying per-candidate contract. CI or local qualification
must record the JSON output together with the platform, Qt version, QPA
platform, Quick backend variables, and graphics API. Headless
`QT_QPA_PLATFORM=offscreen` selects a platform; it is not renderer evidence by
itself.

The benchmark does not establish:

- PDF geometry, blend, ICC, or color-management fidelity;
- QWidget-to-Quick-to-QWidget focus traversal in the product shell;
- accessibility tree/runtime behavior;
- Windows/Linux hosted parity; or
- final-package, SBOM, notices, or Qt LGPL replacement/relink behavior.

Those remain explicit later gates in ADR-007 and ADR-010.

## References

- [Quick-root admission](adr-010-quick-root-admission.md)
- [Qt Quick Controls shell](adr-007-qt-quick-controls-shell.md)
- [Quick composition contract](../QUICK_COMPOSITION.md)
- [Issue #247](https://github.com/studio-berry/loupe/issues/247)
