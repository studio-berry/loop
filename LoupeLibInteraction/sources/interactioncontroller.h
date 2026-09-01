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

#ifndef INTERACTIONCONTROLLER_H
#define INTERACTIONCONTROLLER_H

#include "documentcontextsource.h"
#include "hittestsource.h"
#include "inputintent.h"
#include "interactionglobal.h"
#include "dragsnapper.h"
#include "interactionstate.h"
#include "interactiontrace.h"
#include "overlaybuilder.h"
#include "overlayframe.h"
#include "viewportcontroller.h"

#include <QList>
#include <QObject>

namespace pdfinteraction
{

/// Turns host input into transient state, viewport motion and overlay frames.
///
/// This class is where the P4-S4 exit condition is enforced. Everything a
/// pointer does that is not a viewport change resolves to a new OverlayFrame and
/// nothing else: no document mutation, no cache invalidation, and no
/// PageSurfaceRequest. It never touches pdf::PDFDocument, never calls
/// PageSurfaceCoordinator, and holds no pdf::PDFDocumentSession.
///
/// It also does not commit. A completed drag is emitted as a DragSession for the
/// owner to route through P4-S2's CommandCatalog, which stays the only mutation
/// path; a controller that invoked commands itself would be a second one.
///
/// Two signals separate the two costs, and the separation is the contract:
///
///   overlayFrameChanged  -- rebuild the overlay nodes; page pixels are still
///                           valid.
///   viewportChanged      -- the host should ask PageSurfaceCoordinator for
///                           surfaces. Whether that actually renders anything is
///                           the viewport's requestGeneration() decision, not
///                           this class's: a pan moves what is wanted, a zoom
///                           changes what a wanted page should look like.
class InteractionController final : public QObject
{
    Q_OBJECT

public:
    InteractionController(IDocumentRevisionSource& revisions,
                          ViewportController& viewport,
                          HitTestDispatcher& hitTest,
                          OverlayBuilder& overlays,
                          QObject* parent = nullptr);

    ~InteractionController() override;

    InteractionController(const InteractionController&) = delete;
    InteractionController& operator=(const InteractionController&) = delete;

    /// Qt reports a wheel notch as 120 eighths of a degree
    /// (QWheelEvent::DefaultDeltasPerStep). Spelled out rather than included,
    /// because QWheelEvent lives in Qt6::Gui's event headers and this layer takes
    /// values, not events. The host-independent command contract uses the same
    /// number for every presentation host.
    static constexpr int WheelDeltasPerStep = 120;

    void setZoomModifier(Qt::KeyboardModifier modifier);
    Qt::KeyboardModifier zoomModifier() const noexcept { return m_zoomModifier; }

    void setPanButton(Qt::MouseButton button);
    Qt::MouseButton panButton() const noexcept { return m_panButton; }

    /// Pixels an arrow key scrolls.
    void setKeyScrollStepPx(int pixels);

    /// Optional. When set, every intent, stage duration and frame is recorded.
    /// Absent by default: a diagnostic that is on in production is a tax.
    void setTraceRecorder(InteractionTraceRecorder* recorder);
    InteractionTraceRecorder* traceRecorder() const noexcept { return m_trace; }

    /// Optional. Observed, never owned. Absent by default, and absent means
    /// exactly the behaviour before issue #145: the preview goes wherever the
    /// pointer puts it.
    void setSnapper(DragSnapper* snapper);
    DragSnapper* snapper() const noexcept { return m_snapper; }

    /// Held down, this modifier suppresses snapping for as long as it is held.
    ///
    /// Sampled from the current intent on every move rather than latched at
    /// press: issue #145 AC3 asks for modifiers to be sampled consistently, and
    /// a user who starts a drag and then decides they want the exact position
    /// is asking for the snap to stop, not for the gesture to restart.
    void setSnapSuppressModifier(Qt::KeyboardModifier modifier);
    Qt::KeyboardModifier snapSuppressModifier() const noexcept { return m_snapSuppressModifier; }

    const InteractionState& state() const noexcept { return m_state; }
    const OverlayFrame& overlayFrame() const noexcept { return m_overlay; }

    /// The fence every transient gesture and overlay frame is stamped with. Its
    /// generation is this controller's own, not the viewport's: a zoom during a
    /// drag supersedes page surfaces but must not cancel the gesture the user is
    /// still steering.
    RevisionFencedToken token() const;

    /// The tool a gesture belongs to. Changing it cancels an active drag, which
    /// is issue #141 AC3 -- a tool change must not leave a half-applied
    /// transform behind.
    void setActiveTool(const QString& toolId);
    QString activeTool() const { return m_activeTool; }

