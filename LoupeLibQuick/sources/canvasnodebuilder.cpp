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


#include "canvasnodebuilder.h"

#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGTexture>

#include <cmath>

namespace pdfquick
{

using pdfinteraction::CanvasSnapshot;
using pdfinteraction::CanvasTile;
using pdfinteraction::OverlayFrame;
using pdfinteraction::OverlayPrimitive;
using pdfinteraction::OverlayPrimitiveKind;
using pdfinteraction::PageSurfaceKey;

namespace
{

/// Shorter than this in viewport pixels and a segment contributes nothing but a
/// degenerate triangle with an undefined normal.
constexpr qreal MinimumSegmentLengthPx = 0.01;

using TriangleList = QList<QPointF>;

void appendTriangle(TriangleList& triangles, QPointF a, QPointF b, QPointF c)
{
    triangles.append(a);
    triangles.append(b);
    triangles.append(c);
}

void appendQuad(TriangleList& triangles, QPointF a, QPointF b, QPointF c, QPointF d)
{
    appendTriangle(triangles, a, b, c);
    appendTriangle(triangles, a, c, d);
}

/// Expands a polyline into a triangle ribbon `width` pixels across.
///
/// Joints are square patches at the interior vertices rather than mitres. A
/// mitre at a near-reversing angle runs away to infinity and has to be clamped;
/// at the widths in the design tokens (1 to 3.5 pixels) the difference is
/// sub-pixel, and the patch cannot produce a spike.
void appendStroke(TriangleList& triangles, const QPolygonF& points, bool closed, float width)
{
    const int count = points.size();
    if (count < 2 || width <= 0.0f)
    {
        return;
    }

    const qreal half = width / 2.0;
    const int segments = closed ? count : count - 1;

    for (int index = 0; index < segments; ++index)
    {
        const QPointF start = points.at(index);
        const QPointF end = points.at((index + 1) % count);

        const QPointF delta = end - start;
        const qreal length = std::hypot(delta.x(), delta.y());
        if (length < MinimumSegmentLengthPx)
        {
            continue;
        }

        const QPointF normal(-delta.y() / length * half, delta.x() / length * half);
        appendQuad(triangles, start + normal, end + normal, end - normal, start - normal);
    }

    // Patch the joints. An open polyline has none at its two ends.
    const int firstJoint = closed ? 0 : 1;
    const int lastJoint = closed ? count : count - 1;
    for (int index = firstJoint; index < lastJoint; ++index)
    {
        const QPointF vertex = points.at(index % count);
        appendQuad(triangles,
                   vertex + QPointF(-half, -half),
                   vertex + QPointF(half, -half),
                   vertex + QPointF(half, half),
                   vertex + QPointF(-half, half));
    }
}

/// Fills a convex polygon, expanded into explicit triangles.
///
/// Not DrawTriangleFan: Metal and D3D have no triangle-fan primitive, so a fan
/// silently draws nothing on two of the backends ADR-010 requires evidence from.
void appendConvexFill(TriangleList& triangles, const QPolygonF& points)
{
    if (points.size() < 3)
    {
        return;
    }

    for (int index = 1; index + 1 < points.size(); ++index)
    {
        appendTriangle(triangles, points.at(0), points.at(index), points.at(index + 1));
    }
}

QPolygonF quadFor(const QTransform& transform, const QRectF& rect)
{
    QPolygonF quad;
    quad.append(transform.map(rect.topLeft()));
    quad.append(transform.map(rect.topRight()));
    quad.append(transform.map(rect.bottomRight()));
    quad.append(transform.map(rect.bottomLeft()));
    return quad;
}

QPolygonF squareAt(QPointF center, qreal radius)
{
    QPolygonF square;
    square.append(center + QPointF(-radius, -radius));
    square.append(center + QPointF(radius, -radius));
    square.append(center + QPointF(radius, radius));
    square.append(center + QPointF(-radius, radius));
    return square;
}

QPolygonF diamondAt(QPointF center, qreal radius)
{
    QPolygonF diamond;
    diamond.append(center + QPointF(0.0, -radius));
    diamond.append(center + QPointF(radius, 0.0));
    diamond.append(center + QPointF(0.0, radius));
    diamond.append(center + QPointF(-radius, 0.0));
    return diamond;
}

/// Replaces a node's geometry and material from a triangle list. Returns false
/// when there is nothing to draw, in which case the node must not be attached.
bool applyTriangles(QSGGeometryNode* node, const TriangleList& triangles, const QColor& color)
{
    if (!node || triangles.isEmpty() || color.alpha() == 0)
    {
        return false;
    }

    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), triangles.size());
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);

    QSGGeometry::Point2D* vertices = geometry->vertexDataAsPoint2D();
    for (int index = 0; index < triangles.size(); ++index)
    {
        vertices[index].set(float(triangles.at(index).x()), float(triangles.at(index).y()));
    }

    auto* material = new QSGFlatColorMaterial;
    material->setColor(color);

    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);

    // No explicit Blending flag: QSGFlatColorMaterial::setColor already derives
    // it from the alpha, and the palette's fill colours are translucent.
    return true;
}

}   // namespace

