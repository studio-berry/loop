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
#include "pagespatialindex.h"

#include "pdfevidencegraph.h"

#include <QSet>
#include <QtTest>

using pdfinteraction::EvidenceHitTestSource;
using pdfinteraction::FindingListHitTestSource;
using pdfinteraction::HitTestDispatcher;
using pdfinteraction::InteractionTarget;
using pdfinteraction::InteractionTargetKind;
using pdfinteraction::PageSpatialIndex;

namespace
{

pdf::PDFEvidenceRecord makeRecord(const QString& id, int page, const QRectF& geometry)
{
    pdf::PDFEvidenceRecord record;
    record.id = id;
    record.page = page;
    record.geometry = geometry;
    return record;
}

InteractionTarget makeFindingTarget(const QString& id, int pageIndex, const QRectF& bounds)
{
    InteractionTarget target;
    target.kind = InteractionTargetKind::Finding;
    target.pageIndex = pageIndex;
    target.id = id;
    target.pageBounds = bounds;
    return target;
}

}   // namespace

/// Issue #145: hit testing must query a spatial index for candidates rather
/// than scanning every object on every pointer move, and the interaction
/// grammar (precedence, precise geometry, revision-safe rebuilds) must keep
/// working once it does. `UnitTests/tst_preflightinteraction.cpp` already
/// exercises `HitTestDispatcher` end to end through the preflight overlay
/// path; this file is the focused counterpart for the index itself and the
/// sources that now use it.
class HitTestSourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void spatialIndexHandlesEmptyInput();
    void spatialIndexHandlesDegenerateExtent();
    void spatialIndexRejectsPointsOutsideExtent();
    void spatialIndexNarrowsCandidatesForLargeSpreadObjectCounts();
    void spatialIndexReturnsOverlappingCandidates();
    void spatialIndexBoundsWideRectangleStorage();

    void evidenceSourceHitTestsIdenticalToLinearScan();
    void evidenceSourceHandlesEmptyPage();
    void evidenceSourceHandlesOffPageObjects();
    void evidenceSourceRebuildsIndexOnGraphReplacement();

    void findingListSourceHitTestsIdenticalToLinearScan();

    void dispatcherPrecedenceUnaffectedByIndexing();
};

void HitTestSourceTest::spatialIndexHandlesEmptyInput()
{
    PageSpatialIndex index;
    index.build({});
    QCOMPARE(index.itemCount(), 0);
    QVERIFY(index.query(QPointF(0, 0)).isEmpty());
}

void HitTestSourceTest::spatialIndexHandlesDegenerateExtent()
{
    // Every item collapses onto the same point: AC8's "degenerate bounds"
    // case. The grid cannot subdivide a zero-area extent, so it must fall
    // back to one cell rather than dropping items or crashing.
    PageSpatialIndex index;
    const QList<QRectF> bounds = {
        QRectF(10.0, 10.0, 0.0, 0.0),
        QRectF(10.0, 10.0, 0.0, 0.0),
        QRectF(10.0, 10.0, 0.0, 0.0),
    };
    index.build(bounds);
    QCOMPARE(index.itemCount(), 3);

    const QList<int> hits = index.query(QPointF(10.0, 10.0));
    QCOMPARE(hits.size(), 3);
}

void HitTestSourceTest::spatialIndexRejectsPointsOutsideExtent()
{
    PageSpatialIndex index;
    index.build({ QRectF(0, 0, 10, 10), QRectF(20, 20, 10, 10) });
    QVERIFY(index.query(QPointF(-5.0, -5.0)).isEmpty());
    QVERIFY(index.query(QPointF(1000.0, 1000.0)).isEmpty());
}

void HitTestSourceTest::spatialIndexNarrowsCandidatesForLargeSpreadObjectCounts()
{
    // AC8's "very large object counts", and the substance of AC1: a query
    // must not hand back the whole document as "candidates". 2500 disjoint
    // 1x1 boxes spread across a 500x500 page, on a roughly 50x50 grid --
    // touching one cell should return a small handful of items, not
    // thousands.
    PageSpatialIndex index;
    QList<QRectF> bounds;
    constexpr int Side = 50;
    for (int x = 0; x < Side; ++x)
    {
        for (int y = 0; y < Side; ++y)
        {
            bounds.push_back(QRectF(x * 10.0, y * 10.0, 1.0, 1.0));
        }
    }
    index.build(bounds);
    QCOMPARE(index.itemCount(), Side * Side);

    const QList<int> hits = index.query(QPointF(255.5, 255.5));
    QVERIFY(!hits.isEmpty());
    QVERIFY2(hits.size() < bounds.size() / 10,
             qPrintable(QStringLiteral("expected a narrowed candidate set, got %1 of %2").arg(hits.size()).arg(bounds.size())));
}

