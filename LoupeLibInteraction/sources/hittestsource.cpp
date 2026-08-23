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

#include "hittestsource.h"

#include "pdfdocument.h"

#include <algorithm>

namespace pdfinteraction
{

namespace
{

/// Page indices are 0-based everywhere in this layer and 1-based in
/// pdf::PDFEvidenceRecord. Converting in one place keeps the off-by-one from
/// being rediscovered at every call site.
constexpr int evidencePageToIndex(int page)
{
    return page - 1;
}

bool touchesEdge(const QRectF& box, QPointF point, qreal tolerance)
{
    if (box.isNull())
    {
        return false;
    }

    const QRectF outer = box.adjusted(-tolerance, -tolerance, tolerance, tolerance);
    if (!outer.contains(point))
    {
        return false;
    }

    const QRectF inner = box.adjusted(tolerance, tolerance, -tolerance, -tolerance);
    return !inner.isValid() || !inner.contains(point);
}

InteractionTarget makePageBoxTarget(int pageIndex, const QString& name, const QRectF& box)
{
    InteractionTarget target;
    target.kind = InteractionTargetKind::PageBox;
    target.pageIndex = pageIndex;
    target.id = name;
    target.pageBounds = box;
    return target;
}

}   // namespace

EvidenceHitTestSource::EvidenceHitTestSource(pdf::PDFEvidenceGraph graph)
{
    setGraph(std::move(graph));
}

void EvidenceHitTestSource::setGraph(pdf::PDFEvidenceGraph graph)
{
    m_graph = std::move(graph);
    indexGraph();
}

void EvidenceHitTestSource::indexGraph()
{
    m_targets.clear();
    m_unrenderableRecords = 0;
    m_targets.reserve(m_graph.records.size());

    for (const pdf::PDFEvidenceRecord& record : m_graph.records)
    {
        const int pageIndex = evidencePageToIndex(record.page);

        // Issue #143 AC6: a record with no usable geometry is counted and
        // skipped, never allowed to abort the pass. An empty rect is as
        // unusable as a null one -- it can be hit by nothing.
        if (pageIndex < 0 || record.geometry.isNull() || record.geometry.isEmpty() || record.id.isEmpty())
        {
            ++m_unrenderableRecords;
            continue;
        }

        InteractionTarget target;
        target.kind = InteractionTargetKind::Finding;
        target.pageIndex = pageIndex;
        target.id = record.id;
        target.pageBounds = record.geometry.normalized();
        m_targets.push_back(target);
    }
}

QList<InteractionTarget> EvidenceHitTestSource::hitTest(int pageIndex, QPointF pagePoint) const
{
    QList<InteractionTarget> hits;

    for (const InteractionTarget& target : m_targets)
    {
        if (target.pageIndex == pageIndex && target.pageBounds.contains(pagePoint))
        {
            hits.push_back(target);
        }
    }

    return hits;
}

QList<InteractionTarget> EvidenceHitTestSource::targetsForPage(int pageIndex) const
{
    QList<InteractionTarget> targets;

    for (const InteractionTarget& target : m_targets)
    {
        if (target.pageIndex == pageIndex)
        {
            targets.push_back(target);
        }
    }

    return targets;
}

PageBoxHitTestSource::PageBoxHitTestSource(pdf::PDFDocumentContext* context) :
    m_context(context)
{
}

void PageBoxHitTestSource::setEdgeTolerance(qreal tolerance)
{
    m_edgeTolerance = qMax(qreal(0.0), tolerance);
}

QList<InteractionTarget> PageBoxHitTestSource::hitTest(int pageIndex, QPointF pagePoint) const
{
    QList<InteractionTarget> hits;

    for (const InteractionTarget& target : targetsForPage(pageIndex))
    {
        if (touchesEdge(target.pageBounds, pagePoint, m_edgeTolerance))
        {
            hits.push_back(target);
        }
    }

    return hits;
}

QList<InteractionTarget> PageBoxHitTestSource::targetsForPage(int pageIndex) const
{
    QList<InteractionTarget> targets;

    const pdf::PDFDocumentContext* context = m_context.data();
    const pdf::PDFDocument* document = context ? context->getDocument() : nullptr;
    if (!document || pageIndex < 0)
    {
        return targets;
    }

    const pdf::PDFCatalog* catalog = document->getCatalog();
    if (!catalog || size_t(pageIndex) >= catalog->getPageCount())
    {
        return targets;
    }

    const pdf::PDFPage* page = catalog->getPage(size_t(pageIndex));
    if (!page)
    {
        return targets;
    }

    // Ordered outermost first, so an overlay draws the enclosing boxes under the
    // ones they contain and the smallest-area tie-break picks the innermost edge
    // when several coincide.
    targets.push_back(makePageBoxTarget(pageIndex, QStringLiteral("media"), page->getMediaBox()));
    targets.push_back(makePageBoxTarget(pageIndex, QStringLiteral("crop"), page->getCropBox()));
    targets.push_back(makePageBoxTarget(pageIndex, QStringLiteral("bleed"), page->getBleedBox()));
    targets.push_back(makePageBoxTarget(pageIndex, QStringLiteral("trim"), page->getTrimBox()));
    targets.push_back(makePageBoxTarget(pageIndex, QStringLiteral("art"), page->getArtBox()));

    targets.removeIf([](const InteractionTarget& target)
                     { return target.pageBounds.isNull() || target.pageBounds.isEmpty(); });
    return targets;
}

void HitTestDispatcher::addSource(IHitTestSource* source)
{
    if (source && !m_sources.contains(source))
    {
        m_sources.push_back(source);
    }
}

void HitTestDispatcher::clearSources()
{
    m_sources.clear();
}

void HitTestDispatcher::setSelection(const InteractionTarget& target)
{
    m_selection = target;
}

void HitTestDispatcher::setHandles(QList<InteractionTarget> handles)
{
    m_handles = std::move(handles);
}

bool HitTestDispatcher::isSameTarget(const InteractionTarget& left, const InteractionTarget& right)
{
    return left.isValid() && right.isValid() && left.pageIndex == right.pageIndex && left.id == right.id;
}

bool HitTestDispatcher::ranksBefore(const InteractionTarget& left, const InteractionTarget& right)
{
    if (left.kind != right.kind)
    {
        return int(left.kind) < int(right.kind);
    }

    // Smallest area wins inside a kind: a marker nested inside a larger one is
    // the more specific answer, and it is also the one a user can otherwise
    // never reach.
    const qreal leftArea = left.pageBounds.width() * left.pageBounds.height();
    const qreal rightArea = right.pageBounds.width() * right.pageBounds.height();
    if (!qFuzzyCompare(leftArea + qreal(1.0), rightArea + qreal(1.0)))
    {
        return leftArea < rightArea;
    }

    // Two markers with the same kind and the same area must still order, or the
    // answer depends on container order. The stable id is the only field left
    // that is guaranteed unique and revision-stable.
    return left.id < right.id;
}

QList<InteractionTarget> HitTestDispatcher::hitTestAll(int pageIndex, QPointF pagePoint) const
{
    QList<InteractionTarget> candidates;

    if (pageIndex < 0)
    {
        return candidates;
    }

    for (const InteractionTarget& handle : m_handles)
    {
        if (handle.pageIndex == pageIndex && handle.pageBounds.contains(pagePoint))
        {
            candidates.push_back(handle);
        }
    }

    for (const IHitTestSource* source : m_sources)
    {
        candidates.append(source->hitTest(pageIndex, pagePoint));
    }

    const InteractionTarget selection = m_selection;
    const auto ranks = [&selection](const InteractionTarget& left, const InteractionTarget& right)
    {
        const bool leftHandle = left.kind == InteractionTargetKind::DragHandle;
        const bool rightHandle = right.kind == InteractionTargetKind::DragHandle;
        if (leftHandle != rightHandle)
        {
            return leftHandle;
        }

        const bool leftSelected = isSameTarget(left, selection);
        const bool rightSelected = isSameTarget(right, selection);
        if (leftSelected != rightSelected)
        {
            return leftSelected;
        }

        return ranksBefore(left, right);
    };

    std::stable_sort(candidates.begin(), candidates.end(), ranks);
    return candidates;
}

InteractionTarget HitTestDispatcher::hitTest(int pageIndex, QPointF pagePoint) const
{
    const QList<InteractionTarget> candidates = hitTestAll(pageIndex, pagePoint);
    if (!candidates.isEmpty())
    {
        return candidates.constFirst();
    }

    if (pageIndex < 0)
    {
        return InteractionTarget();
    }

    InteractionTarget page;
    page.kind = InteractionTargetKind::Page;
    page.pageIndex = pageIndex;
    page.id = QString::number(pageIndex);
    return page;
}

}   // namespace pdfinteraction
