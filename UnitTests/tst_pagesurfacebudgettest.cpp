// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "pagesurfacecoordinator.h"
#include "pagesurfacerenderer.h"
#include "viewportcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentsession.h"
#include "pdfjobscheduler.h"
#include "pdfpagecachebudget.h"
#include "pdfprocessingbudget.h"

#include <QtTest>

#include <limits>

namespace
{

constexpr QSizeF A4 = QSizeF(210.0, 297.0);
constexpr qint64 SurfaceBytes = 210LL * 297 * 4;

pdf::PDFRevisionIdentity makeRevision(const QString& documentId, quint64 documentRevision = 1)
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = documentId;
    revision.document.sourceDataHash = documentId.toUtf8();
    revision.documentRevision = documentRevision;
    return revision;
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

class FakeRevisionSource final : public pdfinteraction::IDocumentRevisionSource
{
public:
    pdf::PDFRevisionIdentity currentRevision() const override { return revision; }
    bool isCurrent(const pdf::PDFRevisionIdentity& candidate) const override { return candidate == revision; }

    pdf::PDFRevisionIdentity revision = makeRevision(QStringLiteral("doc-1"));
};

class SessionCoupledRenderer final : public pdfinteraction::IPageSurfaceRenderer
{
public:
    explicit SessionCoupledRenderer(pdf::PDFDocumentContext& context) :
        m_context(context)
    {
    }

    pdfinteraction::PageSurfaceResult render(const pdfinteraction::PageSurfaceRequest& request, pdf::PDFJobContext& jobContext) override
    {
        pdfinteraction::PageSurfaceResult result;
        result.key = request.key;
        result.token = request.token;

        if (jobContext.isCancellationRequested())
        {
            result.state = pdfinteraction::SurfaceTerminalState::Cancelled;
            result.typedError = QStringLiteral("page-surface/cancelled");
            return result;
        }

        QImage image(request.key.targetPixelSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        result.state = pdfinteraction::SurfaceTerminalState::Complete;
        result.pixels = pdfinteraction::makeSurfaceBuffer(std::move(image));
        result.pixelSize = request.key.targetPixelSize;
        result.byteSize = result.pixels ? result.pixels->byteSize : 0;
        return result;
    }

    void shedPrefetchAndQuality() override
    {
        ++shedCalls;
        if (pdf::PDFDocumentSession* session = m_context.getSession())
        {
            session->shedPrefetchAndQuality();
        }
    }

    int shedCalls = 0;

private:
    pdf::PDFDocumentContext& m_context;
};

class InlineJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        const QString jobId = spec.jobId.isEmpty() ? QStringLiteral("job-%1").arg(++m_sequence) : spec.jobId;
        Q_UNUSED(jobId);
        pdf::PDFJobContext context(std::make_shared<pdf::PDFJobCancellationToken>(), pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});
        work(context);
        return jobId;
    }

    bool cancel(const QString& jobId) override
    {
        Q_UNUSED(jobId);
        return false;
    }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = pdf::PDFJobStatus::Succeeded;
        return result;
    }

    void publishCurrentRevision(const QString& documentKey, const pdf::PDFRevisionIdentity& revision) override
    {
        publishedRevisions.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override
    {
        publishedRevisions.remove(documentKey);
    }

private:
    quint64 m_sequence = 0;
    QHash<QString, pdf::PDFRevisionIdentity> publishedRevisions;
};

}   // namespace

class PageSurfaceBudgetTest final : public QObject
{
    Q_OBJECT

private slots:
    void partitionMath();
    void partitionsSingleLimit();
    void combinedResidentStaysBounded();
    void compiledCacheEvictsByByteLimit();
    void oversizedCompiledPageIsRejectedAndRecovers();
    void surfaceBudgetRecoversAfterIncrease();
};