void HitTestSourceTest::spatialIndexReturnsOverlappingCandidates()
{
    // AC8's "overlapping objects": a point inside two overlapping rects must
    // surface both as candidates -- the index must not treat cell placement
    // as exclusive.
    PageSpatialIndex index;
    index.build({ QRectF(0, 0, 20, 20), QRectF(10, 10, 20, 20) });
    const QList<int> hits = index.query(QPointF(15.0, 15.0));
    QCOMPARE(hits.size(), 2);
    QVERIFY(hits.contains(0));
    QVERIFY(hits.contains(1));
}

void HitTestSourceTest::spatialIndexBoundsWideRectangleStorage()
{
    // A full-page finding must not be copied into every grid cell. The
    // overflow bucket keeps the candidate set correct while its storage stays
    // proportional to the number of wide findings.
    PageSpatialIndex index;
    QList<QRectF> bounds;
    bounds.reserve(10000);
    for (int i = 0; i < 10000; ++i)
    {
        bounds.push_back(QRectF(0.0, 0.0, 1000.0, 1000.0));
    }

    index.build(bounds);
    const QList<int> hits = index.query(QPointF(500.0, 500.0));
    QCOMPARE(hits.size(), bounds.size());
    QSet<int> unique;
    for (int hit : hits)
    {
        unique.insert(hit);
    }
    QCOMPARE(unique.size(), bounds.size());
}

void HitTestSourceTest::evidenceSourceHitTestsIdenticalToLinearScan()
{
    pdf::PDFEvidenceGraph graph;
    for (int i = 0; i < 400; ++i)
    {
        const qreal x = (i % 20) * 15.0;
        const qreal y = (i / 20) * 15.0;
        graph.records.push_back(makeRecord(QStringLiteral("e-%1").arg(i), 1, QRectF(x, y, 10.0, 10.0)));
    }
    // A handful of overlapping records, so the precise-test path is
    // exercised alongside the index narrowing.
    graph.records.push_back(makeRecord(QStringLiteral("overlap-a"), 1, QRectF(5.0, 5.0, 8.0, 8.0)));
    graph.records.push_back(makeRecord(QStringLiteral("overlap-b"), 1, QRectF(8.0, 8.0, 8.0, 8.0)));

    EvidenceHitTestSource source(graph);

    const auto idsAt = [&](QPointF point)
    {
        QStringList ids;
        for (const InteractionTarget& target : source.hitTest(0, point))
        {
            ids.push_back(target.id);
        }
        ids.sort();
        return ids;
    };

    // (0, 0) box plus the two overlapping records.
    QCOMPARE(idsAt(QPointF(6.0, 6.0)), (QStringList{ QStringLiteral("e-0"), QStringLiteral("overlap-a") }));
    QCOMPARE(idsAt(QPointF(9.0, 9.0)),
             (QStringList{ QStringLiteral("e-0"), QStringLiteral("overlap-a"), QStringLiteral("overlap-b") }));
    QVERIFY(idsAt(QPointF(-1.0, -1.0)).isEmpty());
}

void HitTestSourceTest::evidenceSourceHandlesEmptyPage()
{
    pdf::PDFEvidenceGraph graph;
    graph.records.push_back(makeRecord(QStringLiteral("e-1"), 1, QRectF(0, 0, 10, 10)));
    EvidenceHitTestSource source(graph);

    // Page 2 (index 1) has no records at all -- must not touch a
    // never-built index for that page and must not crash.
    QVERIFY(source.hitTest(1, QPointF(5.0, 5.0)).isEmpty());
    QVERIFY(source.targetsForPage(1).isEmpty());
}

