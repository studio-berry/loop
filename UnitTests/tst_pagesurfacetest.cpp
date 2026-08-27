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

// Architecture invariant I23, admission half: one render-request path through the
// existing scheduler, and a completed render enters the canvas only under its
// complete key.
//
// As in tst_interactionboundarytest.cpp, the strongest assertion here is the link
// line in UnitTests/CMakeLists.txt. This target links LoopLibInteraction,
// LoopLibCore, Qt6::Core, Qt6::Gui and Qt6::Test, and deliberately not
// Qt6::Widgets. QTEST_GUILESS_MAIN then proves the P4-S3 exit condition: current
// admission, stale rejection, cancellation, pressure shedding and recovery are
// all provable without a Quick item and without a QWidget.

#include <QtTest>

#include <QImage>
#include <QPainter>

#include <atomic>
#include <memory>

#include "pagesurfacecoordinator.h"
#include "pagesurfacekey.h"
#include "pagesurfacerenderer.h"
#include "viewportcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentreader.h"
#include "pdfjobscheduler.h"
#include "pdfprocessingbudget.h"

namespace
{

constexpr QSizeF A4 = QSizeF(210.0, 297.0);

QString overprintFixturesDirectory()
{
    return QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures");
}

QString overprintRendersDirectory()
{
    return QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/renders");
}

/// Same tolerance as tst_overprintrendertest.cpp's compareRender: small
/// Qt/platform rasterization differences are expected, a bounded number of
/// differing pixels is not.
bool imagesMatchWithinTolerance(const QImage& actual, const QImage& expected)
{
    if (actual.isNull() || expected.isNull() || actual.size() != expected.size())
    {
        return false;
    }

    const QImage actualRgba = actual.convertToFormat(QImage::Format_RGBA8888);
    const QImage expectedRgba = expected.convertToFormat(QImage::Format_RGBA8888);
    constexpr int maxChannelDelta = 2;
    constexpr int differingPixelBudget = 64;
    int differingPixels = 0;

    for (int y = 0; y < actualRgba.height(); ++y)
    {
        const uchar* actualLine = actualRgba.constScanLine(y);
        const uchar* expectedLine = expectedRgba.constScanLine(y);
        for (int x = 0; x < actualRgba.width(); ++x)
        {
            const int offset = x * 4;
            int pixelMaxDelta = 0;
            for (int channel = 0; channel < 4; ++channel)
            {
                pixelMaxDelta = qMax(pixelMaxDelta, qAbs(int(actualLine[offset + channel]) - int(expectedLine[offset + channel])));
            }
            if (pixelMaxDelta > maxChannelDelta)
            {
                ++differingPixels;
            }
        }
    }

    return differingPixels <= differingPixelBudget;
}

pdf::PDFRevisionIdentity makeRevision(const QString& documentId, quint64 documentRevision = 1, quint64 cacheGeneration = 0)
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = documentId;
    revision.document.sourceDataHash = documentId.toUtf8();
    revision.documentRevision = documentRevision;
    revision.cacheGeneration = cacheGeneration;
    return revision;
}

pdfinteraction::PageSurfaceKey makeKey(const pdf::PDFRevisionIdentity& revision, int pageIndex = 0)
{
    return pdfinteraction::makePageSurfaceKey(revision, pageIndex, pdf::PageRotation::None, pdf::PDFRenderer::getDefaultFeatures(), QStringLiteral("srgb"), 1.0, QSize(64, 64), 1.0);
}

class FakeGeometrySource final : public pdfinteraction::IPageGeometrySource
{
public:
    explicit FakeGeometrySource(int pageCount) :
        m_pageCount(pageCount)
    {
    }

    int pageCount() const override { return m_pageCount; }

    QSizeF pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(extraRotation);
        return A4;
    }

    QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(extraRotation);
        Q_UNUSED(deviceRect);
        return QTransform();
    }

private:
    int m_pageCount = 0;
};

/// A revision source a test can move forward by hand.
class FakeRevisionSource final : public pdfinteraction::IDocumentRevisionSource
{
public:
    pdf::PDFRevisionIdentity currentRevision() const override { return revision; }
    bool isCurrent(const pdf::PDFRevisionIdentity& candidate) const override { return candidate == revision; }

    pdf::PDFRevisionIdentity revision = makeRevision(QStringLiteral("doc-1"));
};