    void handlePointer(const PointerIntent& intent);
    void handleWheel(const WheelIntent& intent);
    void handleKey(const KeyIntent& intent);
    void handleHostNotification(HostNotification notification);

    /// Selects a target without a pointer, for keyboard navigation and for
    /// P4-S8's evidence navigation.
    void selectTarget(const InteractionTarget& target);

    /// A new document state. Cancels the active gesture, clears the selection,
    /// advances the interaction generation, and publishes an empty frame. The
    /// counterpart of PageSurfaceCoordinator::invalidate.
    void invalidate();

    /// Rebuilds the overlay frame from current state. Called automatically by
    /// the intent handlers; public so a caller that changed a builder input
    /// (findings arrived, a marker was hidden) can refresh without faking input.
    void refreshOverlay();

    /// Replays a recorded trace in order. The controller is left in exactly the
    /// state the original session ended in, provided the viewport, hit-test
    /// sources and document state match -- which is what makes a recorded
    /// interaction bug reproducible.
    ///
    /// Recording is suppressed during replay, so replaying a trace into a
    /// controller that has a recorder attached does not append the trace to
    /// itself.
    void replay(const InteractionTrace& trace);

signals:
    /// Overlay-only change. Page pixels are untouched.
    void overlayFrameChanged();

    /// The viewport moved or rescaled. The host asks the coordinator for
    /// surfaces; the viewport's own generation decides whether anything is
    /// actually re-rendered.
    void viewportChanged();

    void selectionChanged(pdfinteraction::InteractionTarget target);
    void hoverChanged(pdfinteraction::InteractionTarget target);

    /// Exactly one per completed drag, and never for a click below the
    /// hysteresis threshold. The owner turns it into one command invocation.
    void dragCompleted(pdfinteraction::DragSession session);

    void interactionCancelled(pdfinteraction::InteractionCancelReason reason);

private:
    /// A stage timed against the trace clock, if there is one. Costs nothing
    /// when tracing is off.
    ///
    /// Nested time is subtracted, so the stages are exclusive and sum to at most
    /// the frame. Inclusive stages would make the outermost one dominate every
    /// frame, and slow-frame attribution would answer "interaction" for
    /// everything -- an attribution that is always the same answer is not one.
    class StageTimer
    {
    public:
        StageTimer(InteractionController* controller, TraceStage stage);
        ~StageTimer();

        StageTimer(const StageTimer&) = delete;
        StageTimer& operator=(const StageTimer&) = delete;

    private:
        InteractionController* m_controller = nullptr;
        TraceStage m_stage = TraceStage::Interaction;
        qint64 m_startNs = 0;
        bool m_active = false;
    };

    /// Cancels the active gesture when the document state moved under it.
    /// Returns true when the fence still holds.
    bool checkFence();

    void handlePointerPress(const PointerIntent& intent);
    void handlePointerMove(const PointerIntent& intent);
    void handlePointerRelease(const PointerIntent& intent);

    InteractionTarget hitTestAt(QPoint viewportPx, int* pageIndex, QPointF* pagePoint) const;

    void cancelActive(InteractionCancelReason reason);

    /// Pulls the live drag preview onto a snap candidate, if there is one and
    /// the user is not suppressing it.
    void applySnapToPreview(Qt::KeyboardModifiers modifiers);
    void publishOverlay();

    IDocumentRevisionSource* m_revisions = nullptr;
    ViewportController* m_viewport = nullptr;
    HitTestDispatcher* m_hitTest = nullptr;
    OverlayBuilder* m_overlays = nullptr;
    InteractionTraceRecorder* m_trace = nullptr;
    DragSnapper* m_snapper = nullptr;

    InteractionState m_state;
    OverlayFrame m_overlay;

    QString m_activeTool;
    Qt::KeyboardModifier m_zoomModifier = Qt::ControlModifier;
    Qt::MouseButton m_panButton = Qt::MiddleButton;
    Qt::KeyboardModifier m_snapSuppressModifier = Qt::AltModifier;
    int m_keyScrollStepPx = 40;

    quint64 m_generation = 1;
    bool m_replaying = false;

    /// Nested-stage accounting for StageTimer. One entry per open stage.
    QList<qint64> m_stageChildNs;

    /// Pointer position at the last pan step, so a pan scrolls by the delta
    /// rather than by the absolute position.
    QPoint m_panAnchorPx;
};

}   // namespace pdfinteraction

#endif   // INTERACTIONCONTROLLER_H
