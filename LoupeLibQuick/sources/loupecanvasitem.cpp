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


#include "loupecanvasitem.h"

#include "interactioncontroller.h"
#include "pagesurfacecoordinator.h"
#include "viewportcontroller.h"

#include <QFocusEvent>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGSimpleRectNode>
#include <QScreen>
#include <QWheelEvent>

namespace pdfquick
{

using pdfinteraction::CanvasSnapshot;
using pdfinteraction::HostNotification;
using pdfinteraction::InputStamp;
using pdfinteraction::InteractionController;
using pdfinteraction::KeyAction;
using pdfinteraction::KeyIntent;
using pdfinteraction::OverlayFrame;
using pdfinteraction::PageSurfaceCoordinator;
using pdfinteraction::PointerAction;
using pdfinteraction::PointerIntent;
using pdfinteraction::ViewportController;
using pdfinteraction::WheelIntent;

namespace
{

/// Root child order. The scene graph paints children in order, so this is the
/// z-order: background, page pixels, overlays, developer panel. It is fixed
/// here rather than chosen per frame for the same reason OverlayLayer's bands
/// are fixed -- a composite order that depends on what happened to be built
/// first is not an order.
enum RootChild
{
    BackgroundChild = 0,
    TilesChild = 1,
    OverlaysChild = 2,
    HudChild = 3
};

/// docs/quick-design-tokens.json, spacing.values_px.
constexpr qreal HudMarginPx = 12.0;

}   // namespace

LoupeCanvasItem::LoupeCanvasItem(QQuickItem* parent) :
    QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setFlag(ItemIsFocusScope, true);

    // Every button, because which one pans is InteractionController's decision
    // (panButton()), not this item's. Filtering here would silently override it.
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(true);

    m_present.setClock(&m_clock);

    connect(&m_present, &CanvasPresentMetrics::framePresented, this, [this]()
            { m_framePending = false; });
}

LoupeCanvasItem::~LoupeCanvasItem()
{
    for (const QMetaObject::Connection& connection : m_connections)
    {
        QObject::disconnect(connection);
    }
    m_connections.clear();
}

void LoupeCanvasItem::bind(ViewportController* viewport, InteractionController* interaction, PageSurfaceCoordinator* surfaces)
{
    for (const QMetaObject::Connection& connection : m_connections)
    {
        QObject::disconnect(connection);
    }
    m_connections.clear();

    m_viewport = viewport;
    m_interaction = interaction;
    m_surfaces = surfaces;

    m_builderResetPending = true;
    m_tilesDirty = true;
    m_overlaysDirty = true;

    if (m_interaction)
    {
        m_connections.append(connect(m_interaction, &InteractionController::overlayFrameChanged, this, &LoupeCanvasItem::onOverlayFrameChanged));
        m_connections.append(connect(m_interaction, &InteractionController::viewportChanged, this, &LoupeCanvasItem::onViewportChanged));
    }

    if (m_viewport)
    {
        // Both viewport signals reach the same handler, but they mean different
        // things and the coordinator cares: demandChanged supersedes prior
        // surface demand, placementsChanged does not. requestSurfaces() is
        // idempotent, so calling it for a pan costs nothing and calling it for a
        // zoom is required.
        m_connections.append(connect(m_viewport, &ViewportController::demandChanged, this, &LoupeCanvasItem::onViewportChanged));
        m_connections.append(connect(m_viewport, &ViewportController::placementsChanged, this, &LoupeCanvasItem::onViewportChanged));
    }

    if (m_surfaces)
    {
        m_connections.append(connect(m_surfaces, &PageSurfaceCoordinator::snapshotChanged, this, &LoupeCanvasItem::onSnapshotChanged));
    }

    publishViewportGeometry();
    requestFrame();

    emit zoomChanged();
    emit currentPageChanged();
    emit blockCountChanged();
    emit activeToolChanged();
}

void LoupeCanvasItem::setTraceRecorder(pdfinteraction::InteractionTraceRecorder* recorder)
{
    m_recorder = recorder;
    m_present.setRecorder(recorder);

    if (m_interaction)
    {
        m_interaction->setTraceRecorder(recorder);
    }

    // Re-publishing the refresh rate: the recorder may have been attached after
    // the window was, and an unknown rate reports as unavailable rather than 60.
    m_present.attach(window());
}

qreal LoupeCanvasItem::zoom() const
{
    return m_viewport ? m_viewport->zoom() : 1.0;
}