void HitTestSourceTest::evidenceSourceHandlesOffPageObjects()
{
    // A record with a negative page number has no valid 0-based index and
    // must not be indexed under a fabricated page, nor crash indexGraph().
    pdf::PDFEvidenceGraph graph;
    graph.records.push_back(makeRecord(QStringLiteral("bad-page"), 0, QRectF(0, 0, 10, 10)));
    graph.records.push_back(makeRecord(QStringLiteral("good"), 1, QRectF(0, 0, 10, 10)));

    EvidenceHitTestSource source(graph);
    QCOMPARE(source.unrenderableRecordCount(), 1);
    QCOMPARE(source.hitTest(0, QPointF(5.0, 5.0)).size(), 1);
    QCOMPARE(source.hitTest(0, QPointF(5.0, 5.0)).constFirst().id, QStringLiteral("good"));
}

void HitTestSourceTest::evidenceSourceRebuildsIndexOnGraphReplacement()
{
    // AC6: the index must be safe -- and correct -- across a revision
    // replacement, not stale from the previous graph.
    pdf::PDFEvidenceGraph first;
    first.records.push_back(makeRecord(QStringLiteral("first"), 1, QRectF(0, 0, 10, 10)));
    EvidenceHitTestSource source(first);
    QCOMPARE(source.hitTest(0, QPointF(5.0, 5.0)).size(), 1);

    pdf::PDFEvidenceGraph second;
    second.records.push_back(makeRecord(QStringLiteral("second"), 1, QRectF(20, 20, 10, 10)));
    source.setGraph(second);

    QVERIFY(source.hitTest(0, QPointF(5.0, 5.0)).isEmpty());
    const QList<InteractionTarget> hits = source.hitTest(0, QPointF(25.0, 25.0));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.constFirst().id, QStringLiteral("second"));
}

void HitTestSourceTest::findingListSourceHitTestsIdenticalToLinearScan()
{
    FindingListHitTestSource source;
    QList<InteractionTarget> targets;
    for (int i = 0; i < 200; ++i)
    {
        const qreal x = (i % 10) * 20.0;
        const qreal y = (i / 10) * 20.0;
        targets.push_back(makeFindingTarget(QStringLiteral("f-%1").arg(i), 0, QRectF(x, y, 12.0, 12.0)));
    }
    targets.push_back(makeFindingTarget(QStringLiteral("other-page"), 1, QRectF(0, 0, 12, 12)));
    source.setTargets(targets);

    const QList<InteractionTarget> hits = source.hitTest(0, QPointF(6.0, 6.0));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.constFirst().id, QStringLiteral("f-0"));
    QVERIFY(source.hitTest(1, QPointF(500.0, 500.0)).isEmpty());
}

void HitTestSourceTest::dispatcherPrecedenceUnaffectedByIndexing()
{
    // Guards against the index changing *what* is returned, not just how
    // efficiently -- HitTestDispatcher's precedence contract (issue #143
    // AC3) must hold identically once its sources are index-backed.
    pdf::PDFEvidenceGraph graph;
    graph.records.push_back(makeRecord(QStringLiteral("outer"), 1, QRectF(0, 0, 20, 20)));
    graph.records.push_back(makeRecord(QStringLiteral("inner"), 1, QRectF(5, 5, 5, 5)));
    EvidenceHitTestSource evidence(graph);

    FindingListHitTestSource findings;
    findings.setTargets({ makeFindingTarget(QStringLiteral("finding"), 0, QRectF(2, 2, 4, 4)) });

    HitTestDispatcher dispatcher;
    dispatcher.addSource(&evidence);
    dispatcher.addSource(&findings);

    // At (3, 3): "outer" (0,0..20,20) and "finding" (2,2..6,6) both contain
    // the point; "inner" (5,5..10,10) does not. Both winners share
    // InteractionTargetKind::Finding, so the deciding rule is smallest area,
    // not source registration order (reversed below to prove that
    // independence still holds).
    const InteractionTarget winner = dispatcher.hitTest(0, QPointF(3.0, 3.0));
    QCOMPARE(winner.id, QStringLiteral("finding"));

    HitTestDispatcher reordered;
    reordered.addSource(&findings);
    reordered.addSource(&evidence);
    QCOMPARE(reordered.hitTest(0, QPointF(3.0, 3.0)).id, QStringLiteral("finding"));
}

QTEST_MAIN(HitTestSourceTest)
#include "tst_hittestsourcetest.moc"
