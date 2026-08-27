// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "documentviewsession.h"

#include "overlaybuilder.h"
#include "pdfrenderer.h"

#include <QtTest>

class DocumentViewSessionTest final : public QObject
{
    Q_OBJECT

private slots:
    void denyExtraGraphicsSyncsFromRenderFeatures();
};

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
