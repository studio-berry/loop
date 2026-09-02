# ADR-010: Quick-root admission and shell boundary

**Status:** accepted
**Implemented-at:** S22 admission contracts
**Last-verified:** 2026-08-20 @ 6bb1df833c1621b8b9bc32793c7c4449f70c69a2
**Superseded-by:** none
**Supersedes:** the migration-topology and pre-0.1.1 deferral clauses of [ADR-007: Qt Quick Controls foundation](adr-007-qt-quick-controls-shell.md)
**Date:** 2026-08-20
**Deciders:** Loop 0.1.2 execution directive / #178

## Decision

The 0.2.0 product root is Qt Quick. This is an operator directive about the
final composition boundary, not evidence that the current Widgets shell or PDF
canvas has already migrated.

The following rules are binding:

1. A normal `QWidget` is not a `QQuickItem`. It must not be presented as a
   native Quick canvas by casting, embedding assumptions, or a hidden paint
   bridge.
2. `PDFDrawWidget` remains the authoritative canvas until a dedicated direct
   `QQuickItem`/scene-graph adapter passes the canvas and color-management
   gates. `QQuickPaintedItem` is not an automatic compatibility solution.
3. A `QWindowContainer`/WindowContainer hybrid is a measured migration
   candidate only. It is not a supported product outcome without an explicit
   operator decision that supersedes this ADR.
4. Qt Quick Controls 2 is the behavioral foundation. Visual language,
   security policy, and admitted QML modules remain governed by their existing
   contracts.
5. QML owns presentation, focus, and transient control state. C++ remains the
   owner of document identity, revision fencing, preflight, history, plugins,
   and mutation commands.

The historical 0.1.1 implementation work was not completed as a product
release. Its unfinished trust-contract, independent-validation,
resource-envelope, and lifecycle-model units are one multipart 0.2.0
qualification gate. A successful historical workflow or version-policy check
does not create a 0.1.1 release or convert those units into a GO.

## Supersession boundary

ADR-007 remains the source for the Qt Quick Controls choice, ownership split,
and accessibility intent. This ADR supersedes its assumption that the final
shell may be admitted through a generic mixed-mode root and its wording that
GUI work is deferred specifically until a historical 0.1.1 release gate.

## Admission contracts

The qualification surface is not product completion. It must include:

- a QML import and boundary policy checked by
  `scripts/verify-quick-shell-policy.py`;
- design, focus, motion, contrast, and high-contrast token checks;
- a scene-graph smoke executable that records the selected graphics API under
  native and `QT_QUICK_BACKEND=software` modes;
- an accessibility contract preserving the existing baseline;
- a threat model denying network, filesystem, process, remote-import, and
  customer-payload paths from QML.

`QT_QPA_PLATFORM=offscreen` is only a headless platform selection. A
successful smoke result must include scene-graph initialization and a
non-unknown graphics API. `QSG_RHI_BACKEND=software` is not the software
backend contract.

## Acceptance evidence

- [x] 0.1.1 carry-forward ownership is recorded as a 0.2.0 prerequisite.
- [x] Quick-root directive, QWidget/QQuickItem boundary, and hybrid candidate
      rule are recorded.
- [x] Static security, accessibility, and design-token contracts are named.
- [x] Canvas benchmark and admission outcome are accepted on the current
      candidate lineage (ADR-009 amended; PR #338 / P4-S6 parity tests).
- [x] Windows and Linux native and software smoke evidence is attached on the
      P4-S12 branch (`QuickShellSmoke`, `ProductQuickAccessibilitySmoke`; hosted
      CI pending merge to `dev`).
- [ ] Final-artifact SBOM, notices, LGPL replacement/relink evidence, and
      clean-machine package smoke close the packaging gate.
- [x] Product Quick accessibility runtime and focus-bridge evidence close the
      GUI admission gate on the branch (`LoopCanvasAccessible`, `FocusRestoration`,
      `ProductQuickAccessibilitySmoke`; Phase 6 screen-reader proof remains open).
