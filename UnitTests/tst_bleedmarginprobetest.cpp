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

#include "pdfbleedmarginprobe.h"
#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdffont.h"
#include "pdfoptionalcontent.h"
#include "pdfprocessingbudget.h"
#include "pdfrenderer.h"

#include <QtTest>
#include <QImage>
#include <QMarginsF>
#include <QPainter>

class BleedMarginProbeTest : public QObject
{
    Q_OBJECT

private slots:
    void probeFast_emptyPage_returnsNoContent();
    void probeFast_pageWithFullBleed_returnsAllEdgesCovered();
    void probeFast_threeEdgesPresentOneEmpty_returnsSingleEmptyEdge();
    void probeFast_asymmetricBleedDepth_treatsZeroDepthAsNotApplicable();
    void probeRaster_repeatIsDeterministicAndReportsCalibrationInputs();
    void probeRaster_chargesStripPixelsToSessionBudget();
    void renderer_emptyPage_returnsNoErrors();
};

namespace
{

pdf::PDFDocument buildThreeOfFourBleedPage()
{
    constexpr qreal mediaSizePt = 220.0;
    constexpr qreal trimInsetPt = 10.0;
    constexpr qreal trimSizePt = 200.0;

    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, mediaSizePt, mediaSizePt));
    builder.setPageTrimBox(page, QRectF(trimInsetPt, trimInsetPt, trimSizePt, trimSizePt));

    pdf::PDFPageContentStreamBuilder pageContentStreamBuilder(&builder,
                                                              pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = pageContentStreamBuilder.begin(page))
    {
        painter->fillRect(QRectF(0, trimInsetPt + 1.0, mediaSizePt, mediaSizePt - trimInsetPt - 1.0), Qt::black);
        pageContentStreamBuilder.end(painter);
    }

    return builder.build();
}

} // namespace

void BleedMarginProbeTest::probeFast_emptyPage_returnsNoContent()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PDFBleedMarginProbe probe(&session);

    const pdf::PDFCatalog* catalog = document.getCatalog();
    const pdf::PDFPage* page = catalog->getPage(0);

    pdf::PDFBleedMarginProbeSettings settings;
    settings.bleedMM = QMarginsF(3, 3, 3, 3);
    settings.referenceBox = pdf::PDFBleedFixupSettings::ReferenceBox::TrimBox;

    const pdf::PDFBleedMarginProbeResult result = probe.probeFast(page, 0, settings);
    QVERIFY(!result.allEdgesCovered());
}

void BleedMarginProbeTest::probeFast_pageWithFullBleed_returnsAllEdgesCovered()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 220, 220));
    builder.setPageTrimBox(page, QRectF(10, 10, 200, 200));

    pdf::PDFPageContentStreamBuilder pageContentStreamBuilder(&builder,
                                                              pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = pageContentStreamBuilder.begin(page))
    {
        painter->fillRect(QRectF(0, 0, 220, 220), Qt::black);
        pageContentStreamBuilder.end(painter);
    }

    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PDFBleedMarginProbe probe(&session);

    const pdf::PDFCatalog* catalog = document.getCatalog();
    const pdf::PDFPage* pagePtr = catalog->getPage(0);

    pdf::PDFBleedMarginProbeSettings settings;
    settings.bleedMM = QMarginsF(3, 3, 3, 3);
    settings.referenceBox = pdf::PDFBleedFixupSettings::ReferenceBox::TrimBox;

    const pdf::PDFBleedMarginProbeResult result = probe.probeFast(pagePtr, 0, settings);
    QVERIFY(result.allEdgesCovered());
}

void BleedMarginProbeTest::probeFast_threeEdgesPresentOneEmpty_returnsSingleEmptyEdge()
{
    pdf::PDFDocument document = buildThreeOfFourBleedPage();

    pdf::PDFDocumentSession session(&document);
    pdf::PDFBleedMarginProbe probe(&session);

    const pdf::PDFCatalog* catalog = document.getCatalog();
    const pdf::PDFPage* page = catalog->getPage(0);

    pdf::PDFBleedMarginProbeSettings settings;
    settings.bleedMM = QMarginsF(3, 3, 3, 3);
    settings.referenceBox = pdf::PDFBleedFixupSettings::ReferenceBox::TrimBox;

    const pdf::PDFBleedMarginProbeResult result = probe.probeFast(page, 0, settings);
    QVERIFY(!result.allEdgesCovered());
    QVERIFY(result.left.hasContent);
    QVERIFY(result.right.hasContent);
    QVERIFY(result.top.hasContent);
    QVERIFY(!result.bottom.hasContent);
}

