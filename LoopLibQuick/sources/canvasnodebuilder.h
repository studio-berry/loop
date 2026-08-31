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


#ifndef CANVASNODEBUILDER_H
#define CANVASNODEBUILDER_H

#include "canvaspalette.h"
#include "loopquickglobal.h"

#include "canvassnapshot.h"
#include "overlayframe.h"
#include "pagesurfacekey.h"
#include "pdfresourcebudget.h"

#include <QHash>
#include <QPolygonF>
#include <QString>
#include <QTransform>

#include <functional>
#include <map>

QT_BEGIN_NAMESPACE
class QQuickWindow;
class QSGGeometryNode;
class QSGImageNode;
class QSGNode;
QT_END_NAMESPACE

namespace pdfquick
{

/// Turns a CanvasSnapshot and an OverlayFrame into scene-graph nodes.
///
/// **Thread affinity.** Every method here runs on the scene-graph render thread,
/// inside QQuickItem::updatePaintNode, where the GUI thread is blocked. That is
/// what makes it safe to read the item's snapshot and frame without a lock, and
/// it is why this class is not a QObject and has no signals: it must not be
/// reachable from the GUI thread at all.
///
/// **Retention.** Page tiles are keyed by their complete PageSurfaceKey and
/// overlay nodes by OverlayPrimitive::id, which OverlayFrame documents as stable
/// across frames for the same thing. Both exist so the common case -- a hover
/// change, which produces a new OverlayFrame and no new pixels at all -- reuses
/// every texture and rebuilds only the primitive that changed. A builder that
/// rebuilt from scratch each frame would re-upload every page texture on every
/// mouse move.
///
/// **Stroke width is geometry, not a line width.** QSGGeometry::setLineWidth is
/// ignored by most RHI backends, and the design tokens require severity to be
/// distinguishable without colour, which makes width load-bearing rather than
/// decorative. Strokes are therefore expanded into triangle ribbons here.
class LOOPLIBQUICK_EXPORT CanvasNodeBuilder
{
public:
    CanvasNodeBuilder();
    ~CanvasNodeBuilder();

    CanvasNodeBuilder(const CanvasNodeBuilder&) = delete;
    CanvasNodeBuilder& operator=(const CanvasNodeBuilder&) = delete;

    /// The window owning the scene graph. Textures belong to it, so changing it
    /// drops every retained node: a texture outliving its window is a crash.
    void setWindow(QQuickWindow* window);

    /// Shares the document session's resource authority. The builder only has
    /// a source-image proxy for GPU bytes because Qt Quick does not expose the
    /// backend allocation; a missing authority leaves the legacy diagnostic
    /// counters usable for standalone scene-graph tests.
    void setResourceBudget(std::shared_ptr<pdf::PDFResourceBudget> budget);

    void setPalette(const CanvasPalette& palette);
    const CanvasPalette& palette() const noexcept { return m_palette; }

    /// Page pixels. `parent` is emptied of tiles that the snapshot no longer
    /// contains, and every surviving tile keeps its texture.
    ///
    /// `pixelScale` converts viewport pixels to the item's logical coordinates,
    /// which is 1/devicePixelRatio. The snapshot places tiles in device pixels
    /// because that is what the renderer produced; the scene graph draws in
    /// logical units. Dropping this conversion makes the page quadruple in size
    /// on a 2x display, which is the classic version of this bug.
    void syncTiles(QSGNode* parent, const pdfinteraction::CanvasSnapshot& snapshot, qreal pixelScale);

    /// Overlays, in the frame's own paint order.
    ///
    /// `transformForPage` maps a page index onto the page-space-to-viewport
    /// matrix the page surfaces are placed with -- ViewportController's, not a
    /// second one derived here. Overlay geometry is in page space precisely so
    /// that it cannot drift from the pixels; deriving the matrix locally would
    /// reintroduce the drift the page-space rule exists to prevent.
    ///
    /// A transform the source cannot supply (page not laid out) drops that
    /// page's primitives into skippedPrimitives() rather than drawing them at
    /// the origin.
    void syncOverlays(QSGNode* parent,
                      const pdfinteraction::OverlayFrame& frame,
                      const std::function<bool(int, QTransform*)>& transformForPage);

    /// Drops the retention maps without deleting the nodes they point at.
    ///
    /// Called when the scene graph has already taken the node tree away -- a
    /// window change, or releaseResources -- where the nodes are the scene
    /// graph's to delete and touching them here would be a double free. Must run
    /// on the render thread, like everything else on this class.
    void forget();

    /// Primitives the last syncOverlays could not draw: OverlayFrame reported
    /// them unrenderable, or no page transform was available. Counted rather
    /// than silently dropped, the same rule the overlay builder follows.
    int skippedPrimitives() const noexcept { return m_skippedPrimitives; }

    /// Tiles the last syncTiles drew, and how many of those were inexact --
    /// a lower-resolution surface standing in while the fidelity render is in
    /// flight. Reported for the developer trace overlay.
    int tileCount() const noexcept { return m_tileCount; }
    int inexactTileCount() const noexcept { return m_inexactTileCount; }

    /// Bytes behind the textures currently retained, and the most this builder
    /// has ever retained at once.
    ///
    /// It is the source image's size, not the GPU allocation: the scene graph
    /// does not report what a QSGTexture cost, and a number invented from a
    /// format guess would be worse than one that says what it measured. The
    /// roadmap asks for scene-graph texture bytes "where available", and this is
    /// what is actually available. The high-water mark survives forget(), which
    /// is the point of a high-water mark: a scene-graph loss is exactly the
    /// event you want it to have remembered.
    qint64 tileBytes() const noexcept { return m_tileBytes; }
    qint64 tileBytesHighWater() const noexcept { return m_tileBytesHighWater; }

private:
    struct TileNode
    {
        QSGImageNode* node = nullptr;

        /// The buffer the current texture was uploaded from. Compared by pointer
        /// identity: SurfaceBuffer is immutable and shared, so an unchanged
        /// pointer means unchanged pixels and no re-upload.
        pdfinteraction::SurfaceBufferPointer pixels;
        std::shared_ptr<pdf::PDFResourceReservation> resourceReservation;

        QRect placedRect;
        bool exact = true;
    };

    struct OverlayNode
    {
        QSGGeometryNode* fill = nullptr;
        QSGGeometryNode* stroke = nullptr;
        QSGGeometryNode* focus = nullptr;

        /// The primitive and matrix the geometry was built from. Rebuilt only
        /// when one of them actually changed.
        pdfinteraction::OverlayPrimitive primitive;
        QTransform transform;
        bool valid = false;
    };

    void buildOverlayNode(OverlayNode& entry,
                          const pdfinteraction::OverlayPrimitive& primitive,
                          const QTransform& transform);

    static void destroyOverlayNode(OverlayNode& entry);

    QQuickWindow* m_window = nullptr;
    std::shared_ptr<pdf::PDFResourceBudget> m_resourceBudget;
    CanvasPalette m_palette = CanvasPalette::standard();

    std::map<pdfinteraction::PageSurfaceKey, TileNode> m_tiles;
    QHash<QString, OverlayNode> m_overlays;

    int m_skippedPrimitives = 0;
    int m_tileCount = 0;
    int m_inexactTileCount = 0;

    qint64 m_tileBytes = 0;
    qint64 m_tileBytesHighWater = 0;
};

}   // namespace pdfquick

#endif   // CANVASNODEBUILDER_H