void LoupeCanvasItem::setZoom(qreal zoom)
{
    if (!m_viewport || qFuzzyCompare(m_viewport->zoom(), zoom))
    {
        return;
    }

    // Anchored on the item's centre, which is what a zoom control means. A
    // pointer-anchored zoom comes from the wheel path, inside the controller.
    m_viewport->setZoom(zoom, QPointF(width() / 2.0, height() / 2.0) * (window() ? window()->effectiveDevicePixelRatio() : 1.0));
    emit zoomChanged();
}

int LoupeCanvasItem::currentPage() const
{
    return m_viewport ? m_viewport->currentPage() : -1;
}

int LoupeCanvasItem::blockCount() const
{
    return m_viewport ? m_viewport->blockCount() : 0;
}

QString LoupeCanvasItem::activeTool() const
{
    return m_interaction ? m_interaction->activeTool() : QString();
}

void LoupeCanvasItem::setActiveTool(const QString& toolId)
{
    if (!m_interaction || m_interaction->activeTool() == toolId)
    {
        return;
    }

    // Changing the tool cancels an active drag inside the controller (#141 AC3).
    // That is the controller's rule, not a courtesy this item performs first.
    m_interaction->setActiveTool(toolId);
    emit activeToolChanged();
}

void LoupeCanvasItem::setTraceOverlayVisible(bool visible)
{
    if (m_traceOverlayVisible == visible)
    {
        return;
    }

    m_traceOverlayVisible = visible;
    emit traceOverlayVisibleChanged();
    requestFrame();
}

void LoupeCanvasItem::setHighContrast(bool highContrast)
{
    if (m_highContrast == highContrast)
    {
        return;
    }

    m_highContrast = highContrast;
    m_paletteDirty = true;
    emit highContrastChanged();
    requestFrame();
}

void LoupeCanvasItem::refreshPalette()
{
    m_palette = m_highContrast ? CanvasPalette::highContrast() : CanvasPalette::standard();
    m_builder.setPalette(m_palette);
    m_paletteDirty = false;
    m_overlaysDirty = true;
}

CanvasFrameStats LoupeCanvasItem::frameStats() const
{
    CanvasFrameStats stats;
    stats.tiles = m_builder.tileCount();
    stats.inexactTiles = m_builder.inexactTileCount();
    stats.overlayPrimitives = m_lastOverlayPrimitives;
    stats.skippedPrimitives = m_builder.skippedPrimitives();
    stats.droppedPrimitives = m_lastDroppedPrimitives;
    stats.unrenderablePrimitives = m_lastUnrenderablePrimitives;
    return stats;
}

InputStamp LoupeCanvasItem::nextStamp()
{
    InputStamp stamp;
    stamp.monotonicNs = m_clock.nowNs();
    stamp.sequence = ++m_inputSequence;
    return stamp;
}

QPoint LoupeCanvasItem::toViewportPx(const QPointF& itemPosition) const
{
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    return QPoint(qRound(itemPosition.x() * ratio), qRound(itemPosition.y() * ratio));
}

void LoupeCanvasItem::publishViewportGeometry()
{
    if (!m_viewport)
    {
        return;
    }

    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;

    m_viewport->setDevicePixelRatio(ratio);
    m_viewport->setViewportSizePx(QSize(qRound(width() * ratio), qRound(height() * ratio)));

    // The viewport takes pixels-per-millimetre injected rather than reading a
    // screen itself, which is what makes it correct on a second monitor and
    // testable with no monitor at all. This is the layer that may look.
    if (const QQuickWindow* hostWindow = window())
    {
        if (const QScreen* screen = hostWindow->screen())
        {
            const qreal physicalDotsPerInch = screen->physicalDotsPerInch();
            if (physicalDotsPerInch > 0.0)
            {
                m_viewport->setPixelPerMM(physicalDotsPerInch / 25.4);
            }
        }
    }
}

void LoupeCanvasItem::requestFrame()
{
    if (!m_framePending)
    {
        m_framePending = true;
        m_present.frameRequested();
    }

    update();
}

void LoupeCanvasItem::onOverlayFrameChanged()
{
    // Overlay-only change: the page pixels are still valid, and nothing is asked
    // of the coordinator. This is the P4-S4 exit condition observed from the
    // host side -- a hover must not cost a page rerender.
    m_overlaysDirty = true;
    requestFrame();
}

void LoupeCanvasItem::onViewportChanged()
{
    if (m_surfaces)
    {
        m_surfaces->requestSurfaces();
    }

    m_tilesDirty = true;
    m_overlaysDirty = true;
    requestFrame();

    emit zoomChanged();
    emit currentPageChanged();
    emit blockCountChanged();
}

void LoupeCanvasItem::onSnapshotChanged()
{
    m_tilesDirty = true;
    requestFrame();
}

void LoupeCanvasItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);

    if (newGeometry.size() == oldGeometry.size())
    {
        return;
    }

    publishViewportGeometry();

    if (m_surfaces)
    {
        m_surfaces->requestSurfaces();
    }

    m_tilesDirty = true;
    m_overlaysDirty = true;
    requestFrame();
}

void LoupeCanvasItem::releaseResources()
{
    // Qt is taking the node tree away. The nodes are deleted by the scene graph;
    // only the retention maps that pointed at them have to be dropped, and the
    // next updatePaintNode does that.
    m_builderResetPending = true;
    QQuickItem::releaseResources();
}

void LoupeCanvasItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    if (change == ItemSceneChange)
    {
        // The scene graph and its textures belong to the window, so every
        // retained node is invalid the moment it changes -- a texture outliving
        // its window is a crash rather than a glitch. The reset is deferred
        // rather than done here: this runs on the GUI thread, and the builder is
        // render-thread state. CanvasPresentMetrics is a plain QObject on this
        // thread and is attached directly.
        m_builderResetPending = true;
        m_present.attach(value.window);

        m_tilesDirty = true;
        m_overlaysDirty = true;

        publishViewportGeometry();
    }

    QQuickItem::itemChange(change, value);
}

void LoupeCanvasItem::dispatchPointer(PointerAction action, QMouseEvent* event)
{
    if (!m_interaction)
    {
        return;
    }

    PointerIntent intent;
    intent.stamp = nextStamp();
    intent.action = action;
    intent.positionPx = toViewportPx(event->position());
    intent.button = event->button();
    intent.buttons = event->buttons();
    intent.modifiers = event->modifiers();

    m_interaction->handlePointer(intent);
    event->accept();
}

void LoupeCanvasItem::mousePressEvent(QMouseEvent* event)
{
    // Focus follows a press because keyboard viewport commands (arrows, PageUp)
    // are handled here and are useless to an item that cannot be focused.
    forceActiveFocus(Qt::MouseFocusReason);
    dispatchPointer(PointerAction::Press, event);
}

void LoupeCanvasItem::mouseMoveEvent(QMouseEvent* event)
{
    dispatchPointer(PointerAction::Move, event);
}

void LoupeCanvasItem::mouseReleaseEvent(QMouseEvent* event)
{
    dispatchPointer(PointerAction::Release, event);
}

void LoupeCanvasItem::mouseUngrabEvent()
{
    // The grab was taken away, usually by a Flickable or another filter deciding
    // the gesture was theirs. Reported as CaptureLost, which the controller
    // turns into a cancellation -- never as a Release, because a Release
    // completes a drag and would commit a transform the user stopped steering.
    notifyHost(HostNotification::CaptureLost);
    QQuickItem::mouseUngrabEvent();
}

void LoupeCanvasItem::hoverMoveEvent(QHoverEvent* event)
{
    if (!m_interaction)
    {
        return;
    }

    PointerIntent intent;
    intent.stamp = nextStamp();
    intent.action = PointerAction::Move;
    intent.positionPx = toViewportPx(event->position());
    intent.modifiers = event->modifiers();

    m_interaction->handlePointer(intent);
    event->accept();
}

void LoupeCanvasItem::hoverLeaveEvent(QHoverEvent* event)
{
    if (!m_interaction)
    {
        return;
    }

    PointerIntent intent;
    intent.stamp = nextStamp();
    intent.action = PointerAction::Leave;
    intent.modifiers = event->modifiers();

    m_interaction->handlePointer(intent);
    event->accept();
}

void LoupeCanvasItem::wheelEvent(QWheelEvent* event)
{
    if (!m_interaction)
    {
        event->ignore();
        return;
    }

    WheelIntent intent;
    intent.stamp = nextStamp();
    intent.positionPx = toViewportPx(event->position());

    // Both deltas are forwarded unchanged. A mouse reports notches in eighths of
    // a degree and a trackpad reports pixels continuously; collapsing them into
    // one number here would bake this host's convention into a value the trace
    // replays on every other host.
    intent.angleDelta = event->angleDelta();
    intent.pixelDelta = event->pixelDelta();
    intent.modifiers = event->modifiers();

    m_interaction->handleWheel(intent);
    event->accept();
}

void LoupeCanvasItem::dispatchKey(KeyAction action, QKeyEvent* event)
{
    if (!m_interaction)
    {
        event->ignore();
        return;
    }

    KeyIntent intent;
    intent.stamp = nextStamp();
    intent.action = action;
    intent.key = event->key();
    intent.modifiers = event->modifiers();
    intent.autoRepeat = event->isAutoRepeat();

    // No text(). KeyIntent carries a key code and never text, because a trace
    // taken while someone fills in a form must not contain what they typed
    // (issue #140 AC6).
    m_interaction->handleKey(intent);
    event->accept();
}