void PageSurfaceBudgetTest::partitionMath()
{
    QCOMPARE(pdf::PDFPageCacheBudget::compiledPages(-1), qsizetype(0));
    QCOMPARE(pdf::PDFPageCacheBudget::pageSurfaces(-1), qsizetype(0));
    QCOMPARE(pdf::PDFPageCacheBudget::total(-1), qsizetype(0));
    QCOMPARE(pdf::PDFPageCacheBudget::total(0), qsizetype(0));
    QCOMPARE(pdf::PDFPageCacheBudget::total(100), qsizetype(100));
    QCOMPARE(pdf::PDFPageCacheBudget::compiledPages(1), qsizetype(0));
    QCOMPARE(pdf::PDFPageCacheBudget::pageSurfaces(1), qsizetype(1));
    QCOMPARE(pdf::PDFPageCacheBudget::compiledPages(101), qsizetype(50));
    QCOMPARE(pdf::PDFPageCacheBudget::pageSurfaces(101), qsizetype(51));

    const QList<qsizetype> totals = {
        qsizetype(0),
        qsizetype(1),
        qsizetype(100),
        qsizetype(101),
        qsizetype(1 << 20),
        qsizetype(999)
    };
    for (const qsizetype requested : totals)
    {
        const qsizetype total = pdf::PDFPageCacheBudget::total(requested);
        const qsizetype compiled = pdf::PDFPageCacheBudget::compiledPages(requested);
        const qsizetype surfaces = pdf::PDFPageCacheBudget::pageSurfaces(requested);
        QCOMPARE(compiled + surfaces, total);
        QVERIFY(compiled >= 0);
        QVERIFY(surfaces >= 0);
    }

    const QList<qsizetype> requested = {
        qsizetype(-1),
        qsizetype(0),
        qsizetype(1),
        qsizetype(2),
        qsizetype(101),
        qsizetype(256ll * 1024 * 1024),
        std::numeric_limits<qsizetype>::max()
    };

    for (const qsizetype value : requested)
    {
        const qsizetype total = pdf::PDFPageCacheBudget::total(value);
        const qsizetype compiled = pdf::PDFPageCacheBudget::compiledPages(value);
        const qsizetype surfaces = pdf::PDFPageCacheBudget::pageSurfaces(value);
        QCOMPARE(compiled + surfaces, total);
        QVERIFY(compiled >= 0);
        QVERIFY(surfaces >= 0);
    }
}

