// MIT License
#include "loupecanvasitem.h"

#include "interactioncontroller.h"
#include "pagesurfacecoordinator.h"
#include "viewportcontroller.h"

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGSimpleRectNode>

namespace pdfquick
{

namespace
{

enum RootChild
{
    BackgroundChild = 0,
    TilesChild = 1,
    OverlaysChild = 2,
    HudChild = 3,
};

constexpr qreal HudMarginPx = 12.0;

}   // namespace

QSGNode* LoupeCanvasItem::prepareSceneGraph(QSGNode* oldNode, QQuickWindow* hostWindow)
{
    if (m_paletteDirty)
    {
        refreshPalette();
    }

    if (m_builderResetPending.exchange(false))
    {
        m_builder.forget();
        m_present.noteBuilderRebuilt();
        m_tilesDirty = true;
        m_overlaysDirty = true;
    }
    m_builder.setWindow(hostWindow);
    m_builder.setResourceBudget(m_surfaces ? m_surfaces->resourceBudget() : nullptr);

    QSGNode* root = oldNode;
    if (!root)
    {
        root = new QSGNode;
        root->appendChildNode(new QSGSimpleRectNode);
        root->appendChildNode(new QSGNode);
        root->appendChildNode(new QSGNode);
        m_builder.forget();
        m_tilesDirty = true;
        m_overlaysDirty = true;
    }

    auto* background = static_cast<QSGSimpleRectNode*>(root->childAtIndex(BackgroundChild));
    background->setRect(boundingRect());
    background->setColor(m_palette.canvasBackground());
    return root;
}

int LoupeCanvasItem::syncTiles(QSGNode* root, qreal pixelScale)
{
    const pdf::PDFRevisionIdentity currentRevision =
        m_surfaces ? m_surfaces->currentRevision() : pdf::PDFRevisionIdentity();
    const pdf::PDFRevisionIdentity snapshotRevision =
        m_surfaces ? m_surfaces->snapshot().token.revision : pdf::PDFRevisionIdentity();
    const bool snapshotIsCurrent =
        !m_surfaces || !snapshotRevision.isValid() || snapshotRevision == currentRevision;

    if (m_surfaces && (m_tilesDirty || !snapshotIsCurrent))
    {
        const pdfinteraction::CanvasSnapshot& snapshot = m_surfaces->snapshot();
        m_builder.syncTiles(root->childAtIndex(TilesChild),
                            snapshotIsCurrent ? snapshot : pdfinteraction::CanvasSnapshot(),
                            pixelScale);
        m_tilesDirty = false;
        return snapshotIsCurrent ? 0 : 1;
    }

    if (!m_surfaces && m_tilesDirty)
    {
        m_builder.syncTiles(root->childAtIndex(TilesChild),
                            pdfinteraction::CanvasSnapshot(),
                            pixelScale);
        m_tilesDirty = false;
    }
    return 0;
}

int LoupeCanvasItem::syncOverlays(QSGNode* root, qreal pixelScale)
{
    const pdf::PDFRevisionIdentity currentRevision =
        m_surfaces ? m_surfaces->currentRevision() : pdf::PDFRevisionIdentity();
    const pdfinteraction::OverlayFrame* live =
        m_interaction ? &m_interaction->overlayFrame() : nullptr;
    const bool overlayIsCurrent =
        !m_surfaces || !live || !live->token.isValid() || live->token.revision == currentRevision;

    if (live && (m_overlaysDirty || !overlayIsCurrent))
    {
        const pdfinteraction::OverlayFrame empty;
        const pdfinteraction::OverlayFrame& frame = overlayIsCurrent ? *live : empty;
        const QTransform pixelsToItem = QTransform::fromScale(pixelScale, pixelScale);
        pdfinteraction::ViewportController* viewport = m_viewport;

        m_builder.syncOverlays(root->childAtIndex(OverlaysChild),
                               frame,
                               [viewport, pixelsToItem](int pageIndex, QTransform* out)
                               {
                                   if (!viewport || pageIndex < 0 || !out ||
                                       viewport->placedPageRect(pageIndex).isEmpty())
                                   {
                                       return false;
                                   }
                                   *out = pixelsToItem * viewport->pagePointToViewportMatrix(pageIndex);
                                   return true;
                               });

        m_lastOverlayPrimitives = int(frame.primitives.size());
        m_lastDroppedPrimitives = frame.droppedPrimitives;
        m_lastUnrenderablePrimitives = frame.unrenderablePrimitives;
        m_overlaysDirty = false;
        return overlayIsCurrent ? 0 : 1;
    }

    if (!live && m_overlaysDirty)
    {
        m_builder.syncOverlays(root->childAtIndex(OverlaysChild),
                               pdfinteraction::OverlayFrame(),
                               nullptr);
        m_overlaysDirty = false;
    }
    return 0;
}

void LoupeCanvasItem::syncTraceHud(QSGNode* root,
                                   QQuickWindow* hostWindow,
                                   qreal devicePixelRatio)
{
    const bool wantHud = m_traceOverlayVisible && m_recorder;
    QSGImageNode* hud = root->childCount() > HudChild
                            ? static_cast<QSGImageNode*>(root->childAtIndex(HudChild))
                            : nullptr;

    const QImage panel = wantHud
                             ? CanvasTraceOverlay::render(m_recorder->summary(),
                                                          m_present.summary(),
                                                          frameStats(),
                                                          m_palette,
                                                          devicePixelRatio)
                             : QImage();
    if (panel.isNull())
    {
        if (hud)
        {
            root->removeChildNode(hud);
            delete hud;
        }
        return;
    }

    if (!hud)
    {
        hud = hostWindow->createImageNode();
        hud->setOwnsTexture(true);
        hud->setFiltering(QSGTexture::Nearest);
        root->appendChildNode(hud);
    }
    hud->setTexture(hostWindow->createTextureFromImage(
        panel, QQuickWindow::TextureHasAlphaChannel));
    hud->setRect(QRectF(HudMarginPx,
                        HudMarginPx,
                        panel.width() / devicePixelRatio,
                        panel.height() / devicePixelRatio));
}

QSGNode* LoupeCanvasItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QQuickWindow* hostWindow = window();
    if (!hostWindow || width() <= 0.0 || height() <= 0.0)
    {
        m_builderResetPending.store(true);
        delete oldNode;
        return nullptr;
    }

    QSGNode* root = prepareSceneGraph(oldNode, hostWindow);
    const qreal ratio = hostWindow->effectiveDevicePixelRatio();
    const qreal pixelScale = ratio > 0.0 ? 1.0 / ratio : 1.0;
    m_refusedStaleFrames += syncTiles(root, pixelScale);
    m_refusedStaleFrames += syncOverlays(root, pixelScale);

    m_lastTileCount = m_builder.tileCount();
    m_lastInexactTileCount = m_builder.inexactTileCount();
    m_present.noteTileBytes(m_builder.tileBytes(), m_builder.tileBytesHighWater());
    if (!m_firstViewReached && m_lastTileCount > 0)
    {
        m_firstViewReached = true;
        m_present.markFirstView();
    }

    syncTraceHud(root, hostWindow, ratio);
    return root;
}

}   // namespace pdfquick
