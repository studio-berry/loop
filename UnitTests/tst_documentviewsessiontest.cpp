// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "documentviewsession.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdfpagecachebudget.h"

#include "overlaybuilder.h"
#include "pdfrenderer.h"

#include <QtTest>

class DocumentViewSessionTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultCacheLimitPartitions();
    void cacheLimitNormalizesAndReappliesAfterDocumentReplacement();
    void denyExtraGraphicsSyncsFromRenderFeatures();
};

void DocumentViewSessionTest::defaultCacheLimitPartitions()
{
    DocumentViewSession session;
    pdf::PDFDocumentSession* documentSession = session.context().getSession();
    QVERIFY(documentSession != nullptr);
    QVERIFY(session.surfaces() != nullptr);

    const qsizetype total = DocumentViewSession::DefaultCacheLimit;
    QCOMPARE(session.cacheLimit(), total);
    QCOMPARE(documentSession->cacheLimit(), total);
    QCOMPARE(documentSession->compiledCacheByteLimit(), pdf::PDFPageCacheBudget::compiledPages(total));
    QCOMPARE(session.surfaces()->cacheLimit(), total);
    QCOMPARE(session.surfaces()->bounds().maxAdmittedBytes,
             static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(total)));
}

void DocumentViewSessionTest::cacheLimitNormalizesAndReappliesAfterDocumentReplacement()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    DocumentViewSession session;
    session.setCacheLimit(101);

    auto assertPartitions = [&session](qsizetype total)
    {
        pdf::PDFDocumentSession* documentSession = session.context().getSession();
        QVERIFY(documentSession != nullptr);
        QCOMPARE(session.cacheLimit(), total);
        QCOMPARE(documentSession->cacheLimit(), total);
        QCOMPARE(documentSession->compiledCacheByteLimit(), pdf::PDFPageCacheBudget::compiledPages(total));
        QCOMPARE(session.surfaces()->cacheLimit(), total);
        QCOMPARE(session.surfaces()->bounds().maxAdmittedBytes,
                 static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(total)));
    };

    assertPartitions(101);

    session.setCacheLimit(-1);
    assertPartitions(0);

    session.setCacheLimit(101);
    session.context().setDocument(&document);
    session.prepareDocumentView();
    assertPartitions(101);
}

void DocumentViewSessionTest::denyExtraGraphicsSyncsFromRenderFeatures()
{
    DocumentViewSession session;
    pdf::PDFRenderer::Features features = pdf::PDFRenderer::getDefaultFeatures();
    features |= pdf::PDFRenderer::DenyExtraGraphics;
    session.setSurfaceRenderFeatures(features);
    QVERIFY(session.overlays()->denyExtraGraphics());

    features &= ~pdf::PDFRenderer::DenyExtraGraphics;
    session.setSurfaceRenderFeatures(features);
    QVERIFY(!session.overlays()->denyExtraGraphics());
}

QTEST_GUILESS_MAIN(DocumentViewSessionTest)

#include "tst_documentviewsessiontest.moc"