void PageSurfaceBudgetTest::partitionsSingleLimit()
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < 4; ++page)
    {
        builder.appendPage(QRectF(0, 0, A4.width(), A4.height()));
    }
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);
    pdf::PDFDocumentSession* session = context.getSession();
    QVERIFY(session != nullptr);
    QVERIFY(session->compileCacheLimit() > pdf::PDFDocumentSession::ShedCompileCacheLimit);

    const qsizetype total = 256ll * 1024 * 1024;
    session->setCacheLimit(total);
    QCOMPARE(session->cacheLimit(), total);
    QCOMPARE(session->compiledCacheByteLimit(), pdf::PDFPageCacheBudget::compiledPages(total));
    for (size_t page = 0; page < 4; ++page)
    {
        QVERIFY(session->compilePage(page) != nullptr);
    }
    QVERIFY(session->compiledCacheBytes() > 0);
    QVERIFY(session->compiledCacheBytes() <= session->compiledCacheByteLimit());

    FakeRevisionSource revisions;
    InlineJobSubmitter submitter;
    SessionCoupledRenderer renderer(context);
    pdfinteraction::ViewportController viewport;
    FakeGeometrySource geometry(4);
    viewport.setPixelPerMM(1.0);
    viewport.setViewportSizePx(QSize(100, 100));
    viewport.setGeometrySource(&geometry);

    pdfinteraction::PageSurfaceBounds bounds;
    bounds.maxInFlightBytes = SurfaceBytes;
    bounds.maxAdmittedBytes = SurfaceBytes * 2;
    bounds.maxNearViewportRequests = 1;

    pdfinteraction::PageSurfaceCoordinator coordinator(revisions, submitter, renderer, viewport, bounds);
    coordinator.setPageCacheBudget(context.getSharedPageCacheBudget());
    coordinator.setCacheLimit(total);
    QCOMPARE(coordinator.cacheLimit(), total);
    QCOMPARE(coordinator.bounds().maxAdmittedBytes,
             static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(total)));
    QCOMPARE(coordinator.sharedPageCacheBudget(), session->getSharedPageCacheBudget());
    coordinator.setDocumentKey(QStringLiteral("doc-1"));
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();

    QVERIFY(coordinator.counters().shed > 0);
    QVERIFY(renderer.shedCalls > 0);
    QCOMPARE(session->compileCacheLimit(), pdf::PDFDocumentSession::ShedCompileCacheLimit);
    QVERIFY(session->cacheLimit() == total);
    QVERIFY(session->compiledCacheBytes() <= session->compiledCacheByteLimit());
    QVERIFY(coordinator.counters().admittedBytes <= coordinator.bounds().maxAdmittedBytes);
    QVERIFY(coordinator.counters().admittedBytesHighWater <= coordinator.bounds().maxAdmittedBytes);
    QCOMPARE(session->getPageCacheBudget()->residentBytes(),
             session->compiledCacheBytes() + coordinator.counters().admittedBytes);
    QVERIFY(session->getPageCacheBudget()->residentBytes() <= total);
    QVERIFY(session->compiledCacheBytes() + coordinator.counters().admittedBytes <= total);

    pdf::PDFPageCacheBudget combinedBudget(100);
    QVERIFY(combinedBudget.tryReserve(pdf::PDFPageCacheBudget::Pool::CompiledPages, 50));
    QVERIFY(combinedBudget.tryReserve(pdf::PDFPageCacheBudget::Pool::PageSurfaces, 50));
    QVERIFY(!combinedBudget.tryReserve(pdf::PDFPageCacheBudget::Pool::CompiledPages, 1));
    QCOMPARE(combinedBudget.residentBytes(), qsizetype(100));
}

