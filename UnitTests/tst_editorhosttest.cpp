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

#include <QtTest>

#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <memory>

#include "documentviewsession.h"
#include "editorhost.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfworkloadenvelope.h"

#include "dragsnapper.h"
#include "hittestsource.h"
#include "inputintent.h"
#include "interactioncontroller.h"
#include "pagesurfacecoordinator.h"
#include "viewportcontroller.h"

namespace
{

/// Minimal scripted source for fencing proof, mirroring tst_interactioncontrollertest.
/// Returns targets the test wrote down so hover transitions are deterministic
/// without a corpus.
class ScriptedHitTestSource final : public pdfinteraction::IHitTestSource
{
public:
    QList<pdfinteraction::InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const override
    {
        QList<pdfinteraction::InteractionTarget> hits;
        for (const pdfinteraction::InteractionTarget& target : targets)
        {
            if (target.pageIndex == pageIndex && target.pageBounds.contains(pagePoint))
            {
                hits.push_back(target);
            }
        }
        return hits;
    }

    QList<pdfinteraction::InteractionTarget> targets;
};

pdfinteraction::InteractionTarget makeFindingTarget(const QString& id, const QRectF& bounds, int pageIndex = 0)
{
    pdfinteraction::InteractionTarget target;
    target.kind = pdfinteraction::InteractionTargetKind::Finding;
    target.pageIndex = pageIndex;
    target.id = id;
    target.pageBounds = bounds;
    return target;
}

pdfinteraction::PointerIntent makeMoveIntent(QPoint positionPx, quint64 sequence)
{
    pdfinteraction::PointerIntent intent;
    intent.stamp.sequence = sequence;
    intent.stamp.monotonicNs = qint64(sequence) * 1000000;
    intent.action = pdfinteraction::PointerAction::Move;
    intent.positionPx = positionPx;
    intent.button = Qt::NoButton;
    intent.buttons = Qt::NoButton;
    intent.modifiers = Qt::NoModifier;
    return intent;
}

}   // namespace

class EditorHostTest : public QObject
{
    Q_OBJECT

private slots:
    void startsWithNoDocument();
    void exposesCatalogDescriptorsWithoutMutating();
    void navigationCommandsStayDisabledUntilOpen();
    void sessionTeardownDrainsWorkersBeforeAdapters();
    void pageBoxSnapProviderOffersCornersOfTheHittableBoxes();
    void openLargeDocument();
};

void EditorHostTest::startsWithNoDocument()
{
    EditorHost host;
    QCOMPARE(host.documentState(), QStringLiteral("empty"));
    QVERIFY(!host.hasDocument());
    QCOMPARE(host.pageCount(), 0);
}

void EditorHostTest::exposesCatalogDescriptorsWithoutMutating()
{
    EditorHost host;
    const QVariantList descriptors = host.commandDescriptors();
    QVERIFY(descriptors.size() > 100);

    bool sawOpen = false;
    for (const QVariant& entryVariant : descriptors)
    {
        const QVariantMap entry = entryVariant.toMap();
        if (entry.value(QStringLiteral("id")).toString() == QStringLiteral("actionOpen"))
        {
            sawOpen = true;
            QVERIFY(entry.value(QStringLiteral("implemented")).toBool());
            QVERIFY(!host.shortcutForCommand(QStringLiteral("actionOpen")).isEmpty() || entry.contains(QStringLiteral("shortcut")));
        }
    }
    QVERIFY(sawOpen);
}

void EditorHostTest::navigationCommandsStayDisabledUntilOpen()
{
    EditorHost host;
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionGoToNextPage")));
    QCOMPARE(host.invokeCommand(QStringLiteral("actionGoToNextPage")), quint64(0));
}

void EditorHostTest::sessionTeardownDrainsWorkersBeforeAdapters()
{
    auto session = std::make_unique<DocumentViewSession>();
    DocumentViewSession* rawSession = session.get();
    std::atomic_bool started = false;
    std::atomic_bool adapterReached = false;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Background;
    rawSession->scheduler().submit(spec,
                                   [rawSession, &started, &adapterReached](pdf::PDFJobContext& context)
                                   {
                                       started.store(true, std::memory_order_release);
                                       while (!context.isCancellationRequested())
                                       {
                                           QThread::yieldCurrentThread();
                                       }

                                       // The session destructor must join this
                                       // work before destroying the renderer.
                                       rawSession->renderer().shedPrefetchAndQuality();
                                       adapterReached.store(true, std::memory_order_release);
                                   });

    QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 1000);
    session.reset();
    QVERIFY(adapterReached.load(std::memory_order_acquire));
}

