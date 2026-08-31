# Make page rendering host-neutral behind an immutable render key

Category: internal
Audience: developers
Breaking-Change: no
Summary: Put the render path into the P4-S1 seam. LoopLibInteraction gains ViewportController,
the PageSurfaceKey/request/result types, PageSurfaceCoordinator over the existing scheduler, a
serialized PDFDocumentSession access policy, and an immutable CanvasSnapshot. All of it is
drivable, and tested, without a QWidget, a QScreen, a scrollbar or a QML engine
(UnitTestsViewportController and UnitTestsPageSurface, QTEST_GUILESS_MAIN, no Qt6::Widgets on
the link line), which is the P4-S3 exit condition.

No second anything: pdf::PDFDocumentContext stays the revision fence and pdf::PDFJobScheduler
stays the only scheduler (reached through P4-S1's IDocumentRevisionSource and IJobSubmitter,
with the PDFJobPriority::VisiblePage and NearViewport classes that already existed and had no
caller), pdf::PDFDocumentSession stays the only compile cache, pdf::PDFRenderer stays the only
renderer, and pdf::PDFProcessingBudget stays the only byte accounting -- a tile is charged to
the job's own budget in the RasterTile pool rather than to a parallel counter.

What the coordinator adds is the admission rule. A completed render enters the canvas only if
its request is still registered, its request generation is still the viewport's, its revision
is still current, its key still equals what the viewport wants for that page now, and the byte
budget can hold it. Anything else is counted and dropped, so a tile from a superseded document
state is never patched into the current frame. Panning deliberately does not supersede demand:
a pan changes which pages are wanted, not what a wanted page should look like, and cancelling
in-flight renders on every pointer delta is exactly what #142 asks not to happen. Details are
locked in docs/PAGE_SURFACE_CONTRACT.md; architecture invariant I23 binds the rule to the two
test targets.

ViewportController ports the layout, zoom-anchor, pan, visible-set and hit-test math out of
PDFDrawSpaceController and PDFDrawWidgetProxy, with every host input injected instead of
scraped: setPixelPerMM replaces QGuiApplication::primaryScreen(), which is wrong on a second
monitor and untestable anywhere; setViewportSizePx replaces QWidget::rect(); and there is no
scrollbar, only an offset and its range for a host to bind. The Widgets classes are untouched
and keep working as the Phase 4 migration oracle; Phase 5 owns removing the duplication.

The renderer seam is a correctness boundary, not an optimization. pdf::PDFDocumentSession
states it is not thread-safe, so one mutex serializes every render for a context and doubles
as the teardown barrier -- detach() takes it, so a worker can never reach a context that is
going away. A worker never calls setRendererFeatures(), because that reaches
PDFDocumentContext::invalidateCaches() and would make every in-flight key, its own included,
stale.

Two deviations from the orchestration page's field list, both deliberate: the qreal targetDpi
and devicePixelRatio become targetPixelSize plus an integral devicePixelRatio1000, because
float key fields compare by luck (PR #332's Widgets probe reached the same conclusion); and
there is no separate RenderMode enum, since render, proof and output-preview identity ride in
featureBits and colorOutputIdentity rather than in an enum with one live value. The terminal
state is Failed rather than Error, matching CommandTerminalState and pdf::PDFJobStatus.

Also: DocumentJobRelay moves out of documentfacade.{h,cpp} to jobrelay.{h,cpp} as JobRelay,
unchanged, so the coordinator reuses the one owner-thread marshalling primitive rather than
reimplementing its detach ordering. No behaviour is removed from LoopLibWidgets or
LoopLibGui, and no installed artifact changes -- LoopLibInteraction is still STATIC,
non-installed, and links only LoopLibCore, Qt6::Core and Qt6::Gui.

Intentionally not delivered: sub-rectangle rendering. The key carries pageTileBounds so a tiled
canvas will not need a key change, but a non-null tile is refused with
page-surface/tiling-unsupported; the backend generalization is #54 and stays deferred. P4-S4
inherits one thing: RevisionFencedToken is the shared generation-plus-revision primitive, and
the interaction state it introduces must reuse it rather than declare a second one.