/// The same shape as tst_documentfacadetest.cpp's fake: it keeps the scheduler's
/// contract and lets the test decide when work runs, without becoming a second
/// scheduler of its own.
class FakeJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        if (refuseSubmission)
        {
            return QString();
        }

        const QString jobId = spec.jobId.isEmpty() ? QStringLiteral("job-%1").arg(++m_sequence) : spec.jobId;

        submittedSpecs.append(spec);
        m_status.insert(jobId, pdf::PDFJobStatus::Queued);

        if (runInline)
        {
            runJob(jobId, std::move(work));
        }
        else
        {
            m_deferred.insert(jobId, std::move(work));
        }

        return jobId;
    }

    bool cancel(const QString& jobId) override
    {
        cancelledJobs.append(jobId);

        if (m_status.value(jobId, pdf::PDFJobStatus::Succeeded) != pdf::PDFJobStatus::Queued)
        {
            return false;
        }

        if (!cancelStopsQueuedWork)
        {
            // Stands in for a job the scheduler had already started: cancellation
            // is requested, but the work still runs to its own terminal state.
            return false;
        }

        // Matches pdf::PDFJobScheduler: a job cancelled while still queued never
        // runs its work, and nothing will report a terminal state for it.
        m_status.insert(jobId, pdf::PDFJobStatus::Cancelled);
        m_deferred.remove(jobId);
        return true;
    }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = m_status.value(jobId, pdf::PDFJobStatus::Succeeded);
        return result;
    }

    void publishCurrentRevision(const QString& documentKey, const pdf::PDFRevisionIdentity& revision) override
    {
        publishedRevisions.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override { publishedRevisions.remove(documentKey); }

    bool runDeferred(const QString& jobId, bool cancelBeforeRun = false)
    {
        const auto it = m_deferred.find(jobId);
        if (it == m_deferred.end())
        {
            return false;
        }

        pdf::PDFJobWork work = *it;
        m_deferred.erase(it);
        runJob(jobId, std::move(work), cancelBeforeRun);
        return true;
    }

    QStringList deferredJobIds() const { return m_deferred.keys(); }
    int deferredJobCount() const { return int(m_deferred.size()); }

    bool runInline = true;
    bool refuseSubmission = false;
    bool cancelStopsQueuedWork = true;
    QList<pdf::PDFJobSpec> submittedSpecs;
    QStringList cancelledJobs;
    QHash<QString, pdf::PDFRevisionIdentity> publishedRevisions;

private:
    void runJob(const QString& jobId, pdf::PDFJobWork work, bool cancelBeforeRun = false)
    {
        m_status.insert(jobId, pdf::PDFJobStatus::Running);

        auto token = std::make_shared<pdf::PDFJobCancellationToken>();
        if (cancelBeforeRun)
        {
            token->cancel();
        }

        pdf::PDFJobContext context(token, pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});
        work(context);
        m_status.insert(jobId, pdf::PDFJobStatus::Succeeded);
    }

    quint64 m_sequence = 0;
    QHash<QString, pdf::PDFJobStatus> m_status;
    QHash<QString, pdf::PDFJobWork> m_deferred;
};

/// Produces a surface without a PDF, and fails loudly if two renders ever overlap.
class FakePageSurfaceRenderer final : public pdfinteraction::IPageSurfaceRenderer
{
public:
    pdfinteraction::PageSurfaceResult render(const pdfinteraction::PageSurfaceRequest& request, pdf::PDFJobContext& jobContext) override
    {
        const int previous = m_active.fetch_add(1, std::memory_order_acq_rel);
        if (previous != 0)
        {
            reentered = true;
        }

        ++renderCount;
        renderedKeys.append(request.key);

        pdfinteraction::PageSurfaceResult result;
        result.key = request.key;
        result.token = request.token;

        if (jobContext.isCancellationRequested())
        {
            result.state = pdfinteraction::SurfaceTerminalState::Cancelled;
            result.typedError = QStringLiteral("page-surface/cancelled");
        }
        else if (nextState != pdfinteraction::SurfaceTerminalState::Complete)
        {
            result.state = nextState;
        }
        else
        {
            QImage image(request.key.targetPixelSize, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::white);
            result.state = pdfinteraction::SurfaceTerminalState::Complete;
            result.pixels = pdfinteraction::makeSurfaceBuffer(std::move(image));
            result.pixelSize = request.key.targetPixelSize;
            result.byteSize = result.pixels ? result.pixels->byteSize : 0;
        }

        m_active.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }

    void shedPrefetchAndQuality() override { ++shedCount; }

    int renderCount = 0;
    int shedCount = 0;
    bool reentered = false;
    QList<pdfinteraction::PageSurfaceKey> renderedKeys;
    pdfinteraction::SurfaceTerminalState nextState = pdfinteraction::SurfaceTerminalState::Complete;

private:
    std::atomic_int m_active = 0;
};

}   // namespace

class PageSurfaceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void keyEqualityCoversEveryField();
    void keyConstructionNormalizesResolutionFields();
    void compatibilityIgnoresOnlyTheResolutionFields();
    void zoomBucketsAreLogarithmicAndMonotonic();
    void terminalStateNamesAreStable();

    void visibleDemandIsSubmittedAtTheVisiblePriority();
    void prefetchIsSubmittedAtTheNearViewportPriority();
    void repeatedRequestsAreCoalesced();
    void supersededDemandIsCancelledBeforeNewWorkIsSubmitted();
    void completionForASupersededRequestIsRejected();
    void completionAgainstAnOldRevisionIsRejected();
    void revisionReplacementDropsEveryStaleSurface();
    void cancellationIsTerminalAndNotSuccess();
    void completionAfterDestructionReachesNobody();
    void budgetExhaustionIsItsOwnTerminalState();
    void prefetchIsShedBeforeVisibleWork();
    void admittedBytesAreBoundedAndRecover();
    void oversizeSurfacesAreRefusedRatherThanEvictingTheCache();
    void inexactSurfacesStandInDuringZoom();
    void sessionRendererSerializesAndRendersARealPage();
    void sessionRendererEscalatesToAuthoritativeOverprintMatchingGoldenBaseline();
};

void PageSurfaceTest::initTestCase()
{
    qRegisterMetaType<pdf::PDFRevisionIdentity>("pdf::PDFRevisionIdentity");
    qRegisterMetaType<pdfinteraction::PageSurfaceKey>("pdfinteraction::PageSurfaceKey");
    qRegisterMetaType<pdfinteraction::SurfaceTerminalState>("pdfinteraction::SurfaceTerminalState");
}

void PageSurfaceTest::keyEqualityCoversEveryField()
{
    const pdf::PDFRevisionIdentity revision = makeRevision(QStringLiteral("doc-1"));
    const pdfinteraction::PageSurfaceKey base = makeKey(revision);

    QCOMPARE(base, makeKey(revision));

    // Flip one field at a time. A key that still compares equal after a field
    // changed is a key that can admit the wrong pixels.
    pdfinteraction::PageSurfaceKey changed = base;
    changed.revision = makeRevision(QStringLiteral("doc-1"), 2);
    QVERIFY(!(changed == base));

    changed = base;
    changed.pageIndex = 1;
    QVERIFY(!(changed == base));

    changed = base;
    changed.rotation = pdf::PageRotation::Rotate90;
    QVERIFY(!(changed == base));

    changed = base;
    changed.featureBits = base.featureBits ^ static_cast<int>(pdf::PDFRenderer::ColorAdjust_Grayscale);
    QVERIFY(!(changed == base));

    changed = base;
    changed.colorOutputIdentity = QStringLiteral("cmyk");
    QVERIFY(!(changed == base));

    changed = base;
    changed.zoomBucket = base.zoomBucket + 1;
    QVERIFY(!(changed == base));

    changed = base;
    changed.targetPixelSize = QSize(65, 64);
    QVERIFY(!(changed == base));

    changed = base;
    changed.devicePixelRatio1000 = 2000;
    QVERIFY(!(changed == base));

    changed = base;
    changed.pageTileBounds = QRectF(0, 0, 10, 10);
    QVERIFY(!(changed == base));
}

void PageSurfaceTest::keyConstructionNormalizesResolutionFields()
{
    const pdf::PDFRevisionIdentity revision = makeRevision(QStringLiteral("doc-1"));

    // A degenerate target size would render nothing and would hash differently
    // every time it was recomputed from a rounded rectangle.
    const pdfinteraction::PageSurfaceKey degenerate = pdfinteraction::makePageSurfaceKey(revision, 0, pdf::PageRotation::None, pdf::PDFRenderer::getDefaultFeatures(), QString(), 1.0, QSize(0, -4), 1.0);
    QCOMPARE(degenerate.targetPixelSize, QSize(1, 1));

    // Device pixel ratio becomes an exact integer, so two keys either match or do
    // not; a float would leave that to luck.
    const pdfinteraction::PageSurfaceKey scaled = pdfinteraction::makePageSurfaceKey(revision, 0, pdf::PageRotation::None, pdf::PDFRenderer::getDefaultFeatures(), QString(), 1.0, QSize(10, 10), 1.5);
    QCOMPARE(scaled.devicePixelRatio1000, 1500);

    // The several ways of spelling "no tile" are one value.
    const pdfinteraction::PageSurfaceKey emptyTile = pdfinteraction::makePageSurfaceKey(revision, 0, pdf::PageRotation::None, pdf::PDFRenderer::getDefaultFeatures(), QString(), 1.0, QSize(10, 10), 1.0, QRectF(5, 5, 0, 0));
    QVERIFY(emptyTile.pageTileBounds.isNull());

    QVERIFY(degenerate.isValid());
    QVERIFY(!pdfinteraction::makePageSurfaceKey(pdf::PDFRevisionIdentity(), 0, pdf::PageRotation::None, pdf::PDFRenderer::getDefaultFeatures(), QString(), 1.0, QSize(10, 10), 1.0).isValid());
}

