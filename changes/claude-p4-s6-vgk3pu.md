# Make the Quick canvas releasable, recoverable and trustworthy

Category: internal
Audience: developers
Breaking-Change: no
Summary: P4-S5 delivered a canvas that presents. P4-S6 makes it survive the things that take
its scene-graph state away, follow the display it is drawn on, and refuse to draw a document
that is no longer open. Architecture invariant I26 and a new test target UnitTestsCanvasParity
pin the result; UnitTestsQuickCanvas gains the scene-graph half it never had.

Scene-graph lifecycle. QQuickWindow::sceneGraphInvalidated was connected nowhere in the
repository, so a backend loss without a window change left CanvasNodeBuilder's retention maps
pointing at nodes the scene graph had already destroyed. It, sceneGraphAboutToStop and
sceneGraphInitialized are now connected per window, alongside the releaseResources and
ItemSceneChange paths that already existed. Every one of them sets the same deferred reset
flag -- now std::atomic_bool, because two of them run on the render thread -- and
updatePaintNode remains the only place that acts on it, because it is the only point where
"on the render thread" and "the GUI thread is blocked" are both true, which is what
CanvasNodeBuilder::forget() requires. A reset now also marks tiles and overlays dirty:
without that, an invalidation whose next frame arrived with nothing else dirty forgot every
node and drew none, which is a page that silently vanishes. The updatePaintNode early return
that deletes the node tree when the window or the size is gone raises the flag explicitly
rather than relying on the next call happening to take the fresh-root branch.

The authoritative CPU surface cache deliberately survives all of it. A scene-graph loss drops
GPU-side state and nothing else, so the view rebuilds from admitted surfaces without
re-parsing a page -- which is what the 0.2.0 resource-lifecycle rule asks for, and what
sceneGraphInvalidationRebuildsWithoutReparsing asserts by checking that neither the render
count nor the request count moved.

Display metrics. publishViewportGeometry ran on bind, a geometry change and a scene change,
and on nothing else, so a monitor swap or a scale change on the same window left
ViewportController on the old device pixel ratio indefinitely. ItemDevicePixelRatioHasChanged,
QQuickWindow::screenChanged and the current screen's physicalDotsPerInchChanged and
logicalDotsPerInchChanged now republish it and re-request surfaces. This matters more than it
looks: the ratio is part of PageSurfaceKey and compatibleWith() requires it to match exactly,
so a surface rendered for the old ratio cannot stand in even transiently, and a page drawn at
the wrong ratio is the classic four-times-too-large page.

A presenter-side revision fence. PageSurfaceCoordinator::admit() decides staleness and is
thorough, but it cannot reach into a scene graph that is already holding textures: between a
document being replaced and a host calling invalidate(), the retained nodes are the previous
revision's pixels, and any repaint at all -- a hover, another item in the same window --
draws them again. updatePaintNode now compares CanvasSnapshot::token and OverlayFrame::token
against PageSurfaceCoordinator::currentRevision() (a new one-line accessor over the revision
source it already held) on every frame rather than only on a dirty one, and syncs an empty
value instead, which drops the retained tiles and their textures. Only the revision is
fenced, deliberately: a superseded generation means the current document's pixels at the
wrong resolution, which continuous correspondence allows to stand in during a rapid zoom; a
superseded revision is a different document, and there is no version of showing it that is
correct. With no coordinator bound there is nothing to fence against and the item does not
invent a revision to compare with.

Instrumentation, kept separate from PdfTool's benchmark evidence and under the existing rule
that an unmeasured number is unavailable rather than zero: present.first_view_ms (bind to the
first presented frame carrying a current-revision tile -- not the first frame, which is an
empty background, and not the first admitted surface, which measures the renderer),
lifecycle.scene_graph_invalidations, lifecycle.builder_rebuilds, lifecycle.tile_bytes and its
high-water mark, and CanvasFrameStats::refusedStaleFrames. That last one exists because a
canvas that is correctly refusing and a canvas that has stopped being asked for anything look
identical from the outside. All of them render in the developer trace overlay.

The parity gate. UnitTestsCanvasParity is the differential half of P4-S6 and the only target
in the tree that links both canvases. For the same document, revision, renderer configuration
and page box it compares layout against PDFDrawSpaceController -- page extents and the vector
between page origins, in millimetres, which is the unit the oracle computes in -- and pixels
against the surface the Core renderer produced, cropping the grabbed frame to the placed page
rect and insetting by one pixel so the antialiased page edge is not measured. Cases cover one
column, two columns, 90-degree rotation, a crop box smaller than the media box, alpha blend,
and interaction coordinate round-tripping.

The Widgets link is the Phase 4 migration oracle the plan of record authorises, and it is
narrow. PDFDrawSpaceController is declared in a public header but is not exported, so a test
outside that library cannot reach it on Windows, where a shared library exports nothing
without the macro. Rather than export the controller -- which would mean editing an
upstream-derived header that is not clang-format clean, and so reformatting it wholesale, the
kind of churn that invalidates a diff as migration evidence -- LoupeLibWidgets gains one new
pair of files: pdf::PDFDrawSpaceLayoutProbe, an exported free function that constructs a
controller, asks it for page rectangles in millimetres and destroys it. No signals, no
document ownership, no widget, and no upstream source modified. UnitTestsCanvasParity is its
only caller, and Phase 5 deletes both with the library. ADR-009's prohibition is intact:
the parity target constructs no QWidget and cannot reach PDFDrawWidgetProxy, QQuickWidget or
WindowContainer, and UnitTestsQuickCanvas stays Widgets-free and stays what pins I25.

Budgets live in UnitTests/testdata/canvas-parity/budgets.json and are read fail-closed: a
case with no entry fails rather than falling back to a default, because a differential gate
that invents its own tolerance is not a gate. Observed measurements are written next to the
test binary as evidence, not as a baseline -- there is no golden to regenerate, so the gate
cannot be made to pass by refreshing it.

Both Quick test targets now run under QT_QUICK_BACKEND=software as well as
QT_QPA_PLATFORM=offscreen. The scene-graph cases need a scene graph that actually renders --
releaseResources, invalidation and texture retention have no observable behaviour without one
-- and the software backend renders the same way on a GPU-less runner as on a developer
machine. ADR-010 is right that offscreen is not by itself scene-graph evidence; that is what
the native and software smoke runs are for.

Intentionally not delivered: native-backend and Windows parity evidence, which needs the
non-fast CI lane and remains an ADR-010 Phase-4-exit gate; installation or packaging of
LoupeLibQuick, for the same reason; and the QML shell that composes this canvas, which is
P4-S7. Nothing here adds a scheduler, a revision identity, a renderer, a command registry or
a mutation path.
