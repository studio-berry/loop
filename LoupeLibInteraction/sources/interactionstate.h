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
    ToolGesture
};

const char* getInteractionKindName(InteractionKind kind);

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

    /// The snap source the preview latched onto, or empty when the preview is
    /// wherever the pointer put it.
    ///
    /// Recorded rather than re-derived. A commit path that had to work out
    /// whether a preview was snapped, by comparing its bounds against every
    /// candidate again, would be free to reach a different answer than the one
    /// the user watched.
    QString snappedTo;
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

    /// Moves the drag preview onto `topLeft` and records what it snapped to.
    ///
    /// Separate from updateDrag because a snap is a decision about presentation
    /// made after the geometry is known, by something that can see the viewport
    /// -- and this class deliberately cannot. Returns false when there is no
    /// current drag, when the token is stale, or before the drag has passed the
    /// hysteresis threshold.
    bool setDragPreviewOrigin(const RevisionFencedToken& token, QPointF topLeft, const QString& snappedTo);

    /// Ends the active drag and returns it, or nothing when the token no longer
    /// matches or the pointer never passed the hysteresis threshold. A returned
    /// session is what a caller turns into exactly one command invocation.
    std::optional<DragSession> completeDrag(const RevisionFencedToken& token);

    /// Ends the active interaction. Idempotent; always leaves no preview behind.
    void cancel(InteractionCancelReason reason);

    /// Clears hover without disturbing a drag or a tool gesture. Returns true
    /// when something was cleared.
    bool clearHover();

    /// Drops everything, including the selection. For a document replacement.
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
    bool m_pointerCapture = false;

    int m_dragThresholdPx = DefaultDragThresholdPx;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::DragSession)
Q_DECLARE_METATYPE(pdfinteraction::InteractionKind)
Q_DECLARE_METATYPE(pdfinteraction::InteractionCancelReason)

#endif   // INTERACTIONSTATE_H