void PageSurfaceTest::compatibilityIgnoresOnlyTheResolutionFields()
{
    const pdf::PDFRevisionIdentity revision = makeRevision(QStringLiteral("doc-1"));
    const pdfinteraction::PageSurfaceKey wanted = makeKey(revision);

    pdfinteraction::PageSurfaceKey other = wanted;
    other.zoomBucket += 4;
    other.targetPixelSize = QSize(256, 256);
    QVERIFY(other.compatibleWith(wanted));

    // Everything else makes the surface a different picture.
    for (int field = 0; field < 6; ++field)
    {
        pdfinteraction::PageSurfaceKey incompatible = other;
        switch (field)
        {
            case 0:
                incompatible.revision = makeRevision(QStringLiteral("doc-1"), 2);
                break;
            case 1:
                incompatible.pageIndex = 3;
                break;
            case 2:
                incompatible.rotation = pdf::PageRotation::Rotate180;
                break;
            case 3:
                incompatible.featureBits = 0;
                break;
            case 4:
                incompatible.colorOutputIdentity = QStringLiteral("cmyk");
                break;
            case 5:
                incompatible.devicePixelRatio1000 = 2000;
                break;
            default:
                break;
        }

        QVERIFY2(!incompatible.compatibleWith(wanted), qPrintable(QStringLiteral("field %1 was ignored by compatibleWith").arg(field)));
    }
}

void PageSurfaceTest::zoomBucketsAreLogarithmicAndMonotonic()
{
    QCOMPARE(pdfinteraction::zoomBucketFor(1.0), 0);

    // Sixteen steps per doubling.
    QCOMPARE(pdfinteraction::zoomBucketFor(2.0), 16);
    QCOMPARE(pdfinteraction::zoomBucketFor(0.5), -16);

    QVERIFY(pdfinteraction::zoomBucketFor(1.05) >= pdfinteraction::zoomBucketFor(1.0));
    QVERIFY(pdfinteraction::zoomBucketFor(4.0) > pdfinteraction::zoomBucketFor(2.0));

    // Nonsense zoom does not produce a nonsense bucket.
    QCOMPARE(pdfinteraction::zoomBucketFor(0.0), 0);
    QCOMPARE(pdfinteraction::zoomBucketFor(-1.0), 0);
}

void PageSurfaceTest::terminalStateNamesAreStable()
{
    QCOMPARE(QString::fromLatin1(pdfinteraction::getSurfaceTerminalStateName(pdfinteraction::SurfaceTerminalState::Complete)), QStringLiteral("complete"));
    QCOMPARE(QString::fromLatin1(pdfinteraction::getSurfaceTerminalStateName(pdfinteraction::SurfaceTerminalState::Cancelled)), QStringLiteral("cancelled"));
    QCOMPARE(QString::fromLatin1(pdfinteraction::getSurfaceTerminalStateName(pdfinteraction::SurfaceTerminalState::Failed)), QStringLiteral("failed"));
    QCOMPARE(QString::fromLatin1(pdfinteraction::getSurfaceTerminalStateName(pdfinteraction::SurfaceTerminalState::Stale)), QStringLiteral("stale"));
    QCOMPARE(QString::fromLatin1(pdfinteraction::getSurfaceTerminalStateName(pdfinteraction::SurfaceTerminalState::BudgetExhausted)), QStringLiteral("budget-exhausted"));
}

namespace
{

/// Wires a viewport and a coordinator over the fakes above. Four A4 pages, one
/// millimetre per pixel, a viewport small enough that only page 0 is visible.
struct Fixture
{
    explicit Fixture(pdfinteraction::PageSurfaceBounds bounds = pdfinteraction::PageSurfaceBounds::conservativeDefaults()) :
        geometry(4)
    {
        viewport.setPixelPerMM(1.0);
        viewport.setViewportSizePx(QSize(100, 100));
        viewport.setGeometrySource(&geometry);

        coordinator = std::make_unique<pdfinteraction::PageSurfaceCoordinator>(revisions, submitter, renderer, viewport, bounds);
        coordinator->setDocumentKey(QStringLiteral("doc-1"));
    }