CanvasNodeBuilder::CanvasNodeBuilder() = default;

CanvasNodeBuilder::~CanvasNodeBuilder()
{
    // Nodes are owned by the scene graph once attached; the caller detaches and
    // deletes the root. Only the retention maps are dropped here.
    m_tiles.clear();
    m_overlays.clear();
}

void CanvasNodeBuilder::setWindow(QQuickWindow* window)
{
    if (m_window == window)
    {
        return;
    }

    m_window = window;
    forget();
}

void CanvasNodeBuilder::setPalette(const CanvasPalette& palette)
{
    m_palette = palette;

    // Every retained overlay node was coloured by the old palette, so none of
    // them may be reused. Invalidating rather than deleting keeps the nodes
    // attached until the next sync rebuilds them.
    for (auto iterator = m_overlays.begin(); iterator != m_overlays.end(); ++iterator)
    {
        iterator->valid = false;
    }
}

void CanvasNodeBuilder::forget()
{
    for (auto& entry : m_overlays)
    {
        destroyOverlayNode(entry);
    }

    for (auto& entry : m_tiles)
    {
        delete entry.second.node;
    }

    m_tiles.clear();
    m_overlays.clear();
    m_skippedPrimitives = 0;
    m_tileCount = 0;
    m_inexactTileCount = 0;

    // Nothing is retained any more, so nothing is held. The high-water mark is
    // deliberately not cleared: it is a lifetime measurement, and clearing it
    // here would erase the peak that the scene-graph loss came after.
    m_tileBytes = 0;
}

void CanvasNodeBuilder::syncTiles(QSGNode* parent, const CanvasSnapshot& snapshot, qreal pixelScale)
{
    m_tileCount = 0;
    m_inexactTileCount = 0;

    if (!parent || !m_window)
    {
        return;
    }

    qint64 retainedBytes = 0;

    std::map<PageSurfaceKey, TileNode> retained;

    parent->removeAllChildNodes();

    for (const CanvasTile& tile : snapshot.tiles)
    {
        if (!tile.pixels || tile.pixels->image.isNull() || tile.placedRect.isEmpty())
        {
            continue;
        }

        auto existing = m_tiles.find(tile.key);
        TileNode entry;
        if (existing != m_tiles.end())
        {
            entry = existing->second;
            m_tiles.erase(existing);
        }

        if (!entry.node)
        {
            entry.node = m_window->createImageNode();
            entry.node->setOwnsTexture(true);
            entry.node->setFiltering(QSGTexture::Linear);
        }

        // Pointer identity is the whole test. SurfaceBuffer is immutable and
        // shared, so the same pointer is necessarily the same pixels and the
        // upload is skipped -- which is what keeps a pan from re-uploading every
        // visible page.
        if (entry.pixels != tile.pixels)
        {
            const QQuickWindow::CreateTextureOptions options = tile.pixels->image.hasAlphaChannel()
                                                                   ? QQuickWindow::TextureHasAlphaChannel
                                                                   : QQuickWindow::TextureIsOpaque;
            entry.node->setTexture(m_window->createTextureFromImage(tile.pixels->image, options));
            entry.pixels = tile.pixels;
        }

        entry.placedRect = tile.placedRect;
        entry.exact = tile.exact;

        const QRectF placedLogical(tile.placedRect.x() * pixelScale,
                                   tile.placedRect.y() * pixelScale,
                                   tile.placedRect.width() * pixelScale,
                                   tile.placedRect.height() * pixelScale);
        entry.node->setRect(placedLogical);

        // An inexact tile is a lower-resolution surface being scaled up while
        // the fidelity render is still in flight. It is filtered exactly like an
        // exact one -- the distinction is reported through inexactTileCount(),
        // never hidden by making the stand-in look sharper than it is.
        entry.node->setMipmapFiltering(QSGTexture::None);

        parent->appendChildNode(entry.node);
        retained.insert({ tile.key, entry });

        retainedBytes += qint64(tile.pixels->image.sizeInBytes());

        ++m_tileCount;
        if (!tile.exact)
        {
            ++m_inexactTileCount;
        }
    }

    // Anything still in m_tiles was not in this snapshot. Its node is already
    // detached by removeAllChildNodes above, so deleting it is safe and releases
    // the texture it owns.
    for (auto& remaining : m_tiles)
    {
        delete remaining.second.node;
    }

    m_tiles = std::move(retained);

    m_tileBytes = retainedBytes;
    m_tileBytesHighWater = qMax(m_tileBytesHighWater, retainedBytes);
}

void CanvasNodeBuilder::destroyOverlayNode(OverlayNode& entry)
{
    delete entry.fill;
    delete entry.stroke;
    delete entry.focus;
    entry.fill = nullptr;
    entry.stroke = nullptr;
    entry.focus = nullptr;
    entry.valid = false;
}

