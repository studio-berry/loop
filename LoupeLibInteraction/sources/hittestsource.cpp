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

/// The probe rectangle a tolerance turns a point into.
///
/// A zero tolerance yields a zero-size rect, which the spatial index treats
/// exactly as the point overload did -- that identity is what lets the
/// tolerance be introduced without moving any existing result.
QRectF toleranceProbe(QPointF point, qreal tolerance)
{
    if (!(tolerance > 0.0))
    {
        return QRectF(point, QSizeF(0.0, 0.0));
    }

    return QRectF(point.x() - tolerance, point.y() - tolerance, tolerance * 2.0, tolerance * 2.0);
}

/// Precise test with slack. At tolerance zero this is QRectF::contains, so a
/// caller that asks for no slack gets byte-identical results to the exact test
/// this replaced.
bool withinTolerance(const QRectF& box, QPointF point, qreal tolerance)
{
    if (!(tolerance > 0.0))
    {
        return box.contains(point);
    }

    return box.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(point);
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
    m_targetsByPage.clear();
    m_indexByPage.clear();
    m_unrenderableRecords = 0;

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
        m_targetsByPage[pageIndex].push_back(target);
    }

    // Issue #145 AC1: build one spatial index per page so a pointer move
    // queries a grid cell instead of scanning every record on the page. The
    // index is keyed on position within that page's target list, which is
    // stable for the lifetime of this graph (rebuilt wholesale, never
    // mutated in place).
    for (auto it = m_targetsByPage.constBegin(); it != m_targetsByPage.constEnd(); ++it)
    {
        QList<QRectF> bounds;
        bounds.reserve(it.value().size());
        for (const InteractionTarget& target : it.value())
        {
            bounds.push_back(target.pageBounds);
        }
        m_indexByPage[it.key()].build(bounds);
    }
}

QList<InteractionTarget> EvidenceHitTestSource::hitTest(int pageIndex, QPointF pagePoint) const
{
    return hitTest(pageIndex, pagePoint, 0.0, nullptr);
}

QList<InteractionTarget> EvidenceHitTestSource::hitTest(int pageIndex,
                                                        QPointF pagePoint,
                                                        qreal pageTolerance,
                                                        HitTestCounters* counters) const
{
    QList<InteractionTarget> hits;

    const auto pageTargets = m_targetsByPage.constFind(pageIndex);
    const auto pageIndexEntry = m_indexByPage.constFind(pageIndex);
    if (pageTargets == m_targetsByPage.constEnd() || pageIndexEntry == m_indexByPage.constEnd())
    {
        return hits;
    }

    const QList<int> candidates = pageIndexEntry.value().query(toleranceProbe(pagePoint, pageTolerance));

    for (int candidate : candidates)
    {
        const InteractionTarget& target = pageTargets.value().at(candidate);
        if (withinTolerance(target.pageBounds, pagePoint, pageTolerance))
        {
            hits.push_back(target);
        }
    }

    if (counters)
    {
        counters->indexCandidates += int(candidates.size());
        counters->preciseHits += int(hits.size());
    }

    return hits;
}

QList<InteractionTarget> EvidenceHitTestSource::targetsForPage(int pageIndex) const
{
    return m_targetsByPage.value(pageIndex);
}

void FindingListHitTestSource::setTargets(QList<InteractionTarget> targets)
{
    m_targetsByPage.clear();
    m_indexByPage.clear();

    for (const InteractionTarget& target : targets)
    {
        m_targetsByPage[target.pageIndex].push_back(target);
    }

    // Issue #145 AC1: same grid-index treatment as EvidenceHitTestSource, so
    // a finding source populated directly (rather than from an evidence
    // graph) gets the same sub-linear candidate selection.
    for (auto it = m_targetsByPage.constBegin(); it != m_targetsByPage.constEnd(); ++it)
    {
        QList<QRectF> bounds;
        bounds.reserve(it.value().size());
        for (const InteractionTarget& target : it.value())
        {
            bounds.push_back(target.pageBounds);
        }
        m_indexByPage[it.key()].build(bounds);
    }
}

QList<InteractionTarget> FindingListHitTestSource::hitTest(int pageIndex, QPointF pagePoint) const
{
    return hitTest(pageIndex, pagePoint, 0.0, nullptr);
}

