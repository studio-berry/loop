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
#include "pdfprocessingbudget.h"

#include <QtTest>

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
        Q_UNUSED(spec);
        pdf::PDFJobContext context(std::make_shared<pdf::PDFJobCancellationToken>(), pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});
        work(context);
        return spec.jobId;
    }

    bool cancel(const QString& jobId) override
    {
        Q_UNUSED(jobId);
        return false;
    }
};

}   // namespace

class PageSurfaceBudgetTest final : public QObject
{
    Q_OBJECT

private slots:
    void partitionsSingleLimit();
};

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
    coordinator.setDocumentKey(QStringLiteral("doc-1"));
    coordinator.requestSurfaces();
    QCoreApplication::processEvents();

    QVERIFY(coordinator.counters().shed > 0);
    QVERIFY(renderer.shedCalls > 0);
    QCOMPARE(session->compileCacheLimit(), pdf::PDFDocumentSession::ShedCompileCacheLimit);
    QVERIFY(coordinator.counters().admittedBytes <= bounds.maxAdmittedBytes);
}

QTEST_GUILESS_MAIN(PageSurfaceBudgetTest)

#include "tst_pagesurfacebudgettest.moc"