void CanvasNodeBuilder::buildOverlayNode(OverlayNode& entry, const OverlayPrimitive& primitive, const QTransform& transform)
{
    // Geometry is rebuilt wholesale rather than edited in place: the vertex
    // count changes with the primitive's kind and point count, and a resized
    // QSGGeometry has to be reallocated anyway.
    destroyOverlayNode(entry);

    const OverlayStyle style = m_palette.styleFor(primitive);

    QPolygonF outline;
    bool closed = true;
    bool fillable = true;

    switch (primitive.kind)
    {
        case OverlayPrimitiveKind::Rectangle:
            outline = quadFor(transform, primitive.pageBounds);
            break;

        case OverlayPrimitiveKind::Polyline:
            outline = transform.map(primitive.pagePolygon);
            closed = false;

            // A polyline is not necessarily convex, and the fan expansion above
            // is only correct for convex input. Strokes are unaffected.
            fillable = false;
            break;

        case OverlayPrimitiveKind::Marker:
            outline = diamondAt(transform.map(primitive.pageBounds.center()), style.pointRadiusPx);
            break;

        case OverlayPrimitiveKind::Handle:
            outline = squareAt(transform.map(primitive.pageBounds.center()), style.pointRadiusPx);
            break;
    }

    if (outline.size() < 2)
    {
        return;
    }

    if (fillable && style.fill.alpha() > 0)
    {
        TriangleList triangles;
        appendConvexFill(triangles, outline);

        auto* node = new QSGGeometryNode;
        if (applyTriangles(node, triangles, style.fill))
        {
            entry.fill = node;
        }
        else
        {
            delete node;
        }
    }

    {
        TriangleList triangles;
        appendStroke(triangles, outline, closed, style.strokeWidthPx);

        auto* node = new QSGGeometryNode;
        if (applyTriangles(node, triangles, style.stroke))
        {
            entry.stroke = node;
        }
        else
        {
            delete node;
        }
    }

    if (style.focusRing)
    {
        // Built from the outline's bounding box rather than an offset outline.
        // Polygon offsetting is a real algorithm with self-intersection cases,
        // and a focus ring is an affordance: it has to be unmistakably visible
        // and outside the shape, not geometrically exact.
        const qreal grow = style.focusRingOffsetPx + style.focusRingWidthPx / 2.0;
        const QRectF ring = outline.boundingRect().adjusted(-grow, -grow, grow, grow);

        TriangleList triangles;
        appendStroke(triangles, quadFor(QTransform(), ring), true, style.focusRingWidthPx);

        auto* node = new QSGGeometryNode;
        if (applyTriangles(node, triangles, m_palette.isHighContrast() ? m_palette.hudText() : style.stroke))
        {
            entry.focus = node;
        }
        else
        {
            delete node;
        }
    }

    entry.primitive = primitive;
    entry.transform = transform;
    entry.valid = true;
}

namespace
{

QString overlayRetentionKey(const pdfinteraction::OverlayPrimitive& primitive)
{
    return QStringLiteral("%1:%2:%3")
        .arg(int(primitive.layer))
        .arg(int(primitive.target.kind))
        .arg(primitive.id);
}

}   // namespace

void CanvasNodeBuilder::syncOverlays(QSGNode* parent,
                                     const OverlayFrame& frame,
                                     const std::function<bool(int, QTransform*)>& transformForPage)
{
    m_skippedPrimitives = 0;

    if (!parent)
    {
        return;
    }

    // Detaching without deleting: the retained nodes below are re-attached in
    // the frame's paint order, which is how a reordered frame stays correct
    // without discarding every node it still contains.
    parent->removeAllChildNodes();

    QHash<QString, OverlayNode> retained;
    retained.reserve(frame.primitives.size());

    for (const OverlayPrimitive& primitive : frame.primitives)
    {
        if (!primitive.renderable)
        {
            // OverlayFrame emits these deliberately so a host can report what it
            // could not draw instead of losing it silently.
            ++m_skippedPrimitives;
            continue;
        }

        QTransform transform;
        if (!transformForPage || !transformForPage(primitive.pageIndex, &transform))
        {
            ++m_skippedPrimitives;
            continue;
        }

        OverlayNode entry = m_overlays.take(overlayRetentionKey(primitive));

        const bool reusable = entry.valid && entry.primitive == primitive && entry.transform == transform;
        if (!reusable)
        {
            buildOverlayNode(entry, primitive, transform);
        }

        if (entry.fill)
        {
            parent->appendChildNode(entry.fill);
        }
        if (entry.stroke)
        {
            parent->appendChildNode(entry.stroke);
        }
        if (entry.focus)
        {
            parent->appendChildNode(entry.focus);
        }

        if (!entry.fill && !entry.stroke && !entry.focus)
        {
            ++m_skippedPrimitives;
            continue;
        }

        retained.insert(overlayRetentionKey(primitive), entry);
    }

    // Whatever is left keyed in m_overlays is a primitive this frame dropped.
    for (auto iterator = m_overlays.begin(); iterator != m_overlays.end(); ++iterator)
    {
        destroyOverlayNode(iterator.value());
    }

    m_overlays = std::move(retained);
}

}   // namespace pdfquick
