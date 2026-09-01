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

#include "interactionstate.h"

#include <QtMath>

#include <cmath>

namespace pdfinteraction
{

const char* getInteractionKindName(InteractionKind kind)
{
    switch (kind)
    {
        case InteractionKind::None:
            return "none";
        case InteractionKind::Hover:
            return "hover";
        case InteractionKind::Drag:
            return "drag";
        case InteractionKind::Marquee:
            return "marquee";
        case InteractionKind::Pan:
            return "pan";
        case InteractionKind::ToolGesture:
            return "tool-gesture";
    }

    return "unknown";
}

const char* getInteractionCancelReasonName(InteractionCancelReason reason)
{
    switch (reason)
    {
        case InteractionCancelReason::None:
            return "none";
        case InteractionCancelReason::Explicit:
            return "explicit";
        case InteractionCancelReason::Escape:
            return "escape";
        case InteractionCancelReason::PointerCancelled:
            return "pointer-cancelled";
        case InteractionCancelReason::FocusLost:
            return "focus-lost";
        case InteractionCancelReason::CaptureLost:
            return "capture-lost";
        case InteractionCancelReason::ToolChanged:
            return "tool-changed";
        case InteractionCancelReason::SelectionChanged:
            return "selection-changed";
        case InteractionCancelReason::RevisionChanged:
            return "revision-changed";
        case InteractionCancelReason::DocumentClosed:
            return "document-closed";
    }

    return "unknown";
}

void InteractionState::setDragThresholdPx(int pixels)
{
    m_dragThresholdPx = qMax(0, pixels);
}

void InteractionState::setPointerPosition(QPoint viewportPx, int pageIndex, std::optional<QPointF> pagePoint)
{
    m_pointerPx = viewportPx;
    m_pointerPageIndex = pageIndex;
    m_pointerPagePoint = pagePoint;
}

bool InteractionState::setHovered(const InteractionTarget& target)
{
    if (m_hovered == target)
    {
        return false;
    }

    m_hovered = target;
    return true;
}

bool InteractionState::setSelected(const InteractionTarget& target)
{
    if (m_selected == target)
    {
        return false;
    }

    // A drag belongs to the object it grabbed. Retargeting it at a new selection
    // would let a completed drag move something the user never pressed on.
    if (m_drag.has_value())
    {
        cancel(InteractionCancelReason::SelectionChanged);
    }

    m_selected = target;
    return true;
}

bool InteractionState::begin(InteractionKind kind, const RevisionFencedToken& token, InteractionCancelReason reason)
{
    if (kind == InteractionKind::None || !token.isValid())
    {
        return false;
    }

    if (isActive())
    {
        cancel(reason);
    }

    m_kind = kind;
    m_token = token;
    m_lastCancelReason = InteractionCancelReason::None;
    return true;
}

bool InteractionState::beginDrag(const InteractionTarget& target,
                                 const RevisionFencedToken& token,
                                 QPoint originPx,
                                 QPointF originPagePoint,
                                 Qt::MouseButton button,
                                 Qt::KeyboardModifiers modifiers)
{
    if (!target.isValid() || !begin(InteractionKind::Drag, token))
    {
        return false;
    }

    DragSession session;
    session.target = target;
    session.originPx = originPx;
    session.originPagePoint = originPagePoint;

    // The grab offset is captured once, from the press, and never recomputed.
    // Recomputing it from the current pointer is exactly the bug where an object
    // snaps its corner to the cursor on the first move.
    session.grabOffset = target.pageBounds.isNull() ? QPointF() : originPagePoint - target.pageBounds.topLeft();

    session.currentPx = originPx;
    session.button = button;
    session.modifiers = modifiers;
    session.previewPageBounds = target.pageBounds;

    m_drag = session;
    m_pointerCapture = true;
    return true;
}

bool InteractionState::updateDrag(const RevisionFencedToken& token, QPoint currentPx, QPointF currentPagePoint, Qt::KeyboardModifiers modifiers)
{
    if (!m_drag.has_value() || !isCurrent(token))
    {
        return false;
    }

    DragSession& session = *m_drag;
    session.currentPx = currentPx;
    session.modifiers = modifiers;
    session.pageDelta = currentPagePoint - session.originPagePoint;

    if (!session.exceededThreshold)
    {
        const QPoint travel = currentPx - session.originPx;
        const qreal distance = std::hypot(qreal(travel.x()), qreal(travel.y()));
        session.exceededThreshold = distance > qreal(m_dragThresholdPx);
    }

    if (session.exceededThreshold && !session.target.pageBounds.isNull())
    {
        // The preview follows the grab offset, so the point the user pressed
        // stays under the pointer for the whole drag.
        const QPointF topLeft = currentPagePoint - session.grabOffset;
        session.previewPageBounds = QRectF(topLeft, session.target.pageBounds.size());
    }

    // Every move starts unsnapped. A snap decided on the previous move must not
    // survive into this one: the pointer may have left the candidate, and a
    // sticky flag would report a latch the preview no longer shows.
    session.snappedTo.clear();

    return true;
}

bool InteractionState::setDragPreviewOrigin(const RevisionFencedToken& token,
                                            QPointF topLeft,
                                            const QString& snappedTo)
{
    if (!m_drag.has_value() || !isCurrent(token))
    {
        return false;
    }

    DragSession& session = *m_drag;

    if (!session.exceededThreshold || session.target.pageBounds.isNull())
    {
        return false;
    }

    session.previewPageBounds = QRectF(topLeft, session.target.pageBounds.size());
    session.snappedTo = snappedTo;
    return true;
}

std::optional<DragSession> InteractionState::completeDrag(const RevisionFencedToken& token)
{
    if (!m_drag.has_value() || !isCurrent(token))
    {
        // A stale completion is a cancel, never a commit against a document
        // state the gesture was not steering.
        if (m_drag.has_value())
        {
            cancel(InteractionCancelReason::RevisionChanged);
        }

        return std::nullopt;
    }

    const DragSession session = *m_drag;
    clearTransient();

    if (!session.exceededThreshold)
    {
        // A press and release inside the threshold is a click. Returning it as a
        // drag would submit a zero-length transform as a document operation.
        return std::nullopt;
    }

    return session;
}

void InteractionState::cancel(InteractionCancelReason reason)
{
    if (!isActive() && !m_drag.has_value())
    {
        return;
    }

    clearTransient();
    m_lastCancelReason = reason;
}

bool InteractionState::clearHover()
{
    if (!m_hovered.isValid())
    {
        return false;
    }

    m_hovered = InteractionTarget();

    if (m_kind == InteractionKind::Hover)
    {
        m_kind = InteractionKind::None;
        m_token = RevisionFencedToken();
    }

    return true;
}

void InteractionState::reset(InteractionCancelReason reason)
{
    cancel(reason);
    m_lastCancelReason = reason;
    m_hovered = InteractionTarget();
    m_selected = InteractionTarget();
    m_pointerPagePoint.reset();
    m_pointerPageIndex = -1;
}

bool InteractionState::isCurrent(const RevisionFencedToken& token) const
{
    return isActive() && m_token == token;
}

void InteractionState::clearTransient()
{
    m_kind = InteractionKind::None;
    m_token = RevisionFencedToken();
    m_drag.reset();
    m_pointerCapture = false;
}

}   // namespace pdfinteraction
