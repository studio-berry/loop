// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef INTERACTIONSTATE_H
#define INTERACTIONSTATE_H

#include "inputintent.h"
#include "interactionglobal.h"
#include "interactiontarget.h"
#include "pagesurfacekey.h"

#include <QMetaType>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <optional>

namespace pdfinteraction
{

/// What the pointer is currently doing.
enum class InteractionKind
{
    None,
    Hover,
    Drag,
    Marquee,
    Pan,

    /// A text selection being dragged out (issue #232). It is its own kind
    /// rather than a ToolGesture because what it produces is a typed
    /// TextSelection, not a command.
    TextSelect,

    /// A measurement being dragged out (issue #238). It never becomes a
    /// DragSession: a measurement is presentation state, not a document edit.
    Measure,

    ToolGesture
};

const char* getInteractionKindName(InteractionKind kind);

/// The agreed tool vocabulary (issue #103). A tool decides what a left-button
/// gesture means. The set is closed and the host parses a tool name into one of
/// these at its own boundary, so a free-form string cannot reach the pointer
/// path and an unknown tool is refused rather than coerced into a default.
enum class InteractionTool
{
    /// Direct manipulation: a left press selects a target and a second press
    /// on it starts a drag. This is the behavior every gesture had before the
    /// vocabulary existed, so Select is the default.
    Select,

    /// The left button is a viewport grip: a left drag pans and never selects,
    /// retargets or starts a transform. The configured pan button still pans
    /// in every tool.
    Hand,

    /// The left button drags out a text selection on the page under the pointer
    /// (issue #232). What it produces is the typed TextSelection state; it never
    /// selects a target, never starts a transform and never reaches the document.
    Text,

    /// The left button drags out a measurement in production units (issue #238).
    /// A measurement is presentation state fenced to the revision it was taken
    /// from; it is never a document mutation and never a DragSession.
    Measure
};

const char* getInteractionToolName(InteractionTool tool);

/// Parses a tool name at the host boundary. Returns nothing for a name outside
/// the vocabulary; the caller reports that, and the active tool is left as it
/// was.
std::optional<InteractionTool> interactionToolFromName(const QString& name);

/// Why a transient interaction ended without completing.
///
/// Every one of these is a real path a host takes, and each must terminate a
/// drag without committing a partial transform -- issue #141 AC3. They are
/// distinct rather than one "cancelled" because a diagnostic that cannot tell
/// Escape from a revision change cannot explain a lost edit.
enum class InteractionCancelReason
{
    None,
    Explicit,
    Escape,
    PointerCancelled,
    FocusLost,
    CaptureLost,
    ToolChanged,
    SelectionChanged,
    RevisionChanged,
    DocumentClosed
};

const char* getInteractionCancelReasonName(InteractionCancelReason reason);

/// A drag in progress. Transient in the strongest sense: nothing here is ever
/// written to a document, and dropping the whole struct is a complete cancel.
struct DragSession
{
    InteractionTarget target;

    /// Where the press landed, in viewport pixels and in the target page's
    /// space. The page-space origin is kept because the viewport one stops
    /// meaning anything if the view scrolls mid-drag.
    QPoint originPx;
    QPointF originPagePoint;

    /// originPagePoint minus the target's page-space top-left at press time.
    /// Applying this on every move is why the object does not jump when the
    /// press lands away from its centre -- issue #141 AC4.
    QPointF grabOffset;

    /// Latest pointer position, and the page-space translation implied by it.
    QPoint currentPx;
    QPointF pageDelta;

    Qt::MouseButton button = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;

    /// False until the pointer has moved past the hysteresis threshold. A press
    /// and release inside the threshold is a click, not a zero-length drag, and
    /// must not produce a document operation.
    bool exceededThreshold = false;

    /// Where the target would land if the drag completed now. Presentation only:
    /// it feeds the overlay, never the document.
    QRectF previewPageBounds;

    /// The fence this drag completed against. completeDrag() already refuses a
    /// stale token; carrying it lets the consumer re-check before it mutates.
    RevisionFencedToken fence;
};

/// One measurement on a page, in production units (issue #238).
///
/// A measurement is derived presentation state: it names page-space points and
/// the millimetres they span after the page's /UserUnit scale, and no method
/// anywhere turns it into a document edit. The rectangle fields are the
/// axis-aligned region the two points span, so a region drag reads as a
/// distance, a width x height, a perimeter and an area at once.
struct Measurement
{
    int pageIndex = -1;
    QPointF startPagePoint;
    QPointF endPagePoint;

    /// Millimetres per page-space point this measurement was taken with,
    /// including the page's /UserUnit. Carried so a readout can be explained
    /// and re-derived without another document lookup.
    qreal pointToMM = 1.0;

    qreal distanceMM = 0.0;
    qreal widthMM = 0.0;
    qreal heightMM = 0.0;
    qreal perimeterMM = 0.0;
    qreal areaMM2 = 0.0;
    qreal angleDegrees = 0.0;

