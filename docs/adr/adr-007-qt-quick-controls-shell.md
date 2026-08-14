# ADR-007: Qt Quick Controls foundation for the Loupe 1.2 shell

**Status:** accepted
**Implemented-at:** not implemented
**Last-verified:** 2026-08-10 @ 589133449398f029d8b6624b01b49aa4b3343591
**Superseded-by:** none
**Date:** 2026-08-09
**Deciders:** Loupe #178 / 1.2 shell decision

## Context

Loupe's current interactive shell is inherited Qt Widgets code. That shell is
appropriate for the 0.0.x and 1.1 work already in progress, but the 1.2
product surface is a persistent single-document workspace: inspection,
findings, evidence, history, production preview, and approval flows share a
live canvas. It needs layered chrome, anchored inspectors, menus and popovers,
keyboard/focus state, and transitions without coupling product behavior to a
native widget style.

Issue #178 replaces the earlier web/Radix proposal. Loupe is a Qt 6 desktop
application, and the decision must not introduce a second language runtime,
bridge process, or packaging story.

## Decision

Loupe adopts **Qt Quick Controls 2** as the behavioral UI foundation for the
1.2 application shell. Qt Quick Controls supplies control semantics,
composition, focus handling, menus, dialogs, popups, scrolling, and input
behavior. It does not select Loupe's visual language: color, typography,
spacing, icons, animation timing, and custom control styling remain product
design decisions.

This ADR records the foundation only. It does not add Qt Quick modules, QML,
or product UI code. No GUI migration is required before 0.1.1.

### Migration strategy

Migration is staged and mixed-mode first:

1. Keep the existing Widgets shell and `PDFDrawWidget` authoritative while the
   Quick composition and accessibility contracts are validated.
2. Introduce isolated Quick surfaces through the existing Widgets host with a
   single, explicit `QQuickWidget` boundary. The first surface must be a
   non-destructive shell surface, not the PDF canvas or an approval-critical
   mutation flow.
3. Move the application chrome to a Quick root only after the canvas,
   rendering-backend, packaging, and accessibility gates below pass on both
   supported platforms.

There will be one product state model and one command boundary. QML owns
presentation state and control interaction; C++ owns document/session,
preflight, operation-history, and mutation commands. A surface must not create
an alternate document or history model for convenience.

### Canvas hosting

`PDFDrawWidget` remains the authoritative canvas during the mixed-mode phase.
It is not wrapped in `QQuickPaintedItem`: that would add a framebuffer copy and
could change the color-managed rendering and interaction behavior already
covered by the Widgets canvas.

The final Quick shell target is a dedicated `QQuickItem`/scene-graph canvas
adapter that reuses the existing document/session and renderer contracts. The
adapter must be evaluated with the interaction-quality work (#139–#146) and
color-managed canvas work (#163) before the shell cutover. Until that work is
accepted, the Widgets canvas boundary is the deliberate migration choice.

### Rendering backend and fallback

The runtime policy is:

| Deployment | Preferred backend | Required fallback |
| --- | --- | --- |
| Windows x64 | D3D11 RHI | Qt Quick software backend |
| Linux x64 | OpenGL RHI | Qt Quick software backend |
| VM, remote session, or unavailable GPU | platform default attempt | software backend selected explicitly |

The first Quick slice must provide a small offscreen smoke harness that starts
the same QML imports used by the shell and exits successfully under both the
preferred backend and `QSG_RHI_BACKEND=software`. The harness must run on the
Windows and Linux CI/package environments and record the selected backend.
`QT_QPA_PLATFORM=offscreen` is not, by itself, proof that the Quick scene
graph rendered successfully.

This is a release gate for the first Quick implementation, not a claim that
the current Widgets-only build has already verified a Quick backend.

### Licensing and packaging

Qt Quick Controls remains within the existing Qt 6 dependency and deployment
story. The first implementation must inventory the actual linked Qt modules
(at minimum the Quick Controls module and its transitive QML/Quick runtime),
add them to the final-artifact SBOM and notices, and pass the Qt LGPL route in
[#46 / MIC-140](../PACKAGING_LICENSING.md): dynamic linking, replacement/relink
evidence, corresponding source or written offer, and release-asset notices.

No Qt module is added to a shipped artifact by this ADR. The licensing gate is
closed only by the implementation PR that adds the module and its clean-machine
Windows/Linux packaging evidence.

### Accessibility

Qt Quick surfaces extend the existing Editor accessibility baseline in
[`ACCESSIBILITY_BASELINE.md`](../ACCESSIBILITY_BASELINE.md), which remains the
single accessibility standard for Widgets and Quick. Quick must not create a
parallel definition of names, descriptions, focus visibility, keyboard reach,
contrast, status announcements, or high-DPI sizing.

## Alternatives considered

### Continue with Qt Widgets

Widgets is the safe short-term option and remains the migration host. It was
not selected as the long-term 1.2 shell because custom layered chrome,
anchored popovers, transitions, and a product-specific visual language would
be implemented against a native-style imperative hierarchy. That cost is
exactly the shell problem 1.2 introduces. Widgets remains the correct choice
for the current canvas and for any surface that has not passed the bridge
gates.

### React/Radix, Tauri, or another web shell

Rejected. These options add a second runtime, bridge/process lifecycle,
packaging, and accessibility boundary to a Qt application. The earlier web
proposal is historical and is not the Loupe 1.2 direction.

### Immediate full Quick rewrite

Rejected. It would couple the shell decision to unresolved canvas hosting,
RHI deployment, licensing, and accessibility behavior. Staged mixed-mode
adoption preserves feature delivery while those risks are measured.

## Consequences

- New 1.2 product components should be designed against the composition
  pattern in [`QUICK_COMPOSITION.md`](../QUICK_COMPOSITION.md).
- No new UI framework or web runtime is permitted for the 1.2 shell.
- Quick/Widgets focus transfer and input routing are first-class integration
  behavior, not incidental adapter code.
- A Quick foundation does not authorize visual redesign or GUI work before the
  0.1.1 gate.
- The first Quick implementation must carry the backend, packaging,
  licensing, and accessibility evidence required above.

## Acceptance evidence

- [x] Shell migration question answered: adopt Qt Quick Controls for 1.2 with
      staged mixed-mode migration.
- [x] Widgets alternative and its tradeoff recorded.
- [x] Control-behavior versus custom-visual-design boundary recorded.
- [x] Keyboard/focus requirements and bridge behavior documented.
- [x] Windows/Linux preferred and software-fallback policy recorded.
- [x] Qt licensing consequence recorded against #46 / MIC-140.
- [x] Accessibility coordination points to the existing baseline.
- [x] Composition pattern documented separately.
- [ ] Quick backend smoke evidence on Windows and Linux: deferred until the
      first Quick implementation adds the harness; this ADR does not claim it.

## References

- [Qt Quick Controls](https://doc.qt.io/qt-6/qtquickcontrols-index.html)
- [Qt 6.11 changes to Qt Quick](https://doc.qt.io/qt-6/quick-changes-qt6.html)
- [QQuickWindow scene-graph backend selection](https://doc.qt.io/qt-6/qquickwindow.html)
- [Loupe issue #178](https://github.com/studio-berry/loupe/issues/178)
