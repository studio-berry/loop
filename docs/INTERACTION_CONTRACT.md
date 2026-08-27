# Interaction and overlay contract

Status: P4-S4 (0.2.0 Phase 4). Types live in `LoupeLibInteraction/sources/inputintent.h`,
`interactiontarget.h`, `interactionstate.h`, `hittestsource.h`, `overlayframe.h`,
`overlaybuilder.h`, `interactiontrace.h`, and `interactioncontroller.h`.
Architecture invariant **I24**; test targets `UnitTestsInteractionController` and
`UnitTestsOverlayFrame`. Companion to [PAGE_SURFACE_CONTRACT.md](PAGE_SURFACE_CONTRACT.md),
which owns page pixels.

The host-neutral interaction path is `PointerIntent`/`WheelIntent`/`KeyIntent` →
`InteractionController` → `InteractionState` + `ViewportController` → `OverlayBuilder` →
`OverlayFrame`. Nothing in it owns a document, a scheduler, a renderer, or a presentation
host, and nothing in it mutates a PDF.

## The one rule

**Direct manipulation costs an overlay frame. It never costs a page render, and it never
touches the document.**

Hover, selection, drag preview and marquee resolve to a new `OverlayFrame` and nothing else.
`InteractionController` emits two separate signals so the two costs cannot be confused:

| Signal | Meaning |
| --- | --- |
| `overlayFrameChanged` | Rebuild overlay nodes. Page pixels are still valid. |
| `viewportChanged` | Ask `PageSurfaceCoordinator` for surfaces. Whether anything re-renders is `ViewportController::requestGeneration()`'s decision, not the controller's. |

A pan calls `scrollByPixels()` and does **not** advance the request generation; a wheel-zoom
calls `setZoom(zoom, anchorPx)` and does. That split is issue #142's requirement: a pan
changes which pages are wanted, not what a wanted page should look like, and cancelling
in-flight renders on every pointer delta is exactly what it forbids.

Commit is not this layer's. A completed drag is emitted as a `DragSession`; the owner routes
it through P4-S2's `CommandCatalog`, which stays the only mutation path.

## Input intents

`QMouseEvent`, `QWheelEvent` and `QKeyEvent` do not cross the seam. Three value types do,
each carrying an `InputStamp` — a host-supplied monotonic nanosecond reading plus a sequence
ordinal. Two consequences follow, and both are the point: the Quick canvas and its tests
drive identical code, and a recorded session replays with its original spacing,
which a `QEvent` cannot.

`KeyIntent` carries a key code and no text. Key text can be document content being typed, and
intents are recorded into traces.

## Transient state

`InteractionState` is fenced with `pdfinteraction::RevisionFencedToken` from
`pagesurfacekey.h` — reused, not redeclared, as P4-S3 requires. The revision says which
document state a gesture was begun against; the generation distinguishes two gestures begun
against the same one. The generation is `InteractionController`'s own, not the viewport's: a
zoom during a drag supersedes page surfaces but must not cancel the gesture the user is still
steering.

`DragSession` captures its **grab offset** once, at the press, and never recomputes it. That
is issue #141 AC4: the point the user pressed stays under the pointer, and the object does
not snap its corner to the cursor on the first move.

Below `InteractionState::DefaultDragThresholdPx` of travel a press-and-release is a click.
`completeDrag()` returns nothing for it, so no zero-length transform reaches the command
catalog.

### Cancel reasons

