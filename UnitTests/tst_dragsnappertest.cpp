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

#include "dragsnapper.h"

#include <QtTest>

using pdfinteraction::DragSnapper;
using pdfinteraction::ISnapProvider;
using pdfinteraction::SnapCandidate;

namespace
{

/// Candidates a test wrote down, so the ranking rules are provable without a
/// document. PageBoxSnapProvider is the one real provider and is exercised
/// against a document in tst_editorhosttest.cpp; what this file pins is
/// DragSnapper own arithmetic.
class ScriptedSnapProvider final : public ISnapProvider
{
public:
    QList<SnapCandidate> snapCandidates(int pageIndex, const QRectF& probePageRect) const override
    {
        lastProbe = probePageRect;
        ++probeCount;

        QList<SnapCandidate> visible;

        for (const SnapCandidate& candidate : candidates)
        {
            if (pageIndex == page && probePageRect.contains(candidate.pagePoint))
            {
                visible.push_back(candidate);
            }
        }

        return visible;
    }

    QList<SnapCandidate> candidates;
    int page = 0;

    mutable QRectF lastProbe;
    mutable int probeCount = 0;
};

SnapCandidate makeCandidate(QPointF point, const QString& sourceId)
{
    return SnapCandidate{ point, sourceId };
}

bool pointsEqual(QPointF actual, QPointF expected)
{
    return qFuzzyCompare(actual.x() + 1.0, expected.x() + 1.0) && qFuzzyCompare(actual.y() + 1.0, expected.y() + 1.0);
}

}   // namespace

/// Issue #145: a drag must come to rest predictably. The snap threshold is a
/// screen quantity like the hit tolerance, so it has to survive zoom; the
/// winner has to be deterministic rather than dependent on provider
/// registration order; and "did not snap" has to be distinguishable from
/// "snapped onto the point the pointer was already at".
class DragSnapperTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void nearestCandidateWithinThresholdWins();
    void tiesResolveByLowestSourceIdRegardlessOfProviderOrder();
    void nothingWithinThresholdLeavesThePointAndReportsNoSource();
    void snapOntoTheCurrentPointStillReportsItsSource();
    void thresholdIsScreenSpaceAndShrinksWithZoom();
    void disabledSnapperAndEmptyProviderSetAreNoOps();
    void negativePageIndexIsNotProbed();
    void zeroThresholdSnapsNothing();
};

void DragSnapperTest::nearestCandidateWithinThresholdWins()
{
    ScriptedSnapProvider provider;
    provider.candidates = { makeCandidate(QPointF(104.0, 100.0), QStringLiteral("far")),
                            makeCandidate(QPointF(101.0, 100.0), QStringLiteral("near")) };

    DragSnapper snapper;
    snapper.addProvider(&provider);
    snapper.setScreenThresholdPx(8.0);

    QString snappedTo;
    const QPointF result = snapper.snap(0, QPointF(100.0, 100.0), 1.0, &snappedTo);

    QVERIFY(pointsEqual(result, QPointF(101.0, 100.0)));
    QCOMPARE(snappedTo, QStringLiteral("near"));
}

void DragSnapperTest::tiesResolveByLowestSourceIdRegardlessOfProviderOrder()
{
    // Two candidates at exactly the same distance. Without a tie-break the
    // answer would depend on which provider was registered first, which is the
    // determinism rule HitTestDispatcher applies to hit candidates.
    ScriptedSnapProvider alpha;
    alpha.candidates = { makeCandidate(QPointF(102.0, 100.0), QStringLiteral("art-box")) };

    ScriptedSnapProvider beta;
    beta.candidates = { makeCandidate(QPointF(98.0, 100.0), QStringLiteral("trim-box")) };

    QString forwardSource;
    {
        DragSnapper snapper;
        snapper.addProvider(&alpha);
        snapper.addProvider(&beta);
        snapper.setScreenThresholdPx(8.0);
        snapper.snap(0, QPointF(100.0, 100.0), 1.0, &forwardSource);
    }

    QString reversedSource;
    {
        DragSnapper snapper;
        snapper.addProvider(&beta);
        snapper.addProvider(&alpha);
        snapper.setScreenThresholdPx(8.0);
        snapper.snap(0, QPointF(100.0, 100.0), 1.0, &reversedSource);
    }

    QCOMPARE(forwardSource, QStringLiteral("art-box"));
    QCOMPARE(reversedSource, forwardSource);
}

void DragSnapperTest::nothingWithinThresholdLeavesThePointAndReportsNoSource()
{
    ScriptedSnapProvider provider;
    provider.candidates = { makeCandidate(QPointF(140.0, 140.0), QStringLiteral("media-box")) };

    DragSnapper snapper;
    snapper.addProvider(&provider);
    snapper.setScreenThresholdPx(8.0);

    QString snappedTo;
    const QPointF result = snapper.snap(0, QPointF(100.0, 100.0), 1.0, &snappedTo);

    QVERIFY(pointsEqual(result, QPointF(100.0, 100.0)));
    QVERIFY(snappedTo.isEmpty());
}