void BleedMarginProbeTest::probeFast_asymmetricBleedDepth_treatsZeroDepthAsNotApplicable()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 220, 220));
    builder.setPageTrimBox(page, QRectF(10, 10, 200, 200));

    pdf::PDFPageContentStreamBuilder pageContentStreamBuilder(&builder,
                                                              pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = pageContentStreamBuilder.begin(page))
    {
        painter->fillRect(QRectF(0, 0, 220, 220), Qt::black);
        pageContentStreamBuilder.end(painter);
    }

    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PDFBleedMarginProbe probe(&session);

    const pdf::PDFCatalog* catalog = document.getCatalog();
    const pdf::PDFPage* pagePtr = catalog->getPage(0);

    pdf::PDFBleedMarginProbeSettings settings;
    settings.bleedMM = QMarginsF(3, 0, 3, 0);
    settings.referenceBox = pdf::PDFBleedFixupSettings::ReferenceBox::TrimBox;

    const pdf::PDFBleedMarginProbeResult result = probe.probeFast(pagePtr, 0, settings);
    QVERIFY(result.allEdgesCovered());
    QVERIFY(result.left.hasContent);
    QVERIFY(result.right.hasContent);
}

void BleedMarginProbeTest::probeRaster_repeatIsDeterministicAndReportsCalibrationInputs()
{
    pdf::PDFDocument document = buildThreeOfFourBleedPage();
    pdf::PDFDocumentSession session(&document);
    pdf::PDFBleedMarginProbe probe(&session);
    const pdf::PDFPage* page = document.getCatalog()->getPage(0);

    pdf::PDFBleedMarginProbeSettings settings;
    settings.dpi = 150;
    settings.threshold = 16;
    settings.whiteCoverageThreshold = 0.9975;
    settings.bleedMM = QMarginsF(3, 3, 3, 3);
    settings.referenceBox = pdf::PDFBleedFixupSettings::ReferenceBox::TrimBox;
    settings.fastOnly = false;
    settings.maxRasterPixels = 250LL * 1000 * 1000;

    const pdf::PDFBleedMarginProbeResult first = probe.probe(page, 0, settings);
    const pdf::PDFBleedMarginProbeResult second = probe.probe(page, 0, settings);
    QCOMPARE(first.left.hasContent, second.left.hasContent);
    QCOMPARE(first.right.hasContent, second.right.hasContent);
    QCOMPARE(first.top.hasContent, second.top.hasContent);
    QCOMPARE(first.bottom.hasContent, second.bottom.hasContent);
    QCOMPARE(first.left.inkPixels, second.left.inkPixels);
    QCOMPARE(first.right.inkPixels, second.right.inkPixels);
    QCOMPARE(first.top.inkPixels, second.top.inkPixels);
    QCOMPARE(first.bottom.inkPixels, second.bottom.inkPixels);
    QVERIFY(first.left.totalPixels > 0);
    QVERIFY(first.right.totalPixels > 0);
    QVERIFY(first.top.totalPixels > 0);
    QVERIFY(first.bottom.totalPixels > 0);
}

void BleedMarginProbeTest::probeRaster_chargesStripPixelsToSessionBudget()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 200, 200));
    builder.setPageTrimBox(pageReference, QRectF(10, 10, 180, 180));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PDFProcessingLimits limits = pdf::PDFProcessingLimits::conservativeDefaults();
    limits.maxRenderPixels = 1;
    session.setProcessingLimits(limits);

    pdf::PDFBleedMarginProbe probe(&session);
    const pdf::PDFPage* page = document.getCatalog()->getPage(0);

    pdf::PDFBleedMarginProbeSettings settings;
    settings.dpi = 72;
    settings.bleedMM = QMarginsF(3, 3, 3, 3);
    settings.referenceBox = pdf::PDFBleedFixupSettings::ReferenceBox::TrimBox;

    QVERIFY_THROWS_EXCEPTION(pdf::PDFBudgetExceededException, probe.probe(page, 0, settings));
}

void BleedMarginProbeTest::renderer_emptyPage_returnsNoErrors()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFOptionalContentActivity optionalContentActivity(&document, pdf::OCUsage::Export, nullptr);
    pdf::PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(&document);
    pdf::PDFCMSPointer cms = cmsManager.getCurrentCMS();
    pdf::PDFFontCache fontCache(pdf::DEFAULT_FONT_CACHE_LIMIT, pdf::DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    fontCache.setDocument(pdf::PDFModifiedDocument(&document, &optionalContentActivity));
    fontCache.setCacheShrinkEnabled(nullptr, false);

    pdf::PDFMeshQualitySettings meshQualitySettings;
    pdf::PDFRenderer renderer(&document,
                              &fontCache,
                              cms.get(),
                              &optionalContentActivity,
                              pdf::PDFRenderer::getDefaultFeatures(),
                              meshQualitySettings);

    QImage image(QSize(200, 200), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    const QList<pdf::PDFRenderError> errors = renderer.render(&painter, QRectF(0, 0, 200, 200), 0);
    painter.end();

    QVERIFY(errors.isEmpty());
}

QTEST_GUILESS_MAIN(BleedMarginProbeTest)

#include "tst_bleedmarginprobetest.moc"