    /// The fence the measurement was taken against. A measurement whose fence no
    /// longer matches is dropped rather than carried onto new geometry.
    RevisionFencedToken fence;

    bool isValid() const { return pageIndex >= 0 && fence.isValid(); }
    bool operator==(const Measurement& other) const = default;
};

/// Derives a measurement from two page-space points and the page's
/// millimetres-per-point scale. Pure; the caller supplies the scale it read
/// from the page's own /UserUnit.
Measurement measureBetween(int pageIndex, QPointF startPagePoint, QPointF endPagePoint, qreal pointToMM);

/// The operator-facing readout of a measurement's distance, e.g. "42.3 mm".
/// The layer names the unit so no host invents a second one.
QString measurementReadout(const Measurement& measurement);

/// What produced a text selection.
enum class TextSelectionKind
{
    /// A text range: a drag between two page points, or a whole page.
    Text,

    /// One whole text block, which is what a table or a paragraph is to the
    /// text layer (issue #232).
    Block
};

const char* getTextSelectionKindName(TextSelectionKind kind);

/// A typed text selection (issue #232).
///
/// Bound to the revision it was taken from through `fence`; a revision change
/// clears it rather than carrying text onto geometry the operator never saw.
/// `available == false` is a real state -- a page with no text layer -- and it
/// is never an empty string presented as success.
struct TextSelection
{
    TextSelectionKind kind = TextSelectionKind::Text;
    int pageIndex = -1;

    /// False when the page has no text layer. `unavailableReason` then says so.
    bool available = false;
    QString unavailableReason;

    QString text;
    QList<QRectF> geometry;

    /// Reading-order line range of the selection, or -1 when the selection does
    /// not address lines. Stepping by line moves this range.
    int firstLine = -1;
    int lastLine = -1;

    RevisionFencedToken fence;

    bool isValid() const { return pageIndex >= 0 && fence.isValid(); }
    bool isEmpty() const { return text.isEmpty() && geometry.isEmpty(); }
    bool operator==(const TextSelection& other) const = default;
};

/// A cheap, per-event, revision-fenced snapshot of what the pointer is doing.
///
/// The class exists to keep direct manipulation off the document. Hover,
/// selection, drag preview and marquee all mutate this and nothing else; one
/// semantic operation is submitted through P4-S2's CommandCatalog when a drag
/// completes, which stays the only mutation route. Nothing here is serialized
/// into a PDF, and no method on it can reach pdf::PDFDocument.
///
/// The fence is pdfinteraction::RevisionFencedToken from pagesurfacekey.h,
/// reused rather than redeclared as P4-S3 requires. Both halves matter: the
/// revision says which document state the gesture was begun against, and the
/// generation distinguishes two gestures begun against the same one. A drag that
/// began before a background save cannot complete afterwards; it is cancelled
/// with RevisionChanged and its preview is dropped (issue #141 AC5).
class InteractionState final
{
public:
    InteractionState() = default;

    InteractionState(const InteractionState&) = delete;
    InteractionState& operator=(const InteractionState&) = delete;

    /// Pointer movement below this many viewport pixels is not yet a drag.
    static constexpr int DefaultDragThresholdPx = 4;

    void setDragThresholdPx(int pixels);
    int dragThresholdPx() const noexcept { return m_dragThresholdPx; }

    InteractionKind kind() const noexcept { return m_kind; }
    bool isActive() const noexcept { return m_kind != InteractionKind::None && m_token.isValid(); }
    bool isActive(InteractionKind kind) const noexcept { return m_kind == kind && m_token.isValid(); }

    RevisionFencedToken token() const noexcept { return m_token; }
    InteractionCancelReason lastCancelReason() const noexcept { return m_lastCancelReason; }

    /// Latest pointer position, in viewport pixels and in page space. The page
    /// point is absent when the pointer is not over a page.
    QPoint pointerPx() const noexcept { return m_pointerPx; }
    const std::optional<QPointF>& pointerPagePoint() const noexcept { return m_pointerPagePoint; }
    int pointerPageIndex() const noexcept { return m_pointerPageIndex; }

    const InteractionTarget& hovered() const noexcept { return m_hovered; }
    const InteractionTarget& selected() const noexcept { return m_selected; }
    const std::optional<DragSession>& drag() const noexcept { return m_drag; }

    /// The measurement being dragged out, if any. Never a DragSession.
    const std::optional<Measurement>& measurementDraft() const noexcept { return m_measurementDraft; }

    /// Completed measurements, in the order they were taken. Cleared by
    /// clearMeasurements() and by a revision change, never carried onto new
    /// geometry.
    const QList<Measurement>& measurements() const noexcept { return m_measurements; }

    /// The typed text selection, or nothing when no page has one.
    const std::optional<TextSelection>& textSelection() const noexcept { return m_textSelection; }