Every one terminates a drag without committing a partial transform (issue #141 AC3). They are
distinct because a diagnostic that cannot tell Escape from a revision change cannot explain a
lost edit.

`Explicit`, `Escape`, `PointerCancelled`, `FocusLost`, `CaptureLost`, `ToolChanged`,
`SelectionChanged`, `RevisionChanged`, `DocumentClosed`.

`RevisionChanged` is the conflict path (issue #141 AC5): a drag begun before a background save
is dropped rather than rebased, because rebasing a transform onto a revision the user never
saw is a silent edit. `PointerAction::Leave` clears hover and deliberately does **not** end a
drag — a gesture that dies at the edge of the view dies every time someone drags to the
border.

## Hit testing

`IHitTestSource` answers for one domain, one page, one page-space point, in no particular
order. `HitTestDispatcher` ranks, and it is the only place precedence is decided:

1. drag handles — a manipulator is never shadowed by the object under it;
2. the current selection — a selected object stays grabbable beneath an overlapping one;
3. `InteractionTargetKind` declaration order: finding, guide, page box, page;
4. smallest page-space area — a marker nested inside a larger one stays reachable;
5. stable id.

Rules 4 and 5 make the answer independent of source registration order and container order,
which is issue #143 AC3. Being selected is **not** a target kind: a target's identity is its
kind plus its id, and promoting a hit to a "selected" kind would make the same object compare
unequal to itself.

Two Core-backed sources ship in this session:

- `EvidenceHitTestSource` over `pdf::PDFEvidenceRecord`, which already carries a stable `id`,
  a 1-based `page` and a page-space `geometry`. Reusing it is what makes the finding clicked
  on the canvas the same identity P4-S8's findings model lists and P4-S9's inspector resolves.
  Records with no usable geometry or no id are counted in `unrenderableRecordCount()` and
  skipped.
- `PageBoxHitTestSource` over media/crop/bleed/trim/art from `pdf::PDFPage`. Edges only: a
  crop box covers most of the page, and an interior hit would shadow every finding inside it.

Text-layout and annotation sources are later implementations of the same interface.
`pdf::PDFSnapper` stays where it is and is not wrapped here.

## Overlays

`OverlayFrame` is immutable and replaced wholesale, exactly as `CanvasSnapshot` is, so a
scene-graph thread never reads a half-built frame. Its invalidation is independent of the page
surface's.

Geometry is in **page space**, never viewport pixels. Viewport coordinates would freeze a
frame to one scroll offset and make a pan need a rebuild; the host applies
`ViewportController::pagePointToViewportMatrix()` — the same matrix the page surfaces use — so
overlays and pixels cannot drift apart through pan, zoom, rotation or page change (issue #143
AC2).

### Z-order

Painted in this order, first furthest back. It is the reverse of the hit-test precedence, so
what is drawn on top is what a click lands on. Ties inside a band break by registration
sequence.

| Band | Contents |
| --- | --- |
| `PageChrome` | Page boxes: media, crop, bleed, trim, art |
| `Guides` | Guides, rulers, measurements |
| `Findings` | Preflight and evidence markers |
| `Hover` | Transient pointer feedback |
| `Selection` | The selected object's outline |
| `DragHandles` | Manipulators |
| `ToolPreview` | Drag preview and tool previews |

The order is fixed, not configurable. A provider that chose its own band would make the
composite order depend on registration.

### Degraded geometry and bounds

A primitive whose geometry is missing, degenerate, or entirely outside the visible part of its
page is emitted with `renderable == false` and counted in `unrenderablePrimitives`, never
dropped and never allowed to abort the pass (issue #143 AC6). A caller can then say "12
markers are out of view" instead of reporting fewer markers than exist.

`OverlayBounds` is pre-registered, not discovered under load, as `PageSurfaceBounds` is:
`maxPrimitives` and `maxFindingPrimitives`. Findings are capped separately so selection,
handles and tool previews — what the user is steering — cannot be crowded out by markers.
What did not fit is reported in `droppedPrimitives`.

Hiding a marker and focusing one are presentation state (`setHiddenFindingIds`,
`setFocusedId`). Neither touches the document, and unhiding brings the marker straight back
(issue #143 AC4).

A frame built against an invalid token is empty. Drawing one is how a stale marker survives a
reload.

## Trace and replay

`InteractionTraceRecorder` is the host-neutral half of issue #140.

**Determinism.** Time comes from `IMonotonicClock`, injected. Frames are opened and closed by
the caller. There is no wall clock, no sampling by elapsed time, no `thread_local` current
recorder, and no background flush, so the same intent sequence against the same clock produces
byte-identical output. `ManualClock` is what a test drives.

**Privacy (issue #140 AC6).** Everything emitted is a timing, a count, an enumeration name, or
a sequence number. No PDF text, pixels, geometry, object contents, file paths, or revision
identity strings. A test scans a full summary and a full trace for document payload.

**Stages** are exclusive: nested time is subtracted, so they sum to at most the frame.
Inclusive stages would make the outermost one dominate every frame and slow-frame attribution
would answer "interaction" for everything.

`Interaction`, `HitTest`, `Overlay`, `PageSurface`, `External`, `Unknown`.

A frame over budget is charged to its largest stage, or to `Unknown` when the stages account
for less than that — a frame slowed by something none of them measured must not have a cause
invented for it.

**Budgets (issue #140 AC2).** `Reference60HzBudgetMs` and `Reference120HzBudgetMs` are
constants and always present. A known refresh rate adds `status: known` and a
`frame_budget_ms`; an unknown one yields `status: unavailable`, null budgets, and
`interaction-trace/refresh-rate-unknown`. Percentiles are nearest-rank, and an empty sample set
reports `available: false` with null percentiles — missing telemetry never reads as zero
([RESOURCE_BUDGETS.md](RESOURCE_BUDGETS.md)).

**Replay.** `InteractionTrace` round-trips through JSON and `InteractionController::replay()`
feeds it back in order, leaving the controller in the state the original session ended in for
the same viewport, hit-test sources and document state. Recording is suppressed during replay,
so replaying into a recording controller does not append the trace to itself.

## Not in this session

- The developer-facing trace overlay and GPU/present timing from issue #140. Neither can exist
  in a layer that links no QML and no scene graph. **Delivered in P4-S5** by `LoupeLibQuick`:
  `CanvasTraceOverlay` renders the recorder's privacy-safe summary, and `CanvasPresentMetrics`
  measures the scene graph's render pass and swap and charges them to `TraceStage::External`.
  See [QUICK_CANVAS_CONTRACT.md](QUICK_CANVAS_CONTRACT.md).
- Text and annotation hit-test sources. `IHitTestSource` is the seam they will implement.
- Marquee and tool gestures beyond the state they occupy in `InteractionKind`. P4-S9 and
  P4-S11 decide which tools survive into the Quick product.
- Keyboard *commands*. Escape, arrows and PageUp/PageDown are viewport presentation and live
  here; everything else is a command id and belongs to the catalog, not to a shortcut table
  in this layer.