void PageSurfaceBudgetTest::combinedResidentStaysBounded()
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < 4; ++page)
    {
        builder.appendPage(QRectF(0, 0, A4.width(), A4.height()));
    }
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);
    pdf::PDFDocumentSession* session = context.getSession();
    QVERIFY(session != nullptr);
    QVERIFY(session->compileCacheLimit() > pdf::PDFDocumentSession::ShedCompileCacheLimit);

    const qsizetype totalBytes = 8 * 1024 * 1024;
    const qsizetype surfaceBasedTotal = SurfaceBytes * 6;
    QVERIFY(surfaceBasedTotal > SurfaceBytes * 2);
    QVERIFY(totalBytes > surfaceBasedTotal);

    FakeRevisionSource revisions;
    InlineJobSubmitter submitter;
    SessionCoupledRenderer renderer(context);
    pdfinteraction::ViewportController viewport;
    FakeGeometrySource geometry(4);
    viewport.setPixelPerMM(1.0);
    viewport.setViewportSizePx(QSize(100, 100));
    viewport.setGeometrySource(&geometry);

    pdfinteraction::PageSurfaceBounds bounds;
    bounds.maxInFlightBytes = SurfaceBytes;
    bounds.maxAdmittedBytes = SurfaceBytes * 2;
    bounds.maxNearViewportRequests = 1;

    pdfinteraction::PageSurfaceCoordinator coordinator(revisions, submitter, renderer, viewport, bounds);
    coordinator.setPageCacheBudget(context.getSharedPageCacheBudget());
    coordinator.setCacheLimit(totalBytes);
    QCOMPARE(coordinator.cacheLimit(), pdf::PDFPageCacheBudget::total(totalBytes));
    QCOMPARE(session->cacheLimit(), pdf::PDFPageCacheBudget::total(totalBytes));
    QCOMPARE(session->compiledCacheByteLimit(), pdf::PDFPageCacheBudget::compiledPages(totalBytes));
    QCOMPARE(coordinator.bounds().maxAdmittedBytes, static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(totalBytes)));
    QCOMPARE(session->getSharedPageCacheBudget(), coordinator.sharedPageCacheBudget());
    QCOMPARE(pdf::PDFPageCacheBudget::compiledPages(totalBytes) + pdf::PDFPageCacheBudget::pageSurfaces(totalBytes),
             pdf::PDFPageCacheBudget::total(totalBytes));
    QVERIFY(session->getPageCacheBudget()->total() == totalBytes);

    for (size_t page = 0; page < 4; ++page)
    {
        QVERIFY(session->compilePage(page) != nullptr);
    }
    QVERIFY(session->compiledCacheBytes() > 0);
    QVERIFY(session->compiledCacheBytes() <= session->compiledCacheByteLimit());

    coordinator.setDocumentKey(QStringLiteral("doc-1"));
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();

    QVERIFY(coordinator.counters().shed > 0);
    QVERIFY(renderer.shedCalls > 0);
    QCOMPARE(session->compileCacheLimit(), pdf::PDFDocumentSession::ShedCompileCacheLimit);
    QVERIFY(session->compiledCacheByteLimit() == pdf::PDFDocumentSession::ShedCompiledCacheByteLimit || session->compiledCacheByteLimit() <= pdf::PDFDocumentSession::CompiledCacheByteLimitDefault);
    QVERIFY(session->compiledCacheBytes() <= session->compiledCacheByteLimit());
    QVERIFY(coordinator.counters().admittedBytes <= coordinator.bounds().maxAdmittedBytes);
    QVERIFY(coordinator.counters().admittedBytes <= pdf::PDFPageCacheBudget::pageSurfaces(totalBytes));
    QVERIFY(coordinator.counters().admittedBytesHighWater <= coordinator.bounds().maxAdmittedBytes);
    QCOMPARE(session->getPageCacheBudget()->residentBytes(),
             session->compiledCacheBytes() + coordinator.counters().admittedBytes);
    QVERIFY(session->getPageCacheBudget()->residentBytes() <= totalBytes);
    QVERIFY(session->compiledCacheBytes() + coordinator.counters().admittedBytes <= totalBytes);

    const qsizetype oversizeTotal = 1024;
    coordinator.setCacheLimit(oversizeTotal);
    QCOMPARE(coordinator.cacheLimit(), pdf::PDFPageCacheBudget::total(oversizeTotal));
    QCOMPARE(coordinator.bounds().maxAdmittedBytes,
             static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(oversizeTotal)));
    QVERIFY(coordinator.bounds().maxAdmittedBytes < SurfaceBytes);
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();
    QVERIFY(coordinator.counters().admitted == 0 || coordinator.counters().rejectedOversize > 0);
    QVERIFY(coordinator.counters().admittedBytes == 0 || coordinator.counters().rejectedOversize > 0);
    QVERIFY(coordinator.counters().rejectedOversize > 0 || coordinator.counters().admittedBytes == 0);

    const qsizetype recoveredTotal = SurfaceBytes * 6;
    coordinator.setCacheLimit(recoveredTotal);
    QCOMPARE(coordinator.cacheLimit(), pdf::PDFPageCacheBudget::total(recoveredTotal));
    QCOMPARE(coordinator.bounds().maxAdmittedBytes,
             static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(recoveredTotal)));
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();
    QVERIFY(coordinator.counters().admitted > 0);
    QVERIFY(coordinator.counters().admittedBytes > 0);
    QVERIFY(coordinator.counters().admittedBytes <= coordinator.bounds().maxAdmittedBytes);
    QVERIFY(coordinator.counters().admittedBytes <= pdf::PDFPageCacheBudget::pageSurfaces(recoveredTotal));
    QVERIFY(session->compiledCacheBytes() + coordinator.counters().admittedBytes <= recoveredTotal);
    QVERIFY(session->getPageCacheBudget()->residentBytes() <= recoveredTotal);
    QVERIFY(session->getPageCacheBudget()->residentBytes() == session->compiledCacheBytes() + coordinator.counters().admittedBytes);

    const qsizetype largerRecovery = 8 * 1024 * 1024;
    coordinator.setCacheLimit(largerRecovery);
    QCOMPARE(session->compiledCacheByteLimit(), pdf::PDFPageCacheBudget::compiledPages(largerRecovery));
    QCOMPARE(coordinator.bounds().maxAdmittedBytes,
             static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(largerRecovery)));
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();
    QVERIFY(coordinator.counters().admitted > 0);
    QVERIFY(session->compiledCacheBytes() + coordinator.counters().admittedBytes <= largerRecovery);
}