    /// The target that owns pointer capture, if any. A captured pointer keeps
    /// delivering to the drag even once it leaves the target's bounds.
    bool hasPointerCapture() const noexcept { return m_pointerCapture; }

    void setPointerPosition(QPoint viewportPx, int pageIndex, std::optional<QPointF> pagePoint);

    /// Returns true when the hovered target changed, which is the only thing the
    /// caller needs in order to decide whether to rebuild the overlay frame.
    bool setHovered(const InteractionTarget& target);

    /// Returns true when the selection changed. Changing the selection cancels a
    /// drag rather than silently retargeting it.
    bool setSelected(const InteractionTarget& target);

    /// Begins a transient interaction fenced to `token`. Any active interaction
    /// is cancelled first with `reason`. Returns false for an invalid token: a
    /// gesture on a closed document is refused, not begun and immediately lost.
    bool begin(InteractionKind kind, const RevisionFencedToken& token, InteractionCancelReason reason = InteractionCancelReason::Explicit);

    /// Begins a drag on `target`, capturing the grab offset from `pagePoint`.
    bool beginDrag(const InteractionTarget& target,
                   const RevisionFencedToken& token,
                   QPoint originPx,
                   QPointF originPagePoint,
                   Qt::MouseButton button,
                   Qt::KeyboardModifiers modifiers);

    /// Advances the active drag. Returns false when the drag is not active or
    /// `token` no longer matches, which is the revision-conflict path.
    ///
    /// Modifiers are re-read on every move rather than kept from the press:
    /// pressing Shift mid-drag is how constrained movement is expressed, and a
    /// session that remembers only the press state cannot see it.
    bool updateDrag(const RevisionFencedToken& token, QPoint currentPx, QPointF currentPagePoint, Qt::KeyboardModifiers modifiers);

    /// Ends the active drag and returns it, or nothing when the token no longer
    /// matches or the pointer never passed the hysteresis threshold. A returned
    /// session is what a caller turns into exactly one command invocation.
    std::optional<DragSession> completeDrag(const RevisionFencedToken& token);

    /// Sets the in-progress measurement. Refuses a stale token or an invalid
    /// measurement, so a measurement can never be steered onto a revision it
    /// was not taken against.
    bool setMeasurementDraft(const Measurement& draft, const RevisionFencedToken& token);

    /// Ends the in-progress measurement, stores it, and returns it. Nothing is
    /// returned for a stale token or when there is no draft; a stale draft is
    /// dropped rather than completed.
    std::optional<Measurement> completeMeasurement(const RevisionFencedToken& token);

    /// Stores a text selection. Refuses a stale token or a selection without a
    /// page, so a selection always names the page it came from.
    bool setTextSelection(const TextSelection& selection, const RevisionFencedToken& token);

    /// Drops the text selection. Returns true when there was one.
    bool clearTextSelection();

    /// Drops every measurement, draft and completed. Returns true when there
    /// was something to drop.
    bool clearMeasurements();

    /// Drops the state derived from the document revision: the text selection
    /// and every measurement. Returns true when something was dropped. A
    /// revision change calls this instead of carrying a derived fact forward.
    bool clearRevisionBoundState();

    /// Ends the active interaction. Idempotent; always leaves no preview behind.
    void cancel(InteractionCancelReason reason);

    /// Clears hover without disturbing a drag or a tool gesture. Returns true
    /// when something was cleared.
    bool clearHover();

    /// Drops everything, including the selection, the text selection and every
    /// measurement. For a document replacement.
    void reset(InteractionCancelReason reason);

    /// Whether `token` still addresses the active interaction.
    bool isCurrent(const RevisionFencedToken& token) const;

private:
    void clearTransient();

    InteractionKind m_kind = InteractionKind::None;
    InteractionCancelReason m_lastCancelReason = InteractionCancelReason::None;
    RevisionFencedToken m_token;

    QPoint m_pointerPx;
    std::optional<QPointF> m_pointerPagePoint;
    int m_pointerPageIndex = -1;

    InteractionTarget m_hovered;
    InteractionTarget m_selected;
    std::optional<DragSession> m_drag;
    std::optional<Measurement> m_measurementDraft;
    QList<Measurement> m_measurements;
    std::optional<TextSelection> m_textSelection;
    bool m_pointerCapture = false;

    int m_dragThresholdPx = DefaultDragThresholdPx;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::DragSession)
Q_DECLARE_METATYPE(pdfinteraction::InteractionKind)
Q_DECLARE_METATYPE(pdfinteraction::InteractionCancelReason)
Q_DECLARE_METATYPE(pdfinteraction::Measurement)
Q_DECLARE_METATYPE(pdfinteraction::TextSelection)
Q_DECLARE_METATYPE(pdfinteraction::TextSelectionKind)

#endif   // INTERACTIONSTATE_H
