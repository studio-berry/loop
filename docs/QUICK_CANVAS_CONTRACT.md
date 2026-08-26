# Qt Quick canvas contract

Status: P4-S6 (0.2.0 Phase 4). Types live in `LoupeLibQuick/sources/loupecanvasitem.h`,
`canvasnodebuilder.h`, `canvaspalette.h`, `canvaspresentmetrics.h`, `canvastraceoverlay.h`,
and `loupecanvasaccessible.h`. Architecture invariants **I25**, **I26**, and **I27**; test
targets `UnitTestsQuickCanvas`, `UnitTestsCanvasParity`, and `UnitTestsQuickAccessibility`.
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

## Resource lifecycle

Everything in this section is P4-S6, and all of it is about one question: what happens to
retained scene-graph state when the thing that owns it goes away.

`CanvasNodeBuilder` retains a node and a texture per page tile and per overlay primitive.
That retention is what keeps a hover from re-uploading every visible page. It is also the
thing that outlives its owner if nobody handles the teardown, and a texture that outlives its
window is a crash rather than a glitch.

**One flag, acted on in one place.** Every teardown path sets the same deferred reset flag,
and `updatePaintNode` is the only place that acts on it by calling `CanvasNodeBuilder::forget()`.
That is not tidiness: `forget()` is render-thread state, and `updatePaintNode` is the only
point where "on the render thread" and "the GUI thread is blocked" are both true. The flag is
`std::atomic_bool` because two of the paths below raise it from the render thread.

| Event | Thread | What the item does |
| --- | --- | --- |
| `releaseResources()` | GUI | raise the reset flag |
| `itemChange(ItemSceneChange)` | GUI | raise the flag, reattach window and screen signals, republish viewport geometry |
| `QQuickWindow::sceneGraphInvalidated` | render | raise the flag, count the invalidation |
| `QQuickWindow::sceneGraphAboutToStop` | render | raise the flag |
| `QQuickWindow::sceneGraphInitialized` | render → GUI (queued) | mark dirty and ask for a frame |
| `updatePaintNode` with no window or no size | render | raise the flag, delete the node tree |

A reset also marks tiles and overlays dirty. Without that, an invalidation whose next frame
arrived with nothing else dirty would forget every node and draw none, which is a page that
silently vanishes.

**The CPU surface cache survives.** A scene-graph loss drops GPU-side state and nothing else.
`PageSurfaceCoordinator`'s admitted surfaces stay within their existing byte budget, so the
view rebuilds from them without re-parsing a page or admitting anything new. Dropping them
here would make a device loss cost a full re-render of everything visible.

## Display metrics

The device pixel ratio is part of `PageSurfaceKey`, and `PageSurfaceKey::compatibleWith()`
requires it to match exactly — so a surface rendered for the old ratio cannot stand in for
the new one, not even transiently. A page drawn at the wrong ratio is the classic
four-times-too-large page.

The item is the only layer that can see a display, so it is the layer that tells the
viewport. `publishViewportGeometry()` pushes the device pixel ratio, the viewport size in
pixels, and the screen's physical DPI into `ViewportController`, and it runs on: `bind()`, a
geometry change, a scene change, `ItemDevicePixelRatioHasChanged`, `QQuickWindow::screenChanged`,
and the current screen's `physicalDotsPerInchChanged` / `logicalDotsPerInchChanged`.

The last four arrived in P4-S6. Before them a monitor swap or a scale change on the same
window left the viewport on the old ratio indefinitely, because nothing in the item was
listening.

## The presenter's revision fence

`PageSurfaceCoordinator::admit()` decides staleness, and it is thorough. What it cannot do is
reach into a scene graph that is already holding textures. Between a document being replaced
and a host getting round to calling `invalidate()`, the retained nodes are the previous
revision's pixels — and any repaint at all, from a hover to another item in the same window,
draws them again. That is the frozen page.

So `updatePaintNode` checks, on **every** frame rather than only on a dirty one:

- a `CanvasSnapshot` whose `token.revision` is not the coordinator's current revision is
  replaced with an empty one, which drops every retained tile and its texture;
- an `OverlayFrame` whose `token.revision` does not match is replaced the same way, which is
  the host's half of the contract `overlayframe.h` already states.

**Only the revision is fenced here, and that is deliberate.** A superseded *generation* means
the current revision's pixels are being shown at the wrong resolution, and continuous
correspondence explicitly allows that to stand in during a rapid zoom or pan. A superseded
*revision* is a different document, and there is no version of showing it that is correct.