    /// Delivers whatever the relay has queued. The relay is always queued, so a
    /// completion is never visible until the test asks for it.
    static void drain() { QCoreApplication::processEvents(); }

    /// Bytes one page surface costs at the default zoom: 210 x 297 px, 4 bytes
    /// per pixel.
    static constexpr qint64 SurfaceBytes = 210 * 297 * 4;

    FakeGeometrySource geometry;
    FakeRevisionSource revisions;
    FakeJobSubmitter submitter;
    FakePageSurfaceRenderer renderer;
    pdfinteraction::ViewportController viewport;
    std::unique_ptr<pdfinteraction::PageSurfaceCoordinator> coordinator;
};

}   // namespace

void PageSurfaceTest::visibleDemandIsSubmittedAtTheVisiblePriority()
{
    Fixture fixture;
    fixture.coordinator->requestSurfaces();
    Fixture::drain();

    // Page 0 is visible; pages 1 and 2 are the look-ahead.
    QCOMPARE(fixture.submitter.submittedSpecs.size(), 3);
    QCOMPARE(fixture.submitter.submittedSpecs.at(0).priority, pdf::PDFJobPriority::VisiblePage);
    QCOMPARE(fixture.submitter.submittedSpecs.at(0).kind, pdf::PDFJobKind::Rendering);

    // Both fences travel with the job: the scheduler's, and the coordinator's own
    // key check at admission.
    QCOMPARE(fixture.submitter.submittedSpecs.at(0).documentKey, QStringLiteral("doc-1"));
    QCOMPARE(fixture.submitter.submittedSpecs.at(0).documentRevision, fixture.revisions.revision.toString());
    QCOMPARE(fixture.submitter.submittedSpecs.at(0).staleResultPolicy, pdf::PDFJobStaleResultPolicy::Discard);

    QCOMPARE(fixture.coordinator->counters().admitted, 3);
    QCOMPARE(fixture.coordinator->counters().inFlight, 0);

    // Only the visible page is in the frame; the look-ahead is cached, not drawn.
    QCOMPARE(fixture.coordinator->snapshot().tiles.size(), 1);
    QCOMPARE(fixture.coordinator->snapshot().tiles.at(0).key.pageIndex, 0);
    QVERIFY(fixture.coordinator->snapshot().tiles.at(0).exact);
}

void PageSurfaceTest::prefetchIsSubmittedAtTheNearViewportPriority()
{
    Fixture fixture;
    fixture.coordinator->requestSurfaces();

    QCOMPARE(fixture.submitter.submittedSpecs.size(), 3);
    QCOMPARE(fixture.submitter.submittedSpecs.at(1).priority, pdf::PDFJobPriority::NearViewport);
    QCOMPARE(fixture.submitter.submittedSpecs.at(2).priority, pdf::PDFJobPriority::NearViewport);
}

void PageSurfaceTest::repeatedRequestsAreCoalesced()
{
    Fixture fixture;
    fixture.submitter.runInline = false;

    fixture.coordinator->requestSurfaces();
    const int afterFirst = fixture.submitter.submittedSpecs.size();

    // Nothing about the viewport changed, so there is nothing new to ask for.
    fixture.coordinator->requestSurfaces();
    QCOMPARE(fixture.submitter.submittedSpecs.size(), afterFirst);

    // And once the surfaces are cached, a further request submits nothing either.
    for (const QString& jobId : fixture.submitter.deferredJobIds())
    {
        fixture.submitter.runDeferred(jobId);
    }
    Fixture::drain();

    fixture.coordinator->requestSurfaces();
    QCOMPARE(fixture.submitter.submittedSpecs.size(), afterFirst);
    QVERIFY(fixture.coordinator->counters().cacheHitsExact > 0);
}

void PageSurfaceTest::supersededDemandIsCancelledBeforeNewWorkIsSubmitted()
{
    Fixture fixture;
    fixture.submitter.runInline = false;
    fixture.coordinator->requestSurfaces();

    const QStringList firstRound = fixture.submitter.deferredJobIds();
    QCOMPARE(firstRound.size(), 3);

    // A zoom supersedes every key that was in flight.
    fixture.viewport.setZoom(2.0);
    Fixture::drain();

    for (const QString& jobId : firstRound)
    {
        QVERIFY2(fixture.submitter.cancelledJobs.contains(jobId), qPrintable(jobId));
    }

    QCOMPARE(fixture.coordinator->counters().cancelled, 3);

    // The new demand was submitted, and it carries the new generation.
    QVERIFY(fixture.submitter.submittedSpecs.size() > 3);
}