void EditorHostTest::pageBoxSnapProviderOffersCornersOfTheHittableBoxes()
{
    // Issue #145: the one snap provider that ships reads its candidates from
    // PageBoxHitTestSource, the same source the dispatcher hit-tests. Reading
    // pdf::PDFPage a second time instead would let a snap target drift away
    // from the edge the user can actually click.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QRectF mediaBox(0.0, 0.0, 612.0, 792.0);

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(mediaBox);

    const QString path = directory.filePath(QStringLiteral("boxes.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);

    const pdfinteraction::PageBoxHitTestSource& boxes = session->pageBoxSource();
    const QList<pdfinteraction::InteractionTarget> targets = boxes.targetsForPage(0);
    QVERIFY(!targets.isEmpty());

    pdfinteraction::PageBoxSnapProvider provider(&boxes);

    // A probe around one corner of the outermost box returns that corner, named
    // by the box it came from.
    const QPointF corner = targets.constFirst().pageBounds.topLeft();
    const QRectF probe(corner.x() - 5.0, corner.y() - 5.0, 10.0, 10.0);

    const QList<pdfinteraction::SnapCandidate> nearCorner = provider.snapCandidates(0, probe);
    QVERIFY(!nearCorner.isEmpty());

    bool sawTheCorner = false;
    for (const pdfinteraction::SnapCandidate& candidate : nearCorner)
    {
        QVERIFY(!candidate.sourceId.isEmpty());

        if (candidate.pagePoint == corner && candidate.sourceId == targets.constFirst().id)
        {
            sawTheCorner = true;
        }
    }
    QVERIFY(sawTheCorner);

    // Every candidate is a corner of a box the source itself reports; the
    // provider invents no geometry of its own.
    for (const pdfinteraction::SnapCandidate& candidate : nearCorner)
    {
        bool matched = false;

        for (const pdfinteraction::InteractionTarget& target : targets)
        {
            const QRectF bounds = target.pageBounds;

            if (candidate.pagePoint == bounds.topLeft() || candidate.pagePoint == bounds.topRight() ||
                candidate.pagePoint == bounds.bottomLeft() || candidate.pagePoint == bounds.bottomRight())
            {
                matched = true;
                break;
            }
        }

        QVERIFY(matched);
    }

    // Corners only: the middle of a box edge is not a candidate, because a
    // point-snap onto a line needs a projection rule this provider does not
    // claim to have.
    const QPointF edgeMidpoint(mediaBox.center().x(), mediaBox.top());
    QVERIFY(provider.snapCandidates(0, QRectF(edgeMidpoint.x() - 5.0, edgeMidpoint.y() - 5.0, 10.0, 10.0)).isEmpty());

    // A page that does not exist yields nothing rather than reaching past the
    // catalog.
    QVERIFY(provider.snapCandidates(-1, probe).isEmpty());
    QVERIFY(provider.snapCandidates(99, probe).isEmpty());
}

void EditorHostTest::openLargeDocument()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    constexpr int pageCount = 1000;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 612, 792));
    }

    const QString path = directory.filePath(QStringLiteral("large.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    QElapsedTimer openTimer;
    openTimer.start();

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 90000);
    const qint64 openToFirstViewMs = openTimer.elapsed();

    QCOMPARE(host.pageCount(), pageCount);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionGoToDocumentEnd")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionGoToDocumentEnd")) != 0);
    QTRY_COMPARE_WITH_TIMEOUT(host.currentPage(), pageCount - 1, 15000);

    // --- Interaction revision and surface fencing under load (gh-363) ---
    // EditorHost owns DocumentViewSession privately; sessionForTest() is the
    // minimal test-only accessor that lets the shell stress test observe
    // revision/viewport/surfaces/interaction without breaking QML encapsulation.
    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    QVERIFY(session->revisionSource() != nullptr);
    QVERIFY(session->surfaces() != nullptr);
    QVERIFY(session->interaction() != nullptr);

    // Ensure the viewport is in a deterministic state for hit-testing. The
    // host was already given 1024x768 above; re-apply to guarantee
    // requestGeneration is stable before we snapshot it.
    // Note: setViewportGeometry is idempotent when the values are unchanged,
    // so this does not bump generation if already set.
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);

    // The shell's Quick host without a LoupeCanvasItem leaves
    // DocumentViewSession::hitTest() empty (EditorHost::bindCanvas() is the
    // only place that adds PageBoxHitTestSource). For a fencing proof we need
    // a source that can produce overlay-only hover transitions. Inject a
    // scripted finding that covers a small rect around the viewport center so
    // pointer sweeps can enter/leave it without advancing generation.
    pdfinteraction::ViewportController& viewport = session->viewport();
    QVERIFY(viewport.pageCount() == pageCount);
    QRect placed = viewport.placedPageRect(viewport.currentPage());
    // Fallback to page 0 if current page placement is empty (should not happen after navigation).
    if (placed.isEmpty())
    {
        placed = viewport.placedPageRect(0);
    }
    QVERIFY(!placed.isEmpty());

    const QPoint viewportCenter = placed.center();
    const std::optional<QPointF> optCenterPage = viewport.viewportToPagePoint(viewportCenter, viewport.currentPage());
    // Center of a 612x792 page is approx 306x396. Use that as fallback if the
    // inverse mapping fails (e.g., due to rounding).
    const QPointF centerPagePoint = optCenterPage.value_or(QPointF(306.0, 396.0));

    // Finding bounds: 30x30 square centered at the page center point, guaranteed
    // to be inside the page's media box and inside the viewport after mapping.
    const QRectF findingBounds(centerPagePoint.x() - 15.0, centerPagePoint.y() - 15.0, 30.0, 30.0);
    ScriptedHitTestSource scripted;
    scripted.targets.push_back(makeFindingTarget(QStringLiteral("stress-finding-1"), findingBounds, viewport.currentPage()));
    // Also expose the page-box source for completeness; edge hits are another
    // overlay-only path but not required for the assertion.
    session->hitTest()->addSource(&scripted);
    session->hitTest()->addSource(&session->pageBoxSource());

    const pdf::PDFRevisionIdentity revisionBefore = session->revisionSource()->currentRevision();
    const quint64 generationBefore = viewport.requestGeneration();
    const int requestedBefore = session->surfaces()->counters().requested;
    const qint64 admittedHighWaterBefore = session->surfaces()->counters().admittedBytesHighWater;

    QSignalSpy overlaySpy(session->interaction(), &pdfinteraction::InteractionController::overlayFrameChanged);
    QSignalSpy viewportSpy(session->interaction(), &pdfinteraction::InteractionController::viewportChanged);
    QSignalSpy hoverSpy(session->interaction(), &pdfinteraction::InteractionController::hoverChanged);
    QVERIFY(overlaySpy.isValid());
    QVERIFY(viewportSpy.isValid());

    // Helper: page point -> viewport pixel via the same matrix the surfaces use.
    // This keeps the test's arithmetic from diverging from the controller's.
    auto viewportPointFor = [&viewport](QPointF pagePoint, int pageIndex) -> QPoint
    {
        const QTransform matrix = viewport.pagePointToViewportMatrix(pageIndex);
        return matrix.map(pagePoint).toPoint();
    };

    const int currentPageIndex = viewport.currentPage();
    // Perform 80 pointer Move sweeps that alternately hit and miss the scripted
    // finding. Hover changes are overlay-only: they must produce overlay frames
    // but never advance requestGeneration or surface demand.
    constexpr int sweepSteps = 80;
    for (int step = 0; step < sweepSteps; ++step)
    {
        // Sweep horizontally across the finding: offset -40..+39 scaled by 2,
        // so the pointer travels ~160 page units centered on the finding.
        // Steps ~33..47 hit the 30-unit finding, others miss, yielding at least
        // 2 hover transitions (enter + leave) and thus >=2 overlay frames.
        const qreal offset = qreal(step) - 40.0;
        const QPointF pagePoint(centerPagePoint.x() + offset * 2.0, centerPagePoint.y());
        const QPoint viewportPoint = viewportPointFor(pagePoint, currentPageIndex);
        session->interaction()->handlePointer(makeMoveIntent(viewportPoint, quint64(step + 1)));
    }

    // Fencing holds: no document revision change, no viewport generation bump,
    // no new surface requests, no renderer invocation via surfaces. The sweep
    // did produce overlay work.
    QCOMPARE(session->revisionSource()->currentRevision(), revisionBefore);
    QCOMPARE(viewport.requestGeneration(), generationBefore);
    QCOMPARE(session->surfaces()->counters().requested, requestedBefore);
    // Admitted high water should not have grown due to overlay-only input.
    QCOMPARE(session->surfaces()->counters().admittedBytesHighWater, admittedHighWaterBefore);
    QCOMPARE(viewportSpy.size(), 0);
    // At least enter + leave of the finding, plus potentially Page fallback
    // transitions. The exact count depends on hit-test tie-break but is >=2.
    QVERIFY(overlaySpy.size() >= 2);
    // hoverChanged is emitted only when the hovered target actually changes;
    // sweeping across the finding should have produced at least 2 changes.
    QVERIFY(hoverSpy.size() >= 2);

    // Also verify that surfaces remain fenced at the coordinator's generation:
    // overlay-only hover must not have advanced the PageSurfaceCoordinator's
    // own generation (which tracks demand supersession). The coordinator's
    // generation is independent of the viewport's but is similarly only
    // advanced by requestSurfaces/invalidate, not by interaction.
    const quint64 coordinatorGenerationBefore = session->surfaces()->generation();
    // A second smaller sweep to ensure coordinator generation still stable.
    for (int step = 0; step < 20; ++step)
    {
        const QPointF pagePoint(centerPagePoint.x() + qreal(step), centerPagePoint.y() + 10.0);
        const QPoint viewportPoint = viewportPointFor(pagePoint, currentPageIndex);
        session->interaction()->handlePointer(makeMoveIntent(viewportPoint, quint64(sweepSteps + step + 1)));
    }
    QCOMPARE(session->surfaces()->generation(), coordinatorGenerationBefore);
    QCOMPARE(viewport.requestGeneration(), generationBefore);
    QCOMPARE(session->revisionSource()->currentRevision(), revisionBefore);

    // Optional envelope: when LOUPE_STRESS_ENVELOPE is set, record
    // PDFWorkloadEnvelope identity and timings for observability. This is
    // opt-in so CI without the env var does not pay the cost of JSON
    // assertions, but when enabled it proves the 1k-page shell open is
    // measurable and the envelope contract holds.
    const QByteArray envelopeEnv = qgetenv("LOUPE_STRESS_ENVELOPE");
    if (!envelopeEnv.isEmpty())
    {
        const bool envelopeEnabled = envelopeEnv == QByteArrayLiteral("1") || envelopeEnv.toLower() == QByteArrayLiteral("true");
        if (envelopeEnabled)
        {
            pdf::PDFWorkloadEnvelope envelope;
            envelope.identity = pdf::PDFRunIdentity::capture();
            envelope.family = QStringLiteral("shell-stress-1k");
            envelope.status = QStringLiteral("complete");
            envelope.pageCount = pageCount;
            envelope.openToFirstViewMs = openToFirstViewMs;
            envelope.rssHighWaterBytes = pdf::PDFWorkloadEnvelope::currentRssHighWaterBytes();
            envelope.cacheHighWaterBytes = session->surfaces()->counters().admittedBytesHighWater;
            envelope.pressureShedCount = session->surfaces()->counters().shed;
            envelope.elapsedMs = openTimer.elapsed();
            envelope.prefetchShed = session->surfaces()->counters().shed > 0;
            // Interaction slot was held throughout the sweep without blocking.
            envelope.interactionSlotHeld = true;

            const QJsonObject json = envelope.toJson();
            // Contract: family, page_count, open_to_first_view_ms, rss, cache,
            // pressure_shed_count, identity must be present.
            QCOMPARE(json.value(QStringLiteral("family")).toString(), QStringLiteral("shell-stress-1k"));
            QCOMPARE(json.value(QStringLiteral("page_count")).toInt(), pageCount);
            QVERIFY(json.contains(QStringLiteral("open_to_first_view_ms")));
            QVERIFY(!json.value(QStringLiteral("open_to_first_view_ms")).isNull());
            QVERIFY(json.value(QStringLiteral("open_to_first_view_ms")).toInt() >= 0);
            QVERIFY(json.contains(QStringLiteral("rss_high_water_bytes")));
            QVERIFY(json.contains(QStringLiteral("cache_high_water_bytes")));
            QVERIFY(json.contains(QStringLiteral("pressure_shed_count")));
            QVERIFY(json.contains(QStringLiteral("identity")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("commit")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("qt")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("os")));
            QVERIFY(json.value(QStringLiteral("status")).toString() == QStringLiteral("complete"));

            qDebug() << "PDFWorkloadEnvelope shell-stress-1k"
                     << QJsonDocument(json).toJson(QJsonDocument::Compact);
            qDebug() << "openToFirstViewMs" << openToFirstViewMs << "rssHighWater"
                     << envelope.rssHighWaterBytes << "cacheHighWater" << envelope.cacheHighWaterBytes
                     << "shed" << envelope.pressureShedCount;
        }
    }

    // Cleanup: remove scripted sources so teardown does not leave dangling
    // pointers (scripted is stack-allocated). DocumentViewSession will be
    // destroyed with EditorHost.
    session->hitTest()->clearSources();
}

QTEST_GUILESS_MAIN(EditorHostTest)

#include "tst_editorhosttest.moc"