QList<InteractionTarget> FindingListHitTestSource::hitTest(int pageIndex,
                                                           QPointF pagePoint,
                                                           qreal pageTolerance,
                                                           HitTestCounters* counters) const
{
    QList<InteractionTarget> hits;

    const auto pageTargets = m_targetsByPage.constFind(pageIndex);
    const auto pageIndexEntry = m_indexByPage.constFind(pageIndex);
    if (pageTargets == m_targetsByPage.constEnd() || pageIndexEntry == m_indexByPage.constEnd())
    {
        return hits;
    }

    const QList<int> candidates = pageIndexEntry.value().query(toleranceProbe(pagePoint, pageTolerance));

    for (int candidate : candidates)
    {
        const InteractionTarget& target = pageTargets.value().at(candidate);
        if (withinTolerance(target.pageBounds, pagePoint, pageTolerance))
        {
            hits.push_back(target);
        }
    }

    if (counters)
    {
        counters->indexCandidates += int(candidates.size());
        counters->preciseHits += int(hits.size());
    }

    return hits;
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
    return hitTest(pageIndex, pagePoint, m_edgeTolerance, nullptr);
}

QList<InteractionTarget> PageBoxHitTestSource::hitTest(int pageIndex,
                                                       QPointF pagePoint,
                                                       qreal pageTolerance,
                                                       HitTestCounters* counters) const
{
    QList<InteractionTarget> hits;

    // A page box is hittable on its edge only, so it needs a non-zero slack to
    // be reachable at all: an exact edge test would demand the pointer land on
    // a zero-width line. Fall back to the configured value when the caller
    // supplies none.
    const qreal tolerance = pageTolerance > 0.0 ? pageTolerance : m_edgeTolerance;

    const QList<InteractionTarget> boxes = targetsForPage(pageIndex);

    for (const InteractionTarget& target : boxes)
    {
        if (touchesEdge(target.pageBounds, pagePoint, tolerance))
        {
            hits.push_back(target);
        }
    }

    if (counters)
    {
        // At most five boxes and no index: the scan is the narrowing.
        counters->indexCandidates += int(boxes.size());
        counters->preciseHits += int(hits.size());
    }

    return hits;
}

QList<InteractionTarget> PageBoxHitTestSource::targetsForPage(int pageIndex) const
{
    const pdf::PDFDocumentContext* context = m_context.data();
    const pdf::PDFDocument* document = context ? context->getDocument() : nullptr;
    if (!document || pageIndex < 0)
    {
        return {};
    }

    const pdf::PDFRevisionIdentity revision = context->getRevision();
    if (!(revision == m_cachedRevision))
    {
        m_pageTargets.clear();
        m_cachedRevision = revision;
    }

    const auto cached = m_pageTargets.constFind(pageIndex);
    if (cached != m_pageTargets.constEnd())
    {
        return cached.value();
    }

    QList<InteractionTarget> targets;

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
    m_pageTargets.insert(pageIndex, targets);
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

void HitTestDispatcher::setScreenTolerancePx(qreal pixels)
{
    m_screenTolerancePx = qMax(qreal(0.0), pixels);
}

void HitTestDispatcher::setViewScale(qreal pixelsPerPageUnit)
{
    m_viewScale = pixelsPerPageUnit;
}

qreal HitTestDispatcher::pageTolerance() const noexcept
{
    if (!(m_screenTolerancePx > 0.0))
    {
        return 0.0;
    }

    // Clamped because a scale of zero is not a real viewport state, and
    // dividing by it would turn a two-pixel slack into an infinite one and make
    // every target a hit.
    return m_screenTolerancePx / qMax(m_viewScale, qreal(0.01));
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
    return hitTestAll(pageIndex, pagePoint, nullptr);
}

QList<InteractionTarget> HitTestDispatcher::hitTestAll(int pageIndex,
                                                       QPointF pagePoint,
                                                       HitTestCounters* counters) const
{
    QList<InteractionTarget> candidates;

    if (counters)
    {
        *counters = HitTestCounters();
    }

    if (pageIndex < 0)
    {
        return candidates;
    }

    // One conversion, here, for every source. A source that divided by zoom
    // itself would be a second place for this to go stale.
    const qreal tolerance = pageTolerance();

    for (const InteractionTarget& handle : m_handles)
    {
        if (handle.pageIndex == pageIndex &&
            handle.pageBounds.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(pagePoint))
        {
            candidates.push_back(handle);
        }
    }

    if (counters)
    {
        // Handles are a short caller-supplied list, scanned exactly. They are
        // still candidates, so they are counted rather than being invisible in
        // a trace that claims to account for the whole pass.
        counters->indexCandidates += int(m_handles.size());
        counters->preciseHits += int(candidates.size());
    }

    for (const IHitTestSource* source : m_sources)
    {
        candidates.append(source->hitTest(pageIndex, pagePoint, tolerance, counters));
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
    return hitTest(pageIndex, pagePoint, nullptr);
}

InteractionTarget HitTestDispatcher::hitTest(int pageIndex, QPointF pagePoint, HitTestCounters* counters) const
{
    const QList<InteractionTarget> candidates = hitTestAll(pageIndex, pagePoint, counters);
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