void PageSurfaceTest::completionForASupersededRequestIsRejected()
{
    Fixture fixture;
    fixture.submitter.runInline = false;

    // The scheduler had already started this one, so cancelling it does not stop
    // the work: its completion still arrives.
    fixture.submitter.cancelStopsQueuedWork = false;
    fixture.coordinator->requestSurfaces();

    const QStringList firstRound = fixture.submitter.deferredJobIds();
    QVERIFY(!firstRound.isEmpty());

    fixture.viewport.setZoom(2.0);
    Fixture::drain();

    QVERIFY(fixture.submitter.runDeferred(firstRound.first()));
    Fixture::drain();

    QCOMPARE(fixture.coordinator->counters().admitted, 0);
    QVERIFY(fixture.coordinator->counters().rejectedSuperseded > 0);
    QVERIFY(fixture.coordinator->snapshot().tiles.isEmpty());
}

void PageSurfaceTest::completionAgainstAnOldRevisionIsRejected()
{
    Fixture fixture;
    fixture.submitter.runInline = false;
    fixture.coordinator->requestSurfaces();

    const QStringList jobIds = fixture.submitter.deferredJobIds();
    QVERIFY(!jobIds.isEmpty());

    // The document moved on while the render was in flight. The viewport did not,
    // so only the revision clause of the admission rule can catch this.
    fixture.revisions.revision = makeRevision(QStringLiteral("doc-1"), 2);

    QVERIFY(fixture.submitter.runDeferred(jobIds.first()));
    Fixture::drain();

    QCOMPARE(fixture.coordinator->counters().admitted, 0);
    QCOMPARE(fixture.coordinator->counters().rejectedStaleRevision, 1);
    QVERIFY(fixture.coordinator->snapshot().tiles.isEmpty());
}

void PageSurfaceTest::revisionReplacementDropsEveryStaleSurface()
{
    Fixture fixture;
    fixture.coordinator->requestSurfaces();
    Fixture::drain();
    QCOMPARE(fixture.coordinator->counters().admitted, 3);

    const pdf::PDFRevisionIdentity replacement = makeRevision(QStringLiteral("doc-1"), 2);
    fixture.revisions.revision = replacement;
    fixture.coordinator->invalidate(replacement);

    // Nothing rendered for the previous state survives, and nothing from it can
    // be drawn.
    QCOMPARE(fixture.coordinator->counters().admittedBytes, qint64(0));
    QVERIFY(fixture.coordinator->snapshot().tiles.isEmpty());
}

void PageSurfaceTest::cancellationIsTerminalAndNotSuccess()
{
    Fixture fixture;
    fixture.submitter.runInline = false;
    fixture.coordinator->requestSurfaces();

    QSignalSpy terminalSpy(fixture.coordinator.get(), &pdfinteraction::PageSurfaceCoordinator::surfaceTerminal);

    fixture.coordinator->cancelInFlight();

    QCOMPARE(fixture.coordinator->counters().cancelled, 3);
    QCOMPARE(fixture.coordinator->counters().admitted, 0);
    QCOMPARE(fixture.coordinator->counters().inFlight, 0);
    QCOMPARE(terminalSpy.count(), 3);

    const auto state = terminalSpy.at(0).at(1).value<pdfinteraction::SurfaceTerminalState>();
    QCOMPARE(state, pdfinteraction::SurfaceTerminalState::Cancelled);
}

void PageSurfaceTest::completionAfterDestructionReachesNobody()
{
    Fixture fixture;
    fixture.submitter.runInline = false;

    // The destructor cancels; this makes the job outlive that cancel the way a
    // running one would.
    fixture.submitter.cancelStopsQueuedWork = false;
    fixture.coordinator->requestSurfaces();

    const QStringList jobIds = fixture.submitter.deferredJobIds();
    QVERIFY(!jobIds.isEmpty());

    fixture.coordinator.reset();

    // The worker still runs and still posts. The relay was detached first, so the
    // completion is dropped instead of reaching freed memory.
    QVERIFY(fixture.submitter.runDeferred(jobIds.first()));
    Fixture::drain();

    QCOMPARE(fixture.renderer.renderCount, 1);
}

void PageSurfaceTest::budgetExhaustionIsItsOwnTerminalState()
{
    Fixture fixture;
    fixture.renderer.nextState = pdfinteraction::SurfaceTerminalState::BudgetExhausted;

    fixture.coordinator->requestSurfaces();
    Fixture::drain();

    // Incomplete work, never a pass and never an untyped failure.
    QCOMPARE(fixture.coordinator->counters().budgetExhausted, 3);
    QCOMPARE(fixture.coordinator->counters().failed, 0);
    QCOMPARE(fixture.coordinator->counters().admitted, 0);
    QVERIFY(fixture.coordinator->snapshot().tiles.isEmpty());
}

