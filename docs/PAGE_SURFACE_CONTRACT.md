# Page surface contract

Status: P4-S3 (0.2.0 Phase 4). Types live in `LoupeLibInteraction/sources/pagesurfacekey.h`,
`pagesurfacerenderer.h`, `pagesurfacecoordinator.h`, and `canvassnapshot.h`.
Architecture invariant **I23**; test targets `UnitTestsViewportController` and `UnitTestsPageSurface`.

The host-neutral render path is `ViewportController` → `PageSurfaceCoordinator` →
`pdf::PDFJobScheduler` → `IPageSurfaceRenderer` → owner-thread admission → `CanvasSnapshot`.
Nothing in it owns a document, a scheduler, a renderer, or a presentation host.
`pdf::PDFDocumentContext` stays the revision fence, `pdf::PDFJobScheduler` stays the only
scheduler, and `pdf::PDFDocumentSession` stays the only compile cache.

## The key

`PageSurfaceKey` names everything that can change a page's pixels:

| Field | Why it is in the key |
| --- | --- |
| `revision` | The complete `pdf::PDFRevisionIdentity`, never a subset of its fields |
| `pageIndex` | |
| `rotation` | Viewer rotation, on top of the page's own `/Rotate` |
| `featureBits` | `pdf::PDFRenderer::Features`, which also carries the `ColorAdjust_*` modes |
| `colorOutputIdentity` | The colour-managed output path in force; proof and output-preview modes join this string in P4-S9 |
| `zoomBucket` | Quantized zoom: sixteen buckets per doubling |
| `targetPixelSize` | At least 1x1 |
| `devicePixelRatio1000` | DPR as an integer thousandth, so equality cannot drift |
| `pageTileBounds` | Page-space tile; null means the whole page |

`makePageSurfaceKey()` is the only supported constructor and is where the normalizations
happen. A key assembled field by field elsewhere can compare unequal to an equal one built
here, which is precisely the admission bug the full-key rule exists to stop.

`compatibleWith()` ignores exactly two fields — `zoomBucket` and `targetPixelSize`. A
compatible surface may be scaled into place while the fidelity render is in flight. Nothing
else may substitute, and a surface from another revision never may.

## Admission

A completed render enters the snapshot only when all five hold:

1. the request is still registered — a cancelled or coalesced one cannot arrive late and win;
2. `token.generation` is still the viewport's `requestGeneration()`;
3. `IDocumentRevisionSource::isCurrent(key.revision)`;
4. the key still equals what the viewport wants for that page now;
5. the admitted-byte budget can hold the surface.

Every other outcome is counted and dropped. A tile from a superseded state is never patched
into the current frame, at any priority, for any reason.

`requestGeneration()` advances on zoom, rotation, layout, viewport size, device pixel ratio,
pixels-per-millimetre, block change, and layout invalidation. It deliberately does **not**
advance on a pan: a pan changes which pages are wanted, not what a wanted page should look
like, and cancelling in-flight renders on every pointer delta is what issue #142 forbids.

## Terminal states

`SurfaceTerminalState` is `Complete`, `Cancelled`, `Failed`, `Stale`, `BudgetExhausted`.
Cancelled, Failed and BudgetExhausted are distinct and none is success — the rule invariant
I05 pins for `pdf::PDFJobScheduler`. A budget failure is incomplete work
([RESOURCE_BUDGETS.md](RESOURCE_BUDGETS.md)), never a pass and never an untyped error.
Errors are `page-surface/<kebab-reason>` codes; a message that could quote document content
is not forwarded to a presentation host.

## Bounds and shedding

`PageSurfaceBounds` is pre-registered, not discovered under load: `maxInFlightRequests`,
`maxNearViewportRequests`, `maxInFlightBytes`, `maxAdmittedBytes`. Under pressure, in order:

1. coalesce superseded requests before a worker starts;
2. refuse `NearViewport` work over the prefetch bound;
3. ask the render path to shed prefetch and quality
   (`pdf::PDFDocumentSession::shedPrefetchAndQuality`, advisory and non-blocking);
4. reclaim a prefetch slot for visible work rather than dropping the visible work;
5. evict by least-recent access; refuse an oversize surface outright rather than emptying the
   cache for something that still would not fit.

Recovery needs no rebuild: when pressure clears, the next `requestSurfaces()` re-renders what
was evicted.

## Session access

`pdf::PDFDocumentSession` is not thread-safe. `PDFSessionPageSurfaceRenderer` serializes every
render for a context on one mutex, and that mutex is also the teardown barrier: `detach()`
takes it, so it either waits for the render in flight or observes that none is running, and
after it returns no worker can reach the context again. A worker never reconfigures the
session — `setRendererFeatures()` calls `PDFDocumentContext::invalidateCaches()`, which would
advance the cache generation and make every in-flight key, including its own, stale.

## Not in this session

- Sub-rectangle rendering. `pageTileBounds` is carried by the key so a tiled canvas will not
  need a key change, but a non-null tile is refused with `page-surface/tiling-unsupported`.
  The backend generalization is issue #54 and stays deferred.
- Overlays. Page pixels only; the overlay pass and its independent invalidation are P4-S4.
- A fence for a colour-settings change. `colorOutputIdentity` keeps this cache correct, but
  nothing in Core advances `cacheGeneration` when CMS settings change; that is open work for
  P4-S9's preview authority.