void LoupeCanvasItem::keyPressEvent(QKeyEvent* event)
{
    dispatchKey(KeyAction::Press, event);
}

void LoupeCanvasItem::keyReleaseEvent(QKeyEvent* event)
{
    dispatchKey(KeyAction::Release, event);
}

void LoupeCanvasItem::focusOutEvent(QFocusEvent* event)
{
    notifyHost(HostNotification::FocusLost);
    QQuickItem::focusOutEvent(event);
}

void LoupeCanvasItem::notifyHost(HostNotification notification)
{
    if (m_interaction)
    {
        m_interaction->handleHostNotification(notification);
    }
}

QSGNode* LoupeCanvasItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QQuickWindow* hostWindow = window();
    if (!hostWindow || width() <= 0.0 || height() <= 0.0)
    {
        delete oldNode;
        return nullptr;
    }

    if (m_paletteDirty)
    {
        refreshPalette();
    }

    if (m_builderResetPending)
    {
        m_builder.forget();
        m_builderResetPending = false;
    }

    m_builder.setWindow(hostWindow);

    QSGNode* root = oldNode;
    if (!root)
    {
        root = new QSGNode;
        root->appendChildNode(new QSGSimpleRectNode);
        root->appendChildNode(new QSGNode);
        root->appendChildNode(new QSGNode);

        // A fresh root has no retained nodes behind it.
        m_builder.forget();
        m_tilesDirty = true;
        m_overlaysDirty = true;
    }

    auto* background = static_cast<QSGSimpleRectNode*>(root->childAtIndex(BackgroundChild));
    background->setRect(boundingRect());
    background->setColor(m_palette.canvasBackground());

    const qreal ratio = hostWindow->effectiveDevicePixelRatio();
    const qreal pixelScale = ratio > 0.0 ? 1.0 / ratio : 1.0;

    if (m_tilesDirty && m_surfaces)
    {
        m_builder.syncTiles(root->childAtIndex(TilesChild), m_surfaces->snapshot(), pixelScale);
        m_tilesDirty = false;
    }

    if (m_overlaysDirty && m_interaction)
    {
        const OverlayFrame& frame = m_interaction->overlayFrame();

        // Page space to item space, through the viewport's own matrix. Deriving
        // a second matrix here is how overlays and page pixels drift apart.
        const QTransform pixelsToItem = QTransform::fromScale(pixelScale, pixelScale);
        ViewportController* viewport = m_viewport;

        m_builder.syncOverlays(root->childAtIndex(OverlaysChild),
                               frame,
                               [viewport, pixelsToItem](int pageIndex, QTransform* out) -> bool
                               {
                                   if (!viewport || pageIndex < 0 || !out)
                                   {
                                       return false;
                                   }

                                   if (viewport->placedPageRect(pageIndex).isEmpty())
                                   {
                                       return false;
                                   }

                                   *out = viewport->pagePointToViewportMatrix(pageIndex) * pixelsToItem;
                                   return true;
                               });

        m_lastOverlayPrimitives = int(frame.primitives.size());
        m_lastDroppedPrimitives = frame.droppedPrimitives;
        m_lastUnrenderablePrimitives = frame.unrenderablePrimitives;
        m_overlaysDirty = false;
    }

    // The developer panel is rebuilt every frame it is visible, on purpose: its
    // whole content is the numbers that changed since the last one.
    const bool wantHud = m_traceOverlayVisible && m_recorder;
    const bool hasHud = root->childCount() > HudChild;

    if (!wantHud)
    {
        if (hasHud)
        {
            QSGNode* hud = root->childAtIndex(HudChild);
            root->removeChildNode(hud);
            delete hud;
        }
    }
    else
    {
        const QImage panel = CanvasTraceOverlay::render(m_recorder->summary(), m_present.summary(), frameStats(), m_palette, ratio);

        QSGImageNode* hud = hasHud ? static_cast<QSGImageNode*>(root->childAtIndex(HudChild)) : nullptr;

        if (panel.isNull())
        {
            if (hud)
            {
                root->removeChildNode(hud);
                delete hud;
            }
        }
        else
        {
            if (!hud)
            {
                hud = hostWindow->createImageNode();
                hud->setOwnsTexture(true);
                hud->setFiltering(QSGTexture::Nearest);
                root->appendChildNode(hud);
            }

            hud->setTexture(hostWindow->createTextureFromImage(panel, QQuickWindow::TextureHasAlphaChannel));

            const qreal panelWidth = panel.width() / ratio;
            const qreal panelHeight = panel.height() / ratio;
            hud->setRect(QRectF(HudMarginPx, HudMarginPx, panelWidth, panelHeight));
        }
    }

    return root;
}

}   // namespace pdfquick