void PageSurfaceTest::prefetchIsShedBeforeVisibleWork()
{
    pdfinteraction::PageSurfaceBounds bounds;
    bounds.maxNearViewportRequests = 1;

    Fixture fixture(bounds);
    fixture.submitter.runInline = false;
    fixture.coordinator->requestSurfaces();

    // The visible page and one prefetch are submitted; the second prefetch is
    // shed rather than queued behind them.
    QCOMPARE(fixture.submitter.submittedSpecs.size(), 2);
    QCOMPARE(fixture.submitter.submittedSpecs.at(0).priority, pdf::PDFJobPriority::VisiblePage);
    QCOMPARE(fixture.submitter.submittedSpecs.at(1).priority, pdf::PDFJobPriority::NearViewport);
    QCOMPARE(fixture.coordinator->counters().shed, 1);
}

void PageSurfaceTest::admittedBytesAreBoundedAndRecover()
{
    pdfinteraction::PageSurfaceBounds bounds;
    bounds.maxAdmittedBytes = 2 * Fixture::SurfaceBytes;

    Fixture fixture(bounds);
    fixture.coordinator->requestSurfaces();
    Fixture::drain();

    // Three surfaces do not fit in two surfaces' worth of budget.
    QCOMPARE(fixture.coordinator->counters().admitted, 3);
    QVERIFY(fixture.coordinator->counters().evictions > 0);
    QVERIFY(fixture.coordinator->counters().admittedBytes <= bounds.maxAdmittedBytes);
    QVERIFY(fixture.coordinator->counters().admittedBytesHighWater <= bounds.maxAdmittedBytes);

    // Recovery: what was evicted is simply requested again. Nothing had to be
    // rebuilt, and the bound still holds.
    const int admittedBefore = fixture.coordinator->counters().admitted;
    fixture.coordinator->requestSurfaces();
    Fixture::drain();

    QVERIFY(fixture.coordinator->counters().admitted > admittedBefore);
    QVERIFY(fixture.coordinator->counters().admittedBytes <= bounds.maxAdmittedBytes);
    QVERIFY(!fixture.coordinator->snapshot().tiles.isEmpty());
}

void PageSurfaceTest::oversizeSurfacesAreRefusedRatherThanEvictingTheCache()
{
    pdfinteraction::PageSurfaceBounds bounds;
    bounds.maxAdmittedBytes = 1024;

    Fixture fixture(bounds);
    fixture.coordinator->requestSurfaces();
    Fixture::drain();

    // A surface that cannot fit is refused outright. Evicting the whole cache for
    // something that still would not fit is worse than not caching it.
    QCOMPARE(fixture.coordinator->counters().admitted, 0);
    QCOMPARE(fixture.coordinator->counters().rejectedOversize, 3);
    QCOMPARE(fixture.coordinator->counters().evictions, 0);
    QCOMPARE(fixture.coordinator->counters().admittedBytes, qint64(0));
}

void PageSurfaceTest::inexactSurfacesStandInDuringZoom()
{
    Fixture fixture;
    fixture.coordinator->requestSurfaces();
    Fixture::drain();
    QVERIFY(fixture.coordinator->snapshot().tiles.at(0).exact);

    // Nothing completes at the new zoom, so the only thing that can keep the page
    // on screen is the surface rendered for the old one.
    fixture.submitter.runInline = false;
    fixture.viewport.setZoom(1.05);

    QCOMPARE(fixture.coordinator->snapshot().tiles.size(), 1);
    QVERIFY(!fixture.coordinator->snapshot().tiles.at(0).exact);
    QVERIFY(fixture.coordinator->counters().cacheHitsInexact > 0);

    // The substitute is still the current revision. That is the only substitution
    // the admission rule allows.
    QCOMPARE(fixture.coordinator->snapshot().tiles.at(0).key.revision, fixture.revisions.revision);
}

