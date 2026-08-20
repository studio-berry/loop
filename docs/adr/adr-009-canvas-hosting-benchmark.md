# ADR-009: Canvas hosting benchmark and admission

**Status:** accepted
**Implemented-at:** S21 qualification target
**Last-verified:** 2026-08-20 @ 6964a2a3b44b3642eea68212635420bbe6bcda51
**Superseded-by:** none
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

## Verification contract

The target is opt-in through `LOUPE_BUILD_CANVAS_BENCHMARK=ON`. The runner
fails unless every requested candidate reports `status: pass`. CI or local
qualification must record the JSON output together with the platform, Qt
version, QPA platform, Quick backend variables, and graphics API. Headless
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