void DragSnapperTest::snapOntoTheCurrentPointStillReportsItsSource()
{
    // The returned point is identical to the input, so a caller comparing
    // points would conclude nothing happened. The source id is the only thing
    // that separates the two cases, which is why DragSession records it rather
    // than re-deriving it later.
    ScriptedSnapProvider provider;
    provider.candidates = { makeCandidate(QPointF(100.0, 100.0), QStringLiteral("crop-box")) };

    DragSnapper snapper;
    snapper.addProvider(&provider);
    snapper.setScreenThresholdPx(8.0);

    QString snappedTo;
    const QPointF result = snapper.snap(0, QPointF(100.0, 100.0), 1.0, &snappedTo);

    QVERIFY(pointsEqual(result, QPointF(100.0, 100.0)));
    QCOMPARE(snappedTo, QStringLiteral("crop-box"));
}

void DragSnapperTest::thresholdIsScreenSpaceAndShrinksWithZoom()
{
    // 4 page units away. At zoom 1.0 an 8 px threshold reaches it; at zoom 4.0
    // the same 8 px is 2 page units and no longer does. A snap that stayed
    // constant in page units would get easier as the user zoomed in, at exactly
    // the zoom where they are asking for precision.
    ScriptedSnapProvider provider;
    provider.candidates = { makeCandidate(QPointF(104.0, 100.0), QStringLiteral("trim-box")) };

    DragSnapper snapper;
    snapper.addProvider(&provider);
    snapper.setScreenThresholdPx(8.0);

    QString atUnitZoom;
    const QPointF unitZoomResult = snapper.snap(0, QPointF(100.0, 100.0), 1.0, &atUnitZoom);
    QCOMPARE(atUnitZoom, QStringLiteral("trim-box"));
    QVERIFY(pointsEqual(unitZoomResult, QPointF(104.0, 100.0)));

    QString atHighZoom;
    const QPointF highZoomResult = snapper.snap(0, QPointF(100.0, 100.0), 4.0, &atHighZoom);
    QVERIFY(atHighZoom.isEmpty());
    QVERIFY(pointsEqual(highZoomResult, QPointF(100.0, 100.0)));

    // The probe the provider saw narrows with the zoom too, so a provider is
    // never asked to consider geometry the snapper would reject anyway.
    QCOMPARE(provider.lastProbe.width(), 4.0);
}

void DragSnapperTest::disabledSnapperAndEmptyProviderSetAreNoOps()
{
    ScriptedSnapProvider provider;
    provider.candidates = { makeCandidate(QPointF(101.0, 100.0), QStringLiteral("trim-box")) };

    DragSnapper disabled;
    disabled.addProvider(&provider);
    disabled.setScreenThresholdPx(8.0);
    disabled.setEnabled(false);

    QString snappedTo;
    QVERIFY(pointsEqual(disabled.snap(0, QPointF(100.0, 100.0), 1.0, &snappedTo), QPointF(100.0, 100.0)));
    QVERIFY(snappedTo.isEmpty());

    // Disabled means not consulted, not consulted-and-discarded.
    QCOMPARE(provider.probeCount, 0);

    DragSnapper empty;
    empty.setScreenThresholdPx(8.0);
    QVERIFY(pointsEqual(empty.snap(0, QPointF(100.0, 100.0), 1.0, &snappedTo), QPointF(100.0, 100.0)));
    QVERIFY(snappedTo.isEmpty());
}

void DragSnapperTest::negativePageIndexIsNotProbed()
{
    ScriptedSnapProvider provider;
    provider.candidates = { makeCandidate(QPointF(100.0, 100.0), QStringLiteral("trim-box")) };

    DragSnapper snapper;
    snapper.addProvider(&provider);
    snapper.setScreenThresholdPx(8.0);

    QString snappedTo;
    QVERIFY(pointsEqual(snapper.snap(-1, QPointF(100.0, 100.0), 1.0, &snappedTo), QPointF(100.0, 100.0)));
    QVERIFY(snappedTo.isEmpty());
    QCOMPARE(provider.probeCount, 0);
}

void DragSnapperTest::zeroThresholdSnapsNothing()
{
    // Zero is how snapping is turned off by configuration rather than by the
    // enabled flag. A zero-radius probe that still matched an exactly-coincident
    // candidate would make the two knobs disagree.
    ScriptedSnapProvider provider;
    provider.candidates = { makeCandidate(QPointF(100.0, 100.0), QStringLiteral("trim-box")) };

    DragSnapper snapper;
    snapper.addProvider(&provider);
    snapper.setScreenThresholdPx(0.0);

    QString snappedTo;
    QVERIFY(pointsEqual(snapper.snap(0, QPointF(100.0, 100.0), 1.0, &snappedTo), QPointF(100.0, 100.0)));
    QVERIFY(snappedTo.isEmpty());
    QCOMPARE(provider.probeCount, 0);
}

QTEST_MAIN(DragSnapperTest)
#include "tst_dragsnappertest.moc"