void PageSurfaceBudgetTest::compiledCacheEvictsByByteLimit()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, A4.width(), A4.height()));
    builder.appendPage(QRectF(0, 0, A4.width(), A4.height()));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    const pdf::PDFPrecompiledPage* first = session.compilePage(0);
    QVERIFY(first != nullptr);
    const qsizetype onePageBytes = session.compiledCacheBytes();
    QVERIFY(onePageBytes > 0);

    session.setCompiledCacheByteLimit(onePageBytes);
    const pdf::PDFPrecompiledPage* second = session.compilePage(1);
    QVERIFY(second != nullptr);
    QVERIFY(session.compiledCacheBytes() <= onePageBytes);
    QCOMPARE(session.compiledCacheByteLimit(), onePageBytes);
    QCOMPARE(session.compilePage(1), second);
}

void PageSurfaceBudgetTest::oversizedCompiledPageIsRejectedAndRecovers()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, A4.width(), A4.height()));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    session.setCacheLimit(0);
    QCOMPARE(session.compilePage(0), nullptr);
    QCOMPARE(session.compiledCacheBytes(), qsizetype(0));

    const qsizetype recoveredLimit = 256ll * 1024 * 1024;
    session.setCacheLimit(recoveredLimit);
    QVERIFY(session.compilePage(0) != nullptr);
    QVERIFY(session.compiledCacheBytes() > 0);
    QVERIFY(session.compiledCacheBytes() <= session.compiledCacheByteLimit());
}

void PageSurfaceBudgetTest::surfaceBudgetRecoversAfterIncrease()
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < 4; ++page)
    {
        builder.appendPage(QRectF(0, 0, A4.width(), A4.height()));
    }
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);

    FakeRevisionSource revisions;
    InlineJobSubmitter submitter;
    SessionCoupledRenderer renderer(context);
    pdfinteraction::ViewportController viewport;
    FakeGeometrySource geometry(4);
    viewport.setPixelPerMM(1.0);
    viewport.setViewportSizePx(QSize(100, 100));
    viewport.setGeometrySource(&geometry);

    pdfinteraction::PageSurfaceBounds bounds;
    bounds.maxInFlightBytes = SurfaceBytes;
    bounds.maxAdmittedBytes = 1024;
    bounds.maxNearViewportRequests = 1;

    pdfinteraction::PageSurfaceCoordinator coordinator(revisions, submitter, renderer, viewport, bounds);
    coordinator.setDocumentKey(QStringLiteral("doc-1"));
    coordinator.setCacheLimit(2048);
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();

    QCOMPARE(coordinator.counters().admitted, 0);
    QVERIFY(coordinator.counters().rejectedOversize > 0);
    QCOMPARE(coordinator.counters().admittedBytes, qint64(0));

    const qsizetype increasedLimit = SurfaceBytes * 2;
    coordinator.setCacheLimit(increasedLimit);
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();

    QVERIFY(coordinator.counters().admitted > 0);
    QCOMPARE(coordinator.cacheLimit(), increasedLimit);
    QCOMPARE(coordinator.bounds().maxAdmittedBytes, SurfaceBytes);
    QVERIFY(coordinator.counters().admittedBytes <= coordinator.bounds().maxAdmittedBytes);
}

QTEST_GUILESS_MAIN(PageSurfaceBudgetTest)

#include "tst_pagesurfacebudgettest.moc"
