# ADR-010: Quick-root admission and shell boundary

**Status:** accepted
**Implemented-at:** S22 admission contracts
**Last-verified:** 2026-08-20 @ 6bb1df833c1621b8b9bc32793c7c4449f70c69a2
**Superseded-by:** none
**Supersedes:** the migration-topology and pre-0.1.1 deferral clauses of [ADR-007: Qt Quick Controls foundation](adr-007-qt-quick-controls-shell.md)
**Date:** 2026-08-20
**Deciders:** Loupe 0.1.2 execution directive / #178

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

## S22 admission contracts

The qualification surface is deliberately not product UI or product
completion. It must include:

- a QML import and boundary policy checked by
  `scripts/verify-quick-shell-policy.py`;
- provisional typography, spacing, focus, motion, state, contrast, and
  high-contrast tokens checked from `docs/quick-design-tokens.json`;
- a scene-graph smoke executable that uses the shell's imports and records the
  selected `GraphicsApi` under native and `QT_QUICK_BACKEND=software` modes;
- an explicit accessibility contract that preserves the Widgets baseline and
  requires a later Quick/Widgets focus-bridge test; and
- a threat model in
  [`QUICK_SHELL_THREAT_MODEL.md`](../QUICK_SHELL_THREAT_MODEL.md) that denies
  network, filesystem, process, remote-import, and customer-payload paths
  from QML.

`QT_QPA_PLATFORM=offscreen` is only a headless platform selection. A
successful smoke result must include scene-graph initialization
(`scene_graph_initialized`) and a non-`Unknown` graphics API. On Windows,
WARP preference is represented by `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`; the
Qt Quick software backend is represented by `QT_QUICK_BACKEND=software`.
`QSG_RHI_BACKEND=software` is **not** the software-backend contract.

## Acceptance evidence

- [x] 0.1.1 carry-forward ownership is recorded as a 0.2.0 prerequisite.
- [x] Quick-root directive, QWidget/QQuickItem boundary, and hybrid candidate
      rule are recorded.
- [x] Static security, accessibility, and design-token contracts are named.
- [x] Canvas benchmark and admission outcome are accepted on the current
      candidate lineage. ADR-009's amended outcome (direct `QQuickItem`
      admitted; `widget-baseline`/`quick-item` required, `qquickwidget`/
      `window-container` diagnostic-only) is confirmed by real CI on both
      Windows and Linux, native and software backends — runs
      [32562071695](https://github.com/studio-berry/loupe/actions/runs/32562071695)
      (both platforms green on this evidence), corroborated by
      [32557415495](https://github.com/studio-berry/loupe/actions/runs/32557415495)
      and [32559534947](https://github.com/studio-berry/loupe/actions/runs/32559534947).
- [x] Windows and Linux native and software smoke evidence is attached.
      `QuickShellSmoke` reported `scene_graph_initialized` with a non-`Unknown`
      `GraphicsApi` on both OSes and both backends (Windows: `d3d11`/
      `software`; Linux: `software`/`software` — GitHub-hosted `ubuntu-24.04`
      runners have no GPU, so the "native"/preferred-backend request degrades
      to software, matching ADR-007's own documented "unavailable GPU" row).
      Same run evidence as above.
- [ ] Final-artifact SBOM, notices, LGPL replacement/relink evidence, and
      clean-machine package smoke close the packaging gate. **Not closeable
      yet:** no shipped/installed product Quick module exists at this stage
      to produce that evidence from; this is a Phase-4-exit gate.
- [x] Focus-bridge qualification evidence: the QWidget-to-Quick-to-QWidget
      keyboard focus and accessibility-role probe passed natively and under
      the software backend on both Windows and Linux (same runs above).
- [ ] Product Quick accessibility runtime remains open — the focus-bridge
      probe above is qualification-only infrastructure, not the product
      accessibility runtime evidence a real Quick shell would require; that
      remains a Phase-4-exit gate for the same reason as packaging.

## References

- [Qt Quick Controls](https://doc.qt.io/qt-6/qtquickcontrols-index.html)
- [QQuickWindow scene-graph backend selection](https://doc.qt.io/qt-6/qquickwindow.html)
- [Qt Quick shell composition contract](../QUICK_COMPOSITION.md)
- [Quick shell threat model](../QUICK_SHELL_THREAT_MODEL.md)
