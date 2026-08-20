# ADR-010: Quick-root admission and shell boundary

**Status:** accepted
**Implemented-at:** S22 admission contracts
**Last-verified:** 2026-08-20 @ 6bb1df833c1621b8b9bc32793c7c4449f70c69a2
**Superseded-by:** none
**Supersedes:** the migration-topology and pre-0.1.1 deferral clauses of [ADR-007](adr-007-qt-quick-controls-shell.md)
**Date:** 2026-08-20
**Deciders:** Loupe 0.1.2 execution directive / #178

## Decision

The 0.1.2 shell's eventual product root is Qt Quick. This is an operator
directive about the final composition boundary, not evidence that the current
Widgets shell or PDF canvas has already migrated.

The following rules are binding for later Quick sessions:

1. A normal `QWidget` is not a `QQuickItem`. It must not be presented as a
   native Quick canvas by casting, embedding assumptions, or a hidden paint
   bridge.
2. `PDFDrawWidget` remains the authoritative canvas until a dedicated
   `QQuickItem`/scene-graph adapter passes the canvas and color-management
   gates. `QQuickPaintedItem` is not an automatic compatibility solution.
3. A `QWindowContainer`/WindowContainer hybrid is a measured migration
   candidate only. It requires the S21 benchmark and an accepted outcome for
   resize, input, focus, color, DPI, and backend behavior before admission.
4. Qt Quick Controls 2 remains the behavioral foundation. The visual language
   is constrained by [`quick-design-tokens.json`](../quick-design-tokens.json),
   and the security boundary is constrained by
   [`quick-shell-policy.json`](../quick-shell-policy.json).
5. QML owns presentation, focus, and transient control state. C++ remains the
   owner of document identity, revision fencing, preflight, history, plugins,
   and mutation commands.

The 0.1.1 prerequisite is now satisfied by the public GitHub `0.1.1` ref at
`0cf17886b335a04eb15f1cbb732bc3a551c07f77`; its CodeQL, Documentation truth,
Supply Chain Policy, and Release Gate workflow runs completed successfully.
This closes G0 only. It does not convert the missing S21 benchmark, product
accessibility runtime proof, or clean-machine package evidence into a GO.

## Supersession boundary

ADR-007 remains the source for the Qt Quick Controls choice, ownership split,
and accessibility intent. This ADR supersedes only its assumption that the
final shell may be admitted through a generic mixed-mode root and its wording
that GUI work is deferred specifically until the 0.1.1 gate. The new gate is
S21 canvas admission plus S22 backend, security, token, accessibility, and
packaging contracts.

## S22 contracts

The first qualification surface is deliberately not product UI. It consists
of:

- a QML import and boundary policy checked by
  `scripts/verify-quick-shell-policy.py`;
- provisional typography, spacing, focus, motion, state, contrast, and
  high-contrast tokens checked from `docs/quick-design-tokens.json`;
- a scene-graph smoke executable that uses the shell's imports and records the
  selected `GraphicsApi` under native and `QT_QUICK_BACKEND=software` modes;
- an explicit accessibility contract that preserves the Widgets baseline and
  requires a later Quick/Widgets focus bridge test; and
- a threat model in [`QUICK_SHELL_THREAT_MODEL.md`](../QUICK_SHELL_THREAT_MODEL.md)
  that denies network, filesystem, process, remote-import, and customer-payload
  paths from QML.

`QT_QPA_PLATFORM=offscreen` is only a headless platform selection. A successful
smoke result must include `scene_graph_initialized` and a non-`Unknown`
graphics API. On Windows, WARP preference is represented by
`QSG_RHI_PREFER_SOFTWARE_RENDERER=1`; the Qt Quick software backend is
represented by `QT_QUICK_BACKEND=software`. `QSG_RHI_BACKEND=software` is not
the software-backend contract.

## Acceptance evidence

- [x] Public 0.1.1 ref and release-gate workflow evidence recorded for G0.
- [x] Quick-root directive, QWidget/QQuickItem boundary, and WindowContainer
      candidate rule recorded.
- [x] Static security, accessibility, and design-token contracts added.
- [x] Qualification-only scene-graph smoke harness added and wired for native
      and software modes in Windows/Linux CI.
- [x] S21 benchmark and ADR-009 outcome retain the Widgets/hybrid production
      canvas and leave Quick PDF-canvas migration conditional.
- [ ] Windows and Linux CI runs produce native and software smoke evidence.
- [ ] Final-artifact SBOM, notices, LGPL replacement/relink evidence, and
      clean-machine package smoke close the packaging gate.
- [ ] Product Quick accessibility runtime and QWidget-to-Quick-to-QWidget focus
      bridge evidence close the GUI gate.

## References

- [Qt Quick Controls](https://doc.qt.io/qt-6/qtquickcontrols-index.html)
- [QQuickWindow scene-graph backend selection](https://doc.qt.io/qt-6/qquickwindow.html)
- [Qt Quick shell composition contract](../QUICK_COMPOSITION.md)
- [Quick shell threat model](../QUICK_SHELL_THREAT_MODEL.md)
