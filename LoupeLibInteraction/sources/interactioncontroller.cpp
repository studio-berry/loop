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

#include "interactioncontroller.h"

#include <QtMath>

namespace pdfinteraction
{

InteractionController::StageTimer::StageTimer(InteractionController* controller, TraceStage stage) :
    m_controller(controller),
    m_stage(stage),
    m_active(controller && controller->m_trace)
{
    if (!m_active)
    {
        return;
    }

    m_startNs = m_controller->m_trace->nowNs();
    m_controller->m_stageChildNs.push_back(0);
}

InteractionController::StageTimer::~StageTimer()
{
    if (!m_active)
    {
        return;
    }

    const qint64 elapsedNs = m_controller->m_trace->nowNs() - m_startNs;
    const qint64 childNs = m_controller->m_stageChildNs.takeLast();
    m_controller->m_trace->recordStage(m_stage, qMax<qint64>(0, elapsedNs - childNs));

    if (!m_controller->m_stageChildNs.isEmpty())
    {
        m_controller->m_stageChildNs.last() += elapsedNs;
    }
}

InteractionController::InteractionController(IDocumentRevisionSource& revisions,
                                             ViewportController& viewport,
                                             HitTestDispatcher& hitTest,
                                             OverlayBuilder& overlays,
                                             QObject* parent) :
    QObject(parent),
    m_revisions(&revisions),
    m_viewport(&viewport),
    m_hitTest(&hitTest),
    m_overlays(&overlays)
{
}

InteractionController::~InteractionController() = default;

void InteractionController::setZoomModifier(Qt::KeyboardModifier modifier)
{
    m_zoomModifier = modifier;
}

void InteractionController::setPanButton(Qt::MouseButton button)
{
    m_panButton = button;
}

void InteractionController::setKeyScrollStepPx(int pixels)
{
    m_keyScrollStepPx = qMax(1, pixels);
}

void InteractionController::setTraceRecorder(InteractionTraceRecorder* recorder)
{
    m_trace = recorder;
}

RevisionFencedToken InteractionController::token() const
{
    RevisionFencedToken token;
    token.generation = m_generation;
    token.revision = m_revisions ? m_revisions->currentRevision() : pdf::PDFRevisionIdentity();
    return token;
}

void InteractionController::setActiveTool(const QString& toolId)
{
    if (m_activeTool == toolId)
    {
        return;
    }

    m_activeTool = toolId;
    cancelActive(InteractionCancelReason::ToolChanged);
    publishOverlay();
}

bool InteractionController::checkFence()
{
    if (!m_state.isActive())
    {
        return true;
    }

    if (m_revisions && m_revisions->isCurrent(m_state.token().revision) && m_state.token().generation == m_generation)
    {
        return true;
    }

    // The document moved under an in-flight gesture. Issue #141 AC5: the preview
    // is dropped rather than rebased, because rebasing a transform onto a
    // revision the user never saw is a silent edit.
    cancelActive(InteractionCancelReason::RevisionChanged);
    return false;
}

InteractionTarget InteractionController::hitTestAt(QPoint viewportPx, int* pageIndex, QPointF* pagePoint) const
{
    QPointF localPagePoint;
    const int page = m_viewport->pageUnderPoint(viewportPx, &localPagePoint);

    if (pageIndex)
    {
        *pageIndex = page;
    }

    if (pagePoint)
    {
        *pagePoint = localPagePoint;
    }

    if (page < 0)
    {
        return InteractionTarget();
    }

    return m_hitTest->hitTest(page, localPagePoint);
}

void InteractionController::cancelActive(InteractionCancelReason reason)
{
    if (!m_state.isActive() && !m_state.drag().has_value())
    {
        return;
    }

    m_state.cancel(reason);
    Q_EMIT interactionCancelled(reason);
}

void InteractionController::publishOverlay()
{
    const StageTimer timer(this, TraceStage::Overlay);

    m_hitTest->setSelection(m_state.selected());
    m_overlay = m_overlays->build(m_state, token());
    Q_EMIT overlayFrameChanged();
}

void InteractionController::refreshOverlay()
{
    publishOverlay();
}

void InteractionController::handlePointer(const PointerIntent& intent)
{
    if (m_trace && !m_replaying)
    {
        m_trace->recordPointer(intent);
    }

    const StageTimer timer(this, TraceStage::Interaction);

    checkFence();

    switch (intent.action)
    {
        case PointerAction::Press:
            handlePointerPress(intent);
            break;

        case PointerAction::Move:
            handlePointerMove(intent);
            break;

        case PointerAction::Release:
            handlePointerRelease(intent);
            break;

        case PointerAction::Cancel:
            cancelActive(InteractionCancelReason::PointerCancelled);
            publishOverlay();
            break;

        case PointerAction::Leave:
            // Leaving clears hover but must not end a drag: a drag that ends
            // when the pointer crosses the edge of the view drops the gesture
            // every time someone drags something to the border.
            if (m_state.clearHover())
            {
                Q_EMIT hoverChanged(InteractionTarget());
                publishOverlay();
            }
            break;
    }
}

void InteractionController::handlePointerPress(const PointerIntent& intent)
{
    int pageIndex = -1;
    QPointF pagePoint;

    InteractionTarget target;
    {
        const StageTimer hitTimer(this, TraceStage::HitTest);
        target = hitTestAt(intent.positionPx, &pageIndex, &pagePoint);
    }

    m_state.setPointerPosition(intent.positionPx, pageIndex, pageIndex >= 0 ? std::optional<QPointF>(pagePoint) : std::nullopt);

    if (intent.button == m_panButton)
    {
        m_panAnchorPx = intent.positionPx;
        m_state.begin(InteractionKind::Pan, token());
        return;
    }

    if (intent.button != Qt::LeftButton)
    {
        return;
    }

    const bool selectionDidChange = m_state.setSelected(target);
    if (selectionDidChange)
    {
        Q_EMIT selectionChanged(target);
    }

    // Only a handle or the already-selected object starts a drag. Pressing on an
    // unrelated marker selects it; the next press can then drag it. Selecting
    // and dragging in one gesture is how an accidental click nudges something.
    const bool draggable = target.kind == InteractionTargetKind::DragHandle || (!selectionDidChange && target.isValid() && target.kind != InteractionTargetKind::Page);

    if (draggable && pageIndex >= 0)
    {
        m_state.beginDrag(target, token(), intent.positionPx, pagePoint, intent.button, intent.modifiers);
    }

    publishOverlay();
}

void InteractionController::handlePointerMove(const PointerIntent& intent)
{
    int pageIndex = -1;
    QPointF pagePoint;

    if (m_state.isActive(InteractionKind::Pan))
    {
        const QPoint delta = intent.positionPx - m_panAnchorPx;
        m_panAnchorPx = intent.positionPx;

        // The content follows the pointer -- the same sign PDFDrawWidget's
        // Translate operation uses, so the Quick shell and the Widgets oracle pan
        // in the same direction. Panning deliberately does not advance the
        // viewport's request generation: cancelling every in-flight render on
        // each pointer delta is exactly what issue #142 forbids.
        if (!delta.isNull() && !m_viewport->scrollByPixels(delta).isNull())
        {
            Q_EMIT viewportChanged();
        }

        m_state.setPointerPosition(intent.positionPx, -1, std::nullopt);
        return;
    }

    if (m_state.drag().has_value())
    {
        const int dragPage = m_state.drag()->target.pageIndex;
        const std::optional<QPointF> dragPagePoint = m_viewport->viewportToPagePoint(intent.positionPx, dragPage);

        // A drag stays on the page it began on. Resolving the page under the
        // pointer instead would retarget the gesture the moment it crossed a
        // page boundary.
        if (dragPagePoint.has_value())
        {
            m_state.setPointerPosition(intent.positionPx, dragPage, dragPagePoint);
            m_state.updateDrag(token(), intent.positionPx, *dragPagePoint, intent.modifiers);
        }

        publishOverlay();
        return;
    }

    InteractionTarget target;
    {
        const StageTimer hitTimer(this, TraceStage::HitTest);
        target = hitTestAt(intent.positionPx, &pageIndex, &pagePoint);
    }

    m_state.setPointerPosition(intent.positionPx, pageIndex, pageIndex >= 0 ? std::optional<QPointF>(pagePoint) : std::nullopt);

    if (!m_state.setHovered(target))
    {
        // Same target as last time. There is nothing to redraw, and rebuilding
        // the frame anyway would make a slow pointer sweep cost one frame per
        // pixel.
        return;
    }

    Q_EMIT hoverChanged(target);
    publishOverlay();
}

void InteractionController::handlePointerRelease(const PointerIntent& intent)
{
    if (m_state.isActive(InteractionKind::Pan) && intent.button == m_panButton)
    {
        m_state.cancel(InteractionCancelReason::Explicit);
        return;
    }

    if (!m_state.drag().has_value())
    {
        return;
    }

    const std::optional<DragSession> session = m_state.completeDrag(token());
    if (session.has_value())
    {
        // One semantic operation per completed drag, and the controller does not
        // apply it: the owner routes this through the command catalog, which
        // stays the only mutation path (issue #141 AC2).
        Q_EMIT dragCompleted(*session);
    }

    publishOverlay();
}

void InteractionController::handleWheel(const WheelIntent& intent)
{
    if (m_trace && !m_replaying)
    {
        m_trace->recordWheel(intent);
    }

    const StageTimer timer(this, TraceStage::Interaction);

    checkFence();

    if (intent.modifiers.testFlag(m_zoomModifier))
    {
        const qreal steps = qreal(intent.angleDelta.y()) / qreal(WheelDeltasPerStep);
        if (qFuzzyIsNull(steps))
        {
            return;
        }

        const qreal zoom = m_viewport->zoom() * qPow(ViewportController::ZoomStep, steps);

        // Anchored at the pointer, so the point under the cursor stays put. This
        // does advance the viewport's request generation: a zoom changes what a
        // wanted page should look like, not merely which pages are wanted.
        m_viewport->setZoom(zoom, QPointF(intent.positionPx));
        Q_EMIT viewportChanged();
        return;
    }

    const QPoint pixelDelta = !intent.pixelDelta.isNull() ? intent.pixelDelta : intent.angleDelta * m_keyScrollStepPx / WheelDeltasPerStep;
    if (!pixelDelta.isNull() && !m_viewport->scrollByPixels(pixelDelta).isNull())
    {
        Q_EMIT viewportChanged();
    }
}

void InteractionController::handleKey(const KeyIntent& intent)
{
    if (m_trace && !m_replaying)
    {
        m_trace->recordKey(intent);
    }

    const StageTimer timer(this, TraceStage::Interaction);

    checkFence();

    if (intent.action != KeyAction::Press)
    {
        return;
    }

    switch (intent.key)
    {
        case Qt::Key_Escape:
            if (m_state.isActive() || m_state.drag().has_value())
            {
                cancelActive(InteractionCancelReason::Escape);
                publishOverlay();
            }
            return;

        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        {
            // The offset is the content's top-left in viewport pixels, so moving
            // the view down means moving the content up. Pressing Down therefore
            // decreases the offset, not increases it.
            const QPoint step = intent.key == Qt::Key_Left    ? QPoint(m_keyScrollStepPx, 0)
                                : intent.key == Qt::Key_Right ? QPoint(-m_keyScrollStepPx, 0)
                                : intent.key == Qt::Key_Up    ? QPoint(0, m_keyScrollStepPx)
                                                              : QPoint(0, -m_keyScrollStepPx);

            if (!m_viewport->scrollByPixels(step).isNull())
            {
                Q_EMIT viewportChanged();
            }

            return;
        }

        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        {
            // Block direction and pixel direction have opposite signs: the next
            // block is a higher index, and the content moving up is a lower
            // offset.
            const int blockDirection = intent.key == Qt::Key_PageUp ? -1 : 1;

            if (m_viewport->isBlockMode())
            {
                m_viewport->setBlockIndex(m_viewport->blockIndex() + blockDirection);
                Q_EMIT viewportChanged();
                return;
            }

            const int step = qMax(1, m_viewport->viewportSizePx().height()) * -blockDirection;
            if (!m_viewport->scrollByPixels(QPoint(0, step)).isNull())
            {
                Q_EMIT viewportChanged();
            }

            return;
        }

        default:
            // Everything else is a command, and commands are the catalog's. A
            // shortcut table here would be the second action registry P4-S2
            // exists to prevent.
            return;
    }
}

void InteractionController::handleHostNotification(HostNotification notification)
{
    if (m_trace && !m_replaying)
    {
        m_trace->recordNotification(notification);
    }

    const StageTimer timer(this, TraceStage::Interaction);

    const InteractionCancelReason reason = notification == HostNotification::CaptureLost ? InteractionCancelReason::CaptureLost
                                                                                         : InteractionCancelReason::FocusLost;

    const bool wasActive = m_state.isActive() || m_state.drag().has_value();
    cancelActive(reason);

    const bool hoverCleared = m_state.clearHover();
    if (hoverCleared)
    {
        Q_EMIT hoverChanged(InteractionTarget());
    }

    if (wasActive || hoverCleared)
    {
        publishOverlay();
    }
}

void InteractionController::selectTarget(const InteractionTarget& target)
{
    if (!m_state.setSelected(target))
    {
        return;
    }

    Q_EMIT selectionChanged(target);
    publishOverlay();
}

void InteractionController::invalidate()
{
    cancelActive(InteractionCancelReason::DocumentClosed);

    ++m_generation;
    m_state.reset(InteractionCancelReason::DocumentClosed);
    m_hitTest->setHandles(QList<InteractionTarget>());

    publishOverlay();
}

void InteractionController::replay(const InteractionTrace& trace)
{
    m_replaying = true;

    for (const TraceInputRecord& record : trace.inputs)
    {
        if (record.pointer.has_value())
        {
            handlePointer(*record.pointer);
        }
        else if (record.wheel.has_value())
        {
            handleWheel(*record.wheel);
        }
        else if (record.key.has_value())
        {
            handleKey(*record.key);
        }
        else if (record.notification.has_value())
        {
            handleHostNotification(*record.notification);
        }
    }

    m_replaying = false;
}

}   // namespace pdfinteraction
