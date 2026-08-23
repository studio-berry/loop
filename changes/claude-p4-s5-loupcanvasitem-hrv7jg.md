# Present the canvas from a direct QQuickItem

Category: internal
Audience: developers
Breaking-Change: no
Summary: Close the Phase 4 seam at the presentation end. A new LoupeLibQuick target -- the
first and only one in Phase 4 that links Qt6::Quick -- adds LoupeCanvasItem, a direct
QQuickItem that turns Qt Quick events into the PointerIntent, WheelIntent and KeyIntent
values InteractionController takes, and turns CanvasSnapshot page pixels and OverlayFrame
overlays into a scene-graph node tree. It is opt-in (-DLOUPE_BUILD_QUICK_CANVAS=ON, which CI
sets on Linux and Windows) and not installed, for the same reason LoupeLibInteraction is not:
ADR-010's packaging and accessibility gates are Phase-4-exit gates.

The load-bearing rule is that the Quick layer presents and decides nothing. The item does no
hit testing, no gesture recognition and no viewport arithmetic, because InteractionController
and ViewportController already own all of it; a completed drag leaves as dragCompleted for
the owner to route through P4-S2's CommandCatalog, which stays the only mutation path. Its
entire QML-visible surface is zoom, currentPage, blockCount, activeTool, traceOverlayVisible
and highContrast. No document, session, scheduler or SurfaceBuffer is reachable from QML;
wiring is a C++-only bind() taking the three neutral objects by pointer. That is ADR-010
rule 5 written as an API, and it is what UnitTestsQuickCanvas and architecture invariant I25
pin.

The two issue #140 carry-overs docs/INTERACTION_CONTRACT.md assigns to this session land
here, because neither can exist in a layer with no scene graph. CanvasPresentMetrics measures
the scene graph's render pass and swap wait and charges them to TraceStage::External, so
slow-frame attribution can answer "the GPU" rather than "unknown"; a swap with no matching
render pass is counted rather than recorded as a zero-cost frame, and this layer can see a
screen so it publishes the real refresh rate the neutral layer had to report as unavailable.
CanvasTraceOverlay renders the recorder's summary into a panel over the canvas, and takes
summaries rather than the recorder, the controller or the document -- which makes #140 AC6
structural rather than a review rule: every input the panel has is a timing, a count or an
enumeration name, so it cannot display page text, geometry, a path or a revision identity
even while the canvas beneath it shows a filled-in form. A percentile with no samples renders
as "--", never "0.00".

Three implementation rules are worth keeping. Strokes are expanded into triangle ribbons
rather than set with QSGGeometry::setLineWidth, which most RHI backends ignore, and fills are
expanded into explicit triangles rather than drawn as a triangle fan, which Metal and D3D do
not have -- stroke width is load-bearing here, because the design tokens require severity to
be legible without colour. Overlay geometry is mapped through ViewportController's own matrix
and the window's device pixel ratio rather than a matrix derived locally, since a second
matrix is how overlays and page pixels drift apart on a scaled display. And CanvasNodeBuilder
is render-thread state that the GUI thread never calls into: bind(), a window change and
releaseResources() set a pending flag that updatePaintNode acts on, and CanvasPresentMetrics'
DirectConnection handlers only read a clock into an atomic, posting the durations to the GUI
thread so the single-threaded recorder is never touched from the render thread.

LoupeLibInteraction is unchanged and still links no Widgets, Qml or Quick;
scripts/verify-interaction-boundary.py and invariant I21 continue to enforce that, and the
new target is deliberately not added to docs/interaction-boundary-policy.json, whose
forbidden-include list is global across the targets it names. Also anchors the five hook
commands in .claude/settings.json at $CLAUDE_PROJECT_DIR: they were relative paths, so a
single working-directory change in an agent session made every hook fail to resolve and
blocked Bash, Write and Edit for the rest of that session.