void PageSurfaceTest::sessionRendererSerializesAndRendersARealPage()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));

    pdf::PDFPageContentStreamBuilder contentBuilder(&builder, pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = contentBuilder.begin(page))
    {
        painter->fillRect(QRectF(20, 20, 160, 160), Qt::black);
        contentBuilder.end(painter);
    }

    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);

    pdfinteraction::PDFSessionPageSurfaceRenderer renderer(context);

    pdfinteraction::PageSurfaceRequest request;
    request.key = makeKey(context.getRevision());
    request.token = pdfinteraction::RevisionFencedToken{ 1, context.getRevision() };

    auto token = std::make_shared<pdf::PDFJobCancellationToken>();
    pdf::PDFJobContext jobContext(token, pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});

    const pdfinteraction::PageSurfaceResult result = renderer.render(request, jobContext);

    QCOMPARE(result.state, pdfinteraction::SurfaceTerminalState::Complete);
    QVERIFY(result.pixels);
    QCOMPARE(result.pixelSize, QSize(64, 64));
    QVERIFY(result.byteSize > 0);
    QVERIFY(!result.pixels->image.isNull());

    // A key from a document state the session has moved past is stale, not a
    // failure and certainly not a success.
    pdfinteraction::PageSurfaceRequest staleRequest = request;
    staleRequest.key = makeKey(makeRevision(QStringLiteral("other-doc"), 7));
    QCOMPARE(renderer.render(staleRequest, jobContext).state, pdfinteraction::SurfaceTerminalState::Stale);

    // Tile bounds are carried by the key but not yet honoured, and saying so is
    // better than quietly returning a whole page.
    pdfinteraction::PageSurfaceRequest tileRequest = request;
    tileRequest.key.pageTileBounds = QRectF(0, 0, 100, 100);
    const pdfinteraction::PageSurfaceResult tileResult = renderer.render(tileRequest, jobContext);
    QCOMPARE(tileResult.state, pdfinteraction::SurfaceTerminalState::Failed);
    QCOMPARE(tileResult.typedError, QStringLiteral("page-surface/tiling-unsupported"));

    // Cancellation is observed before any work starts.
    token->cancel();
    QCOMPARE(renderer.render(request, jobContext).state, pdfinteraction::SurfaceTerminalState::Cancelled);

    // After detach the renderer is inert rather than dangling.
    auto liveToken = std::make_shared<pdf::PDFJobCancellationToken>();
    pdf::PDFJobContext liveContext(liveToken, pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});
    renderer.detach();
    const pdfinteraction::PageSurfaceResult detached = renderer.render(request, liveContext);
    QCOMPARE(detached.state, pdfinteraction::SurfaceTerminalState::Failed);
    QCOMPARE(detached.typedError, QStringLiteral("page-surface/context-gone"));
}

void PageSurfaceTest::sessionRendererEscalatesToAuthoritativeOverprintMatchingGoldenBaseline()
{
    // Issue #49: the canvas's one-page escalation to PDFRenderPolicy::forOutputPreview()
    // must produce the same pixels as the authoritative PDFTransparencyRenderer
    // construction tst_overprintrendertest.cpp already trusts against this
    // committed baseline -- proving canvas escalation and that test agree, not
    // just that each independently renders something plausible.
    const QString fixturePath = overprintFixturesDirectory() + QStringLiteral("/overprint-cmyk-mode1-on.pdf");
    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentContext context(&document);
    pdfinteraction::PDFSessionPageSurfaceRenderer renderer(context);

    pdfinteraction::PageSurfaceRequest request;
    request.key = pdfinteraction::makePageSurfaceKey(context.getRevision(),
                                                     0,
                                                     pdf::PageRotation::None,
                                                     pdf::PDFRenderer::getDefaultFeatures(),
                                                     pdfinteraction::withAuthoritativeOverprintMarker(QStringLiteral("srgb")),
                                                     1.0,
                                                     QSize(128, 128),
                                                     1.0);
    request.token = pdfinteraction::RevisionFencedToken{ 1, context.getRevision() };

    auto token = std::make_shared<pdf::PDFJobCancellationToken>();
    pdf::PDFJobContext jobContext(token, pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});

    const pdfinteraction::PageSurfaceResult result = renderer.render(request, jobContext);

    QCOMPARE(result.state, pdfinteraction::SurfaceTerminalState::Complete);
    QVERIFY(result.pixels);
    QVERIFY(!result.pixels->image.isNull());
    QCOMPARE(result.pixelSize, QSize(128, 128));

    const QImage baseline(overprintRendersDirectory() + QStringLiteral("/overprint-cmyk-mode1-on.png"));
    QVERIFY2(!baseline.isNull(), "Missing committed baseline overprint-cmyk-mode1-on.png");
    QVERIFY2(imagesMatchWithinTolerance(result.pixels->image, baseline),
             "Authoritative canvas escalation does not match the committed overprint-cmyk-mode1-on.png baseline");
}

QTEST_GUILESS_MAIN(PageSurfaceTest)

#include "tst_pagesurfacetest.moc"
