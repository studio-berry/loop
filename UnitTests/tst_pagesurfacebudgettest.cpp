// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "documentcontextsource.h"
#include "jobsubmitter.h"
#include "pagesurfacecoordinator.h"
#include "pagesurfacerenderer.h"
#include "viewportcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentsession.h"
#include "pdfjobscheduler.h"

#include <QtTest>

namespace
{

constexpr QSizeF A4 = QSizeF(210.0, 297.0);

class DocumentGeometrySource final : public pdfinteraction::IPageGeometrySource
{
public:
    explicit DocumentGeometrySource(pdf::PDFDocument* document) :
        m_document(document)
    {
    }

    int pageCount() const override
    {
        return m_document ? int(m_document->getCatalog()->getPageCount()) : 0;
    }

    QSizeF pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        const bool transposed = extraRotation == pdf::PageRotation::Rotate90 || extraRotation == pdf::PageRotation::Rotate270;
        return transposed ? A4.transposed() : A4;
    }

    QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(extraRotation);

        QTransform matrix;
        matrix.translate(deviceRect.left(), deviceRect.bottom());
        matrix.scale(deviceRect.width() / A4.width(), -deviceRect.height() / A4.height());
        return matrix;
    }

private:
    pdf::PDFDocument* m_document = nullptr;
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
    const pdf::PDFDocumentPointer document = builder.build();

    pdf::PDFDocumentContext context(nullptr);
    context.setDocument(document);
    QVERIFY(context.getSession() != nullptr);

    pdf::PDFDocumentSession* session = context.getSession();
    QCOMPARE(session->compileCacheLimit(), pdf::PDFDocumentSession::CompileCacheLimit);
    QVERIFY(session->prefetchEnabled());

    pdf::PDFJobScheduler scheduler(2);
    pdfinteraction::PDFJobSchedulerSubmitter submitter(scheduler);
    pdfinteraction::PDFSessionPageSurfaceRenderer renderer(context);
    pdfinteraction::PDFDocumentContextSource revisions(&context);

    pdfinteraction::ViewportController viewport;
    DocumentGeometrySource geometry(document.data());
    viewport.setGeometrySource(&geometry);
    viewport.setPixelPerMM(1.0);
    viewport.setViewportSizePx(QSize(220, 320));
    viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);

    pdfinteraction::PageSurfaceBounds bounds;
    bounds.maxInFlightBytes = 210 * 297 * 4;
    bounds.maxAdmittedBytes = 210 * 297 * 4 * 2;

    pdfinteraction::PageSurfaceCoordinator coordinator(revisions, submitter, renderer, viewport, bounds);
    coordinator.setDocumentKey(revisions.documentKey());
    coordinator.invalidate(revisions.revision());

    for (int page = 0; page < 4; ++page)
    {
        QVERIFY(session->compilePage(static_cast<size_t>(page)) != nullptr);
    }

    coordinator.requestSurfaces();
    QTRY_VERIFY_WITH_TIMEOUT(coordinator.counters().admitted >= 1, 30000);
    QCoreApplication::processEvents();

    QVERIFY(coordinator.counters().admittedBytes <= bounds.maxAdmittedBytes);
    QVERIFY(session->compileCacheLimit() <= pdf::PDFDocumentSession::CompileCacheLimit);
    QVERIFY(!session->prefetchEnabled() || session->qualityPrefetchShed() || coordinator.counters().shed > 0);
}

QTEST_GUILESS_MAIN(PageSurfaceBudgetTest)

#include "tst_pagesurfacebudgettest.moc"
