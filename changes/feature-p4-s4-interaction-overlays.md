# Move canvas interaction and overlays out of QWidget ownership

Category: internal
Audience: developers
Breaking-Change: no
Summary: Complete the host-neutral half of the Phase 4 seam. LoopLibInteraction gains input
intents, transient InteractionState, a hit-test dispatcher over existing Core geometry, an
immutable OverlayFrame with a fixed z-order, a deterministic interaction trace with replay,
and InteractionController tying them together. All of it is drivable, and tested, without a
QWidget, a QScreen, a QPainter or a QML engine (UnitTestsInteractionController and
UnitTestsOverlayFrame, QTEST_GUILESS_MAIN, no Qt6::Widgets on the link line), which is the
P4-S4 exit condition.

The load-bearing rule is that direct manipulation costs an overlay frame and nothing else. A
pointer sweep of forty moves is asserted against the real P4-S3 PageSurfaceCoordinator:
counters().requested is unchanged, the viewport's requestGeneration() is unchanged, the
revision is unchanged, and the renderer is never entered. InteractionController emits
overlayFrameChanged and viewportChanged as separate signals so the two costs cannot be
conflated, and a pan scrolls without advancing the request generation while a wheel-zoom
advances it -- the split issue #142 asks for, since cancelling in-flight renders on every
pointer delta is exactly what it forbids.

No second anything. RevisionFencedToken is reused from pagesurfacekey.h rather than
redeclared, as P4-S3 required; pdf::PDFDocumentContext stays the revision fence; the
interaction generation is the controller's own so a zoom mid-drag supersedes surfaces without
cancelling the gesture. There is no commit path here: a completed drag is emitted as a
DragSession and the owner routes it through P4-S2's CommandCatalog, which stays the only
mutation route. Escape, arrows and PageUp/PageDown are viewport presentation and live here;
every other key is a command id, so no shortcut table is introduced that could become a second
action registry.

Hit testing reuses semantic geometry Core already owns. EvidenceHitTestSource reads
pdf::PDFEvidenceRecord -- stable id, 1-based page, page-space geometry -- so the finding
clicked on the canvas is the identity P4-S8's findings model will list and P4-S9's inspector
will resolve, rather than a parallel id minted here. PageBoxHitTestSource reads media, crop,
bleed, trim and art from pdf::PDFPage, edges only, because a crop box interior would shadow
every finding inside it. Precedence is handles, then the selection, then kind order, then
smallest area, then stable id; a test asserts the answer is identical with the sources
registered in reverse. Being selected is deliberately not a target kind: promoting a hit would
make the same object compare unequal to itself.

Overlays are page-space and immutable, replaced wholesale as CanvasSnapshot is. The z-order is
fixed -- page chrome, guides, findings, hover, selection, handles, tool preview -- and is the
reverse of the hit-test precedence, so what is drawn on top is what a click lands on. Geometry is
mapped by ViewportController::pagePointToViewportMatrix(), the same matrix the page surfaces
use, so overlays cannot drift from pixels through pan, zoom, rotation or page change. A
primitive with missing, degenerate or off-page geometry is emitted with renderable == false and
counted rather than dropped, and OverlayBounds caps findings separately from everything else so
markers cannot crowd out what the user is steering. Details are locked in
docs/INTERACTION_CONTRACT.md; architecture invariant I24 binds the rule to the two test
targets.

The trace is the host-neutral half of #140: an injected IMonotonicClock, caller-driven frames,
nearest-rank percentiles, the 60 Hz and 120 Hz reference budgets as constants with a typed
interaction-trace/refresh-rate-unknown result when the refresh rate is not known, exclusive
stage timings, and slow-frame attribution that charges an unexplained frame to Unknown rather
than inventing a cause. Absent telemetry is null, never zero. Traces round-trip through JSON
and replay through the controller; recording is suppressed during replay so a trace cannot
append itself. A test scans a full summary and a full trace for document payload, and KeyIntent
carries a key code and no text because key text can be document content being typed.

Prior work on the unmerged 0.1.2 candidate branches was ported selectively, not merged:
PDFInteractionState's kind/cancel-reason shape from cursor/s19b-141-interaction-df6f, the
percentile, budget and attribution math from cursor/gh-140-interaction-trace-df6f, and the
z-order table from cursor/gh-143-overlay-pass-df6f. Their QWidget residency, their
thread_local current recorder, their environment-variable configuration and their trace widget
were not. No behaviour is removed from LoopLibWidgets or LoopLibGui, which keep working as
the Phase 4 migration oracle, and no installed artifact changes -- LoopLibInteraction is still
STATIC, non-installed, and links only LoopLibCore, Qt6::Core and Qt6::Gui.

Intentionally not delivered: the developer-facing trace overlay and GPU/present timing from
#140, which cannot exist in a layer that links no QML and no scene graph and belong to P4-S5
and P4-S6; text-layout and annotation hit-test sources, which are later implementations of the
same IHitTestSource seam; and tool gestures beyond the state they occupy in InteractionKind,
since P4-S9 and P4-S11 decide which tools survive into the Quick product. P4-S5 inherits
InputIntent, OverlayFrame and InteractionController as the types LoopCanvasItem binds to.
