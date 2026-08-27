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

#include "overlaybuilder.h"

#include <algorithm>

namespace pdfinteraction
{

namespace
{

/// A primitive whose geometry cannot be drawn is still emitted, flagged, and
/// counted. Dropping it here would make an overlay with broken input look
/// identical to an overlay with no input, and issue #143 AC6 asks for the
/// opposite: a safe "not renderable" state rather than a blocked frame.
void markUnrenderable(OverlayPrimitive& primitive)
{
    primitive.renderable = false;
    primitive.pageBounds = QRectF();
    primitive.pagePolygon = QPolygonF();
}

}   // namespace

OverlayBuilder::OverlayBuilder(const ViewportController& viewport, OverlayBounds bounds) :
    m_viewport(&viewport),
    m_bounds(bounds)
{
}

void OverlayBuilder::setFindings(QList<InteractionTarget> findings)
{
    m_findings = std::move(findings);
}

void OverlayBuilder::setSeverities(QHash<QString, OverlaySeverity> severities)
{
    m_severities = std::move(severities);
}

void OverlayBuilder::setGuides(QList<InteractionTarget> guides)
{
    m_guides = std::move(guides);
}

void OverlayBuilder::setHandles(QList<InteractionTarget> handles)
{
    m_handles = std::move(handles);
}

void OverlayBuilder::setHiddenFindingIds(QSet<QString> hiddenIds)
{
    m_hiddenFindingIds = std::move(hiddenIds);
}

void OverlayBuilder::setFocusedId(QString focusedId)
{
    m_focusedId = std::move(focusedId);
}

QRectF OverlayBuilder::visiblePageBounds(int pageIndex) const
{
    if (!m_viewport)
    {
        return QRectF();
    }

    const QRect placed = m_viewport->placedPageRect(pageIndex);
    if (!placed.isValid())
    {
        return QRectF();
    }

    const QRect visiblePx = placed.intersected(m_viewport->viewportRect());
    if (visiblePx.isEmpty())
    {
        return QRectF();
    }

    // Back through the same matrix the page surfaces use. Deriving the clip from
    // page sizes independently is how two implementations of "the visible part
    // of the page" drift apart under rotation.
    const QTransform toViewport = m_viewport->pagePointToViewportMatrix(pageIndex);
    bool invertible = false;
    const QTransform toPage = toViewport.inverted(&invertible);
    if (!invertible)
    {
        return QRectF();
    }

    return toPage.mapRect(QRectF(visiblePx)).normalized();
}

OverlayFrame OverlayBuilder::build(const InteractionState& state, const RevisionFencedToken& token) const
{
    OverlayFrame frame;

    if (!token.isValid() || !m_viewport)
    {
        return frame;
    }

    frame.token = token;

    const bool suppressExtraGraphics = m_denyExtraGraphics;

    // Resolved once. ViewportController::visiblePages() rebuilds a list on every
    // call, and asking it per primitive is how a frame with a few thousand
    // findings turns into a quadratic walk -- the unbounded per-frame work issue
    // #143 AC5 rules out.
    const QList<int> visiblePageList = m_viewport->visiblePages();
    const QSet<int> visiblePages(visiblePageList.cbegin(), visiblePageList.cend());
    const auto isVisiblePage = [&visiblePages](int pageIndex)
    { return visiblePages.contains(pageIndex); };

    QHash<int, QRectF> clipCache;
    const auto clipFor = [&](int pageIndex) -> QRectF
    {
        const auto it = clipCache.constFind(pageIndex);
        if (it != clipCache.constEnd())
        {
            return it.value();
        }

        const QRectF clip = visiblePageBounds(pageIndex);
        clipCache.insert(pageIndex, clip);
        return clip;
    };

    int findingCount = 0;
    quint32 sequence = 0;

    // `id` is the primitive's identity, which is not always the target's: the
    // selection outline and the drag preview draw a second primitive for an
    // object that already has one, and a host keys retained nodes on the id. The
    // target keeps its own id so a click routed back from a primitive still names
    // the real object.
    const auto emitPrimitive = [&](const InteractionTarget& target, const QString& id, QRectF pageBounds, OverlayLayer layer, OverlayPrimitiveKind kind, OverlaySeverity severity) -> bool
    {
        if (frame.primitives.size() >= m_bounds.maxPrimitives)
        {
            ++frame.droppedPrimitives;
            return false;
        }

        if (layer == OverlayLayer::Findings && findingCount >= m_bounds.maxFindingPrimitives)
        {
            ++frame.droppedPrimitives;
            return false;
        }

        OverlayPrimitive primitive;
        primitive.id = id;
        primitive.layer = layer;
        primitive.kind = kind;
        primitive.severity = severity;
        primitive.pageIndex = target.pageIndex;
        primitive.pageBounds = pageBounds;
        primitive.target = target;
        primitive.sequence = sequence++;
        primitive.hovered = state.hovered().isValid() && state.hovered().id == target.id && state.hovered().pageIndex == target.pageIndex;
        primitive.selected = state.selected().isValid() && state.selected().id == target.id && state.selected().pageIndex == target.pageIndex;
        primitive.focused = !m_focusedId.isEmpty() && m_focusedId == target.id;

        const QRectF clip = clipFor(target.pageIndex);
        if (pageBounds.isNull() || pageBounds.isEmpty() || clip.isNull())
        {
            markUnrenderable(primitive);
            ++frame.unrenderablePrimitives;
        }
        else
        {
            const QRectF clipped = pageBounds.intersected(clip);
            if (clipped.isEmpty())
            {
                // Entirely off the visible part of its page. Emitted so a caller
                // can say "12 markers are outside the view" rather than losing
                // them, but with nothing for a host to paint.
                markUnrenderable(primitive);
                ++frame.unrenderablePrimitives;
            }
            else
            {
                primitive.pageBounds = clipped;
            }
        }

        if (layer == OverlayLayer::Findings)
        {
            ++findingCount;
        }

        frame.primitives.push_back(primitive);
        return true;
    };

    for (const InteractionTarget& guide : m_guides)
    {
        if (suppressExtraGraphics || !isVisiblePage(guide.pageIndex))
        {
            continue;
        }

        const OverlayLayer layer = guide.kind == InteractionTargetKind::PageBox ? OverlayLayer::PageChrome : OverlayLayer::Guides;
        emitPrimitive(guide, guide.id, guide.pageBounds, layer, OverlayPrimitiveKind::Rectangle, OverlaySeverity::None);
    }

    for (const InteractionTarget& finding : m_findings)
    {
        if (suppressExtraGraphics || !isVisiblePage(finding.pageIndex) || m_hiddenFindingIds.contains(finding.id))
        {
            continue;
        }

        const OverlaySeverity severity = m_severities.value(finding.id, OverlaySeverity::Info);
        if (!emitPrimitive(finding, finding.id, finding.pageBounds, OverlayLayer::Findings, OverlayPrimitiveKind::Marker, severity))
        {
            // The bound is a bound, not a hint. Keep counting what was dropped
            // so the frame reports the shortfall instead of implying there were
            // only ever this many findings.
            continue;
        }
    }

    const InteractionTarget& hovered = state.hovered();
    if (!suppressExtraGraphics && hovered.isValid() && isVisiblePage(hovered.pageIndex))
    {
        emitPrimitive(hovered, hovered.id + QStringLiteral("#hover"), hovered.pageBounds, OverlayLayer::Hover, OverlayPrimitiveKind::Rectangle, OverlaySeverity::None);
    }

    const InteractionTarget& selected = state.selected();
    if (!suppressExtraGraphics && selected.isValid() && isVisiblePage(selected.pageIndex))
    {
        emitPrimitive(selected, selected.id + QStringLiteral("#selection"), selected.pageBounds, OverlayLayer::Selection, OverlayPrimitiveKind::Rectangle, OverlaySeverity::None);
    }

    for (const InteractionTarget& handle : m_handles)
    {
        if (suppressExtraGraphics || !isVisiblePage(handle.pageIndex))
        {
            continue;
        }

        emitPrimitive(handle, handle.id, handle.pageBounds, OverlayLayer::DragHandles, OverlayPrimitiveKind::Handle, OverlaySeverity::None);
    }

    if (!suppressExtraGraphics && state.drag().has_value())
    {
        const DragSession& drag = *state.drag();
        if (drag.exceededThreshold && isVisiblePage(drag.target.pageIndex))
        {
            emitPrimitive(drag.target, drag.target.id + QStringLiteral("#preview"), drag.previewPageBounds, OverlayLayer::ToolPreview, OverlayPrimitiveKind::Rectangle, OverlaySeverity::None);
        }
    }

    std::stable_sort(frame.primitives.begin(), frame.primitives.end(), overlayPaintsBefore);
    return frame;
}

}   // namespace pdfinteraction