With no coordinator bound there is nothing to fence against, and the item does not invent a
revision to compare with — that would be the second document truth the whole seam exists to
prevent.

## Canvas instrumentation

`CanvasPresentMetrics::summary()` carries the P4-S6 milestones alongside the timing
percentiles, under the same rule as everything else the recorder publishes: a number that has
not been measured is `available: false`, never `0`.

- `present.first_view_ms` — from `bind()` to the first presented frame carrying a
  current-revision page tile. Not the first frame, which is an empty background, and not the
  first admitted surface, which measures the renderer rather than the view. A new `bind()`
  restarts it, because a second document is a second first view.
- `lifecycle.scene_graph_invalidations` and `lifecycle.builder_rebuilds` — how often the
  scene graph went away, and how often the retention maps were rebuilt.
- `lifecycle.tile_bytes` and `lifecycle.tile_bytes_high_water` — the source-image bytes behind
  the retained textures. It is what is actually measurable: the scene graph does not report
  what a `QSGTexture` cost, and a number invented from a format guess would be worse than one
  that says what it measured. The high-water mark survives a reset, which is the point of a
  high-water mark.
- `CanvasFrameStats::refusedStaleFrames` — frames the fence above refused. A canvas that is
  correctly refusing and a canvas that has stopped being asked for anything look identical
  from the outside; this is what tells them apart.

This is canvas evidence and it stays separate from PdfTool's benchmark evidence, which
measures a different thing on a different path.

## Parity against the Widgets oracle

`UnitTestsCanvasParity` is the P4-S6 differential gate and the only target in the tree that
links both canvases. It compares, for the same document, revision, renderer configuration and
page box:

- **layout**, against the Widgets draw-space controller — page extents and the vector between
  page origins, in millimetres, which is the unit the oracle computes in. Absolute position
  depends on centring and scroll offset, which are host state rather than layout;
- **pixels**, against the surface the Core renderer produced — the grabbed frame cropped to
  the placed page rect, inset by one pixel so the antialiased page edge is not measured.

The oracle is narrow on purpose. It is reached through `pdf::PDFDrawSpaceLayoutProbe`, a
migration-only free function in `LoupeLibWidgets` that constructs a `PDFDrawSpaceController`,
asks it for page rectangles and destroys it. The probe exists because
`PDFDrawSpaceController` is declared in a public header but is not exported, so a test outside
that library cannot reach it on Windows; exporting one value-returning function is a smaller
change than exporting the controller, and it means no upstream-derived source is modified at
all. The parity target therefore constructs no `QWidget` and cannot reach
`PDFDrawWidgetProxy`, `QQuickWidget` or `WindowContainer`; ADR-009's prohibition is intact,
and `UnitTestsQuickCanvas` remains Widgets-free and remains what pins I25. Both the probe and
the link are migration-only, and Phase 5 deletes them with the library.

Budgets live in `UnitTests/testdata/canvas-parity/budgets.json` and are read fail-closed: a
case with no entry fails rather than falling back to a default. Observed measurements are
written next to the test binary as evidence, not as a baseline — there is no golden to
regenerate, so the gate cannot be made to pass by refreshing it.

The gate runs on the software backend under an offscreen platform, which is what makes it
deterministic on a runner with no GPU. ADR-010 is right that offscreen is not by itself
scene-graph evidence; native and both-OS evidence still comes from the smoke and benchmark
runs, and P4-S6 does not close that gate.

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

- Native-backend and Windows parity evidence. The differential gate above runs on the
  software backend in the pull-request lane; native and both-OS evidence is an ADR-010
  Phase-4-exit gate and is not closed here.
- Installation and packaging. `LoupeLibQuick` is built opt-in
  (`-DLOUPE_BUILD_QUICK_CANVAS=ON`, which CI sets on both platforms) and is not installed,
  for the same reason `LoupeLibInteraction` is not: ADR-010's packaging and accessibility
  gates are Phase-4-exit gates, and an install rule here would claim a shipped product Quick
  module exists.
- A QML shell that hosts the canvas. P4-S5 delivered the item and its contract and P4-S6
  made it releasable and trustworthy; which shell composes it is P4-S7.
- Text and annotation hit-test sources, and tool gestures beyond the state they occupy in
  `InteractionKind`. Unchanged from P4-S4: `IHitTestSource` is the seam, and P4-S9 and P4-S11
  decide which tools survive.
