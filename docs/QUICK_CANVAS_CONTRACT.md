# Qt Quick canvas contract

Status: P4-S5 (0.2.0 Phase 4). Types live in `LoupeLibQuick/sources/loupecanvasitem.h`,
`canvasnodebuilder.h`, `canvaspalette.h`, `canvaspresentmetrics.h`, and
`canvastraceoverlay.h`. Architecture invariant **I25**; test target `UnitTestsQuickCanvas`.
Companion to [INTERACTION_CONTRACT.md](INTERACTION_CONTRACT.md), which owns the
host-neutral half, and [PAGE_SURFACE_CONTRACT.md](PAGE_SURFACE_CONTRACT.md), which owns
page pixels.

`LoupeLibQuick` is the first and only target in Phase 4 that links `Qt6::Quick`. It closes
the path that P4-S1 through P4-S4 built up to:

```
Qt Quick event  →  PointerIntent / WheelIntent / KeyIntent  →  InteractionController
CanvasSnapshot (page pixels) + OverlayFrame (overlays)      →  QSGNode tree
```

## The one rule

**The Quick layer presents. It decides nothing.**

`LoupeCanvasItem` translates and draws. It performs no hit testing, no gesture recognition,
no panning arithmetic and no zoom arithmetic, because `InteractionController` and
`ViewportController` already own all of it. A host that re-derives any of it is a second
implementation that will disagree with the first, which is what the seam exists to prevent.

Concretely, the item never:

- owns or reads a `pdf::PDFDocument`, `pdf::PDFDocumentSession` or `pdf::PDFJobScheduler`;
- mutates a document — a completed drag leaves as `InteractionController::dragCompleted`
  for the owner to route through P4-S2's `CommandCatalog`, which stays the only mutation
  path;
- invents a generation or a revision — it reads `RevisionFencedToken` and never writes one;
- exposes a document, session, scheduler or pixel buffer to QML.

## What QML may see

| Property | Kind |
| --- | --- |
| `zoom` | presentation |
| `currentPage` | presentation |
| `blockCount` | presentation (layout blocks, not document pages) |
| `activeTool` | transient control state |
| `traceOverlayVisible` | developer diagnostic |
| `highContrast` | accessibility preference |

That is the whole QML-visible surface, and it is ADR-010 rule 5 written as an API: QML owns
presentation, focus and transient control state; C++ owns document identity, revision
fencing, history and mutation.

Wiring is C++ only. `bind(ViewportController*, InteractionController*, PageSurfaceCoordinator*)`
takes the three neutral objects by pointer; none of them is a property, a context property,
or constructible from QML. `SurfaceBuffer` crosses C++ ownership boundaries and nothing
else — never a QML property, JS value, `QByteArray` context property, or URL.

`LoupeLibQuick` installs with `LoupeEditor` and registers `Loupe.Canvas` from
C++ with `QML_NAMED_ELEMENT` and ships no `.qml` of its own. The packaged
`Loupe.Quick` module (P4-S7) hosts `LoupeCanvas` in `CanvasPane.qml`; wiring
remains C++ `bind()` only.

## Admitted hosting

ADR-009 as amended admits the direct `QQuickItem` and prohibits `QQuickPaintedItem`,
`QQuickWidget` and `WindowContainer` as shipped product architecture. `LoupeCanvasItem`
builds scene-graph nodes in `updatePaintNode`; there is no `QPainter` anywhere in the page
pixel or overlay path, and neither `LoupeLibQuick` nor `UnitTestsQuickCanvas` links
`Qt6::QuickWidgets` or `Qt6::Widgets`.

## Threads

`updatePaintNode` runs on the scene-graph render thread while the GUI thread is blocked.
That is what makes it safe to read `CanvasSnapshot` and `OverlayFrame` there without a lock:
both are immutable values replaced wholesale, so a reader never sees half an update.

`CanvasNodeBuilder` holds scene-graph nodes and is therefore render-thread state. The GUI
thread never calls into it — not even to reset it. `bind()`, a window change and
`releaseResources()` all set a pending flag that `updatePaintNode` acts on.

`CanvasPresentMetrics` connects to `QQuickWindow`'s render-thread signals with
`Qt::DirectConnection`, and those handlers do exactly one thing: read the clock into an
atomic. The resulting durations are posted to the GUI thread, where they reach
`InteractionTraceRecorder`. Timestamps stay accurate because they are taken on the thread
the work happened on; the recorder stays single-threaded because only numbers cross.

## Coordinates

`OverlayFrame` geometry is in **page space** and `CanvasSnapshot` tiles are placed in
**device pixels**; the scene graph draws in the item's **logical** coordinates. Both are
converted through `ViewportController`'s own matrix and the window's device pixel ratio, and
never through a matrix derived locally — deriving a second one is how overlays and page
pixels drift apart on a scaled display.

Stroke widths are expanded into triangle ribbons rather than set with
`QSGGeometry::setLineWidth`, which most RHI backends ignore. Fills are expanded into
explicit triangles rather than drawn as a triangle fan, which Metal and D3D do not have.

## Issue #140 carry-overs

Both land here, because neither can exist in a layer with no scene graph:

**GPU and present timing.** `CanvasPresentMetrics` measures the render pass
(`beforeRendering` → `afterRendering`) and the swap wait (`afterRendering` → `frameSwapped`)
and charges the total to `TraceStage::External`, so slow-frame attribution can answer "the
GPU" instead of "unknown". A swap with no matching render pass is counted as
`frames_without_render_stamp` rather than recorded as a zero-cost frame. This layer can also
see a screen, so it publishes the real refresh rate to the recorder; an unknown rate still
reports `unavailable` with a typed reason rather than an assumed 60Hz.

**The developer trace overlay.** `CanvasTraceOverlay` renders
`InteractionTraceRecorder::summary()` and `CanvasPresentMetrics::summary()` into a panel over
the canvas. It takes summaries rather than the recorder, the controller or the document,
which is what makes issue #140 AC6 structural: every input it has is a timing, a count or an
enumeration name, so the panel cannot display page text, geometry, a file path or a revision
identity even while the canvas beneath it shows a filled-in form. A percentile with no
samples renders as `--`, never `0.00`.

## Accessibility and design tokens

`CanvasPalette` mirrors `quick-design-tokens.json`. Two of its rules are structural rather
than cosmetic:

- **`must_not_depend_on_color_alone`** — severity changes stroke width as well as hue, so the
  four severities stay distinguishable in a monochrome capture and to a red-green colour-blind
  reader. `UnitTestsQuickCanvas` asserts the widths are strictly increasing.
- **`must_preserve_focus_indicator`** — the focus ring survives high contrast, and focus stays
  a separate state from selection, exactly as `OverlayFrame` separates them.

The product Quick accessibility runtime remains a Phase-4-exit gate, per ADR-010.

## Not in this session

- Installation and packaging. `LoupeLibQuick` is built opt-in
  (`-DLOUPE_BUILD_QUICK_CANVAS=ON`, which CI sets on both platforms) and is not installed,
  for the same reason `LoupeLibInteraction` is not: ADR-010's packaging and accessibility
  gates are Phase-4-exit gates, and an install rule here would claim a shipped product Quick
  module exists.
- A QML shell that hosts the canvas. This session delivers the item and its contract; which
  shell composes it is P4-S6 and later.
- Text and annotation hit-test sources, and tool gestures beyond the state they occupy in
  `InteractionKind`. Unchanged from P4-S4: `IHitTestSource` is the seam, and P4-S9 and P4-S11
  decide which tools survive.
