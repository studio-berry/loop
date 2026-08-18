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

#include "pdfbleedfixup.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfglobal.h"

#include <QtTest>
#include <QColor>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <limits>

namespace
{

QByteArray documentDigest(const pdf::PDFDocument& document)
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid())
    {
        return {};
    }

    const QString fileName = tempDir.filePath(QStringLiteral("digest.pdf"));
    pdf::PDFDocument copy = document;
    pdf::PDFDocumentWriter writer(nullptr);
    if (!writer.write(fileName, &copy, true))
    {
        return {};
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

}

class BleedFixupTest : public QObject
{
    Q_OBJECT

private slots:
    void targetBleedRect_expandsByMillimeters();
    void sideAlreadyBleeding_detectsSufficientMargin();
    void stripWidthPx_dependsOnMode();
    void edgeStripRects_areOutsideAndInsideReference();
    void cornerStripRects_fillBleedQuadrants();
    void buildEdgeFillImage_mirrorFlipsHorizontally();
    void buildCornerFillImage_mirrorFlipsBothAxes();
    void buildCornerFillImage_pixelRepeatTilesCornerPixel();
    void buildEdgeFillImage_pixelRepeatTilesEdge();
    void buildEdgeFillImage_stretchScalesToBleedDepth();
    void apply_selectedSidesOnly_reportsAndExpandsSelectedEdges();
    void apply_normalLetterWithinBudget_preservesBleedSemantics();
    void rasterPlan_largeFormatRejectsBeforeAllocation();
    void rasterPlan_budgetChangesWithDpiAndLimit();
    void rasterPlan_invalidDimensionsFailBeforeNarrowing();
    void analyzeOnly_largeFormatKeepsDocumentUnchanged();
};

void BleedFixupTest::targetBleedRect_expandsByMillimeters()
{
    const QRectF reference(0.0, 0.0, 100.0, 200.0);
    const QMarginsF bleedMM(3.0, 3.0, 3.0, 3.0);
    const QRectF target = pdf::PDFBleedFixupMath::targetBleedRect(reference, bleedMM);
    const qreal expected = 3.0 * pdf::PDF_MM_TO_POINT;

    QCOMPARE(target.left(), reference.left() - expected);
    QCOMPARE(target.right(), reference.right() + expected);
    QCOMPARE(target.top(), reference.top() - expected);
    QCOMPARE(target.bottom(), reference.bottom() + expected);
}

void BleedFixupTest::sideAlreadyBleeding_detectsSufficientMargin()
{
    const QRectF reference(10.0, 10.0, 100.0, 100.0);
    const QRectF bleed(0.0, 0.0, 120.0, 120.0); // 10pt each side
    QVERIFY(pdf::PDFBleedFixupMath::sideAlreadyBleeding(reference, bleed, pdf::PDFBleedFixupSide::Left, 9.0));
    QVERIFY(!pdf::PDFBleedFixupMath::sideAlreadyBleeding(reference, bleed, pdf::PDFBleedFixupSide::Left, 11.0));
}

void BleedFixupTest::stripWidthPx_dependsOnMode()
{
    QCOMPARE(pdf::PDFBleedFixupMath::stripWidthPx(pdf::PDFBleedFixupMode::Mirror, 35, 1), 35);
    QCOMPARE(pdf::PDFBleedFixupMath::stripWidthPx(pdf::PDFBleedFixupMode::PixelRepeat, 35, 1), 1);
    QCOMPARE(pdf::PDFBleedFixupMath::stripWidthPx(pdf::PDFBleedFixupMode::Stretch, 35, 2), 2);
}

void BleedFixupTest::edgeStripRects_areOutsideAndInsideReference()
{
    const QRectF reference(10.0, 20.0, 100.0, 200.0);
    const qreal depth = 5.0;

    const QRectF leftSrc = pdf::PDFBleedFixupMath::edgeStripSourceRect(reference, pdf::PDFBleedFixupSide::Left, depth);
    const QRectF leftDst = pdf::PDFBleedFixupMath::edgeStripDestRect(reference, pdf::PDFBleedFixupSide::Left, depth);
    QCOMPARE(leftSrc.left(), 10.0);
    QCOMPARE(leftDst.right(), 10.0);
    QCOMPARE(leftDst.left(), 5.0);

    const QRectF topSrc = pdf::PDFBleedFixupMath::edgeStripSourceRect(reference, pdf::PDFBleedFixupSide::Top, depth);
    const QRectF topDst = pdf::PDFBleedFixupMath::edgeStripDestRect(reference, pdf::PDFBleedFixupSide::Top, depth);
    QCOMPARE(topSrc.bottom(), reference.bottom());
    QCOMPARE(topDst.top(), reference.bottom());
}

void BleedFixupTest::cornerStripRects_fillBleedQuadrants()
{
    const QRectF reference(10.0, 20.0, 100.0, 200.0);
    const qreal leftDepth = 4.0;
    const qreal topDepth = 6.0;

    const QRectF source = pdf::PDFBleedFixupMath::cornerStripSourceRect(
            reference, pdf::PDFBleedFixupSide::Left, pdf::PDFBleedFixupSide::Top, leftDepth, topDepth);
    const QRectF dest = pdf::PDFBleedFixupMath::cornerStripDestRect(
            reference, pdf::PDFBleedFixupSide::Left, pdf::PDFBleedFixupSide::Top, leftDepth, topDepth);

    QCOMPARE(source.left(), 10.0);
    QCOMPARE(source.right(), 14.0);
    QCOMPARE(source.bottom(), reference.bottom());
    QCOMPARE(source.top(), reference.bottom() - topDepth);

    QCOMPARE(dest.right(), 10.0);
    QCOMPARE(dest.left(), 6.0);
    QCOMPARE(dest.top(), reference.bottom());
    QCOMPARE(dest.bottom(), reference.bottom() + topDepth);
    QVERIFY(!source.intersects(dest));

    QVERIFY(!pdf::PDFBleedFixupMath::cornerStripDestRect(
            reference, pdf::PDFBleedFixupSide::Top, pdf::PDFBleedFixupSide::Left, leftDepth, topDepth).isValid());
}

void BleedFixupTest::buildEdgeFillImage_mirrorFlipsHorizontally()
{
    QImage page(4, 2, QImage::Format_ARGB32);
    page.fill(Qt::black);
    page.setPixel(0, 0, qRgb(255, 0, 0));
    page.setPixel(1, 0, qRgb(0, 255, 0));
    page.setPixel(2, 0, qRgb(0, 0, 255));
    page.setPixel(3, 0, qRgb(255, 255, 0));

    const QImage fill = pdf::PDFBleedFixupMath::buildEdgeFillImage(page, QRect(0, 0, 2, 2),
                                                                   pdf::PDFBleedFixupSide::Left,
                                                                   pdf::PDFBleedFixupMode::Mirror,
                                                                   2);
    QVERIFY(!fill.isNull());
    QCOMPARE(fill.width(), 2);
    QCOMPARE(qRed(fill.pixel(0, 0)), 0);
    QCOMPARE(qGreen(fill.pixel(0, 0)), 255);
    QCOMPARE(qRed(fill.pixel(1, 0)), 255);
}

void BleedFixupTest::buildCornerFillImage_mirrorFlipsBothAxes()
{
    QImage page(2, 2, QImage::Format_ARGB32);
    page.setPixel(0, 0, qRgb(255, 0, 0));
    page.setPixel(1, 0, qRgb(0, 255, 0));
    page.setPixel(0, 1, qRgb(0, 0, 255));
    page.setPixel(1, 1, qRgb(255, 255, 0));

    const QImage fill = pdf::PDFBleedFixupMath::buildCornerFillImage(
            page, QRect(0, 0, 2, 2),
            pdf::PDFBleedFixupSide::Left, pdf::PDFBleedFixupSide::Top,
            pdf::PDFBleedFixupMode::Mirror, 2, 2);
    QVERIFY(!fill.isNull());
    QCOMPARE(fill.size(), QSize(2, 2));
    QCOMPARE(qRed(fill.pixel(0, 0)), 255);
    QCOMPARE(qGreen(fill.pixel(0, 0)), 255);
    QCOMPARE(qBlue(fill.pixel(1, 0)), 255);
    QCOMPARE(qGreen(fill.pixel(0, 1)), 255);
    QCOMPARE(qRed(fill.pixel(1, 1)), 255);
}

void BleedFixupTest::buildCornerFillImage_pixelRepeatTilesCornerPixel()
{
    QImage page(2, 2, QImage::Format_ARGB32);
    page.fill(Qt::black);
    page.setPixel(0, 0, qRgb(10, 20, 30));

    const QImage fill = pdf::PDFBleedFixupMath::buildCornerFillImage(
            page, QRect(0, 0, 2, 2),
            pdf::PDFBleedFixupSide::Left, pdf::PDFBleedFixupSide::Top,
            pdf::PDFBleedFixupMode::PixelRepeat, 3, 4);
    QCOMPARE(fill.size(), QSize(3, 4));
    QCOMPARE(qRed(fill.pixel(2, 3)), 10);
    QCOMPARE(qGreen(fill.pixel(0, 0)), 20);
    QCOMPARE(qBlue(fill.pixel(1, 1)), 30);
}

void BleedFixupTest::buildEdgeFillImage_pixelRepeatTilesEdge()
{
    QImage page(3, 2, QImage::Format_ARGB32);
    page.fill(Qt::black);
    page.setPixel(0, 0, qRgb(10, 20, 30));
    page.setPixel(1, 0, qRgb(40, 50, 60));

    const QImage fill = pdf::PDFBleedFixupMath::buildEdgeFillImage(page, QRect(0, 0, 1, 2),
                                                                   pdf::PDFBleedFixupSide::Left,
                                                                   pdf::PDFBleedFixupMode::PixelRepeat,
                                                                   4);
    QCOMPARE(fill.width(), 4);
    QCOMPARE(fill.height(), 2);
    for (int x = 0; x < 4; ++x)
    {
        QCOMPARE(qRed(fill.pixel(x, 0)), 10);
        QCOMPARE(qGreen(fill.pixel(x, 0)), 20);
        QCOMPARE(qBlue(fill.pixel(x, 0)), 30);
    }
}

void BleedFixupTest::buildEdgeFillImage_stretchScalesToBleedDepth()
{
    QImage page(2, 4, QImage::Format_ARGB32);
    page.fill(qRgb(7, 8, 9));

    const QImage fill = pdf::PDFBleedFixupMath::buildEdgeFillImage(page, QRect(0, 0, 1, 4),
                                                                   pdf::PDFBleedFixupSide::Left,
                                                                   pdf::PDFBleedFixupMode::Stretch,
                                                                   5);
    QCOMPARE(fill.width(), 5);
    QCOMPARE(fill.height(), 4);
}

void BleedFixupTest::apply_selectedSidesOnly_reportsAndExpandsSelectedEdges()
{
    pdf::PDFDocumentBuilder builder;
    const QRectF media(0.0, 0.0, 100.0, 100.0);
    const pdf::PDFObjectReference pageReference = builder.appendPage(media);
    builder.setPageTrimBox(pageReference, QRectF(10.0, 10.0, 80.0, 80.0));
    builder.setPageBleedBox(pageReference, QRectF(10.0, 10.0, 80.0, 80.0));
    pdf::PDFDocument document = builder.build();

    pdf::PDFBleedFixupSettings settings;
    settings.analyzeOnly = true;
    settings.force = true;
    settings.sides = pdf::bleedFixupSideBit(pdf::PDFBleedFixupSide::Left);

    pdf::PDFBleedFixupReport report;
    const pdf::PDFOperationResult result = pdf::PDFBleedFixup::apply(&document, settings, &report);
    QVERIFY2(result, qPrintable(result.getErrorMessage()));
    QCOMPARE(report.pages.size(), 1);

    const pdf::PDFBleedFixupPageReport& page = report.pages.front();
    QVERIFY(pdf::isBleedFixupSideEnabled(page.sidesRequested, pdf::PDFBleedFixupSide::Left));
    QVERIFY(pdf::isBleedFixupSideEnabled(page.sidesEligible, pdf::PDFBleedFixupSide::Left));
    QCOMPARE(page.sidesRequested & pdf::bleedFixupSideBit(pdf::PDFBleedFixupSide::Right), quint8(0));
    QCOMPARE(page.sidesEligible & pdf::bleedFixupSideBit(pdf::PDFBleedFixupSide::Right), quint8(0));
    QVERIFY(page.sidesApplied.contains(pdf::PDFBleedFixupSide::Left));
    QCOMPARE(page.newBleedBox.right(), page.originalBleedBox.right());
    QCOMPARE(page.newBleedBox.top(), page.originalBleedBox.top());
    QVERIFY(page.newBleedBox.left() < page.originalBleedBox.left());
}

void BleedFixupTest::apply_normalLetterWithinBudget_preservesBleedSemantics()
{
    pdf::PDFDocumentBuilder builder;
    const QRectF media(0.0, 0.0, 8.5 * 72.0, 11.0 * 72.0);
    const pdf::PDFObjectReference pageReference = builder.appendPage(media);
    builder.setPageTrimBox(pageReference, media.adjusted(18, 18, -18, -18));
    builder.setPageBleedBox(pageReference, media.adjusted(18, 18, -18, -18));
    pdf::PDFDocument document = builder.build();

    pdf::PDFBleedFixupSettings settings;
    settings.force = true;
    settings.dpi = 72;
    settings.sides = pdf::bleedFixupSideBit(pdf::PDFBleedFixupSide::Left);

    pdf::PDFBleedFixupReport report;
    const pdf::PDFOperationResult result = pdf::PDFBleedFixup::apply(&document, settings, &report);
    QVERIFY2(result, qPrintable(result.getErrorMessage()));
    QCOMPARE(report.pages.size(), 1);
    QVERIFY(report.pages.front().sidesApplied.contains(pdf::PDFBleedFixupSide::Left));
    QVERIFY(report.pages.front().newBleedBox.left() < report.pages.front().originalBleedBox.left());
}

void BleedFixupTest::rasterPlan_largeFormatRejectsBeforeAllocation()
{
    pdf::PDFDocumentBuilder builder;
    const QRectF media(0.0, 0.0, 48.0 * 72.0, 96.0 * 72.0);
    const pdf::PDFObjectReference pageReference = builder.appendPage(media);
    builder.setPageTrimBox(pageReference, media);
    pdf::PDFDocument document = builder.build();

    pdf::PDFBleedFixupSettings settings;
    settings.force = true;
    const QByteArray before = documentDigest(document);
    QVERIFY(!before.isEmpty());
    pdf::PDFBleedFixupReport report;
    const pdf::PDFOperationResult applyResult = pdf::PDFBleedFixup::apply(&document, settings, &report);
    QVERIFY(!applyResult);
    QVERIFY(applyResult.getErrorMessage().contains(QStringLiteral("14400 x 28800")));
    QCOMPARE(documentDigest(document), before);

    const pdf::PDFBleedFixupMath::PDFBleedRasterPlan plan = pdf::PDFBleedFixupMath::planRaster(
            QSizeF(48.0 * 72.0, 96.0 * 72.0), 300, 250LL * 1000 * 1000, false, true);

    QVERIFY(plan.rasterRequired);
    QVERIFY(!plan.withinBudget);
    QVERIFY(plan.imageSize.isEmpty());
    QVERIFY(plan.pixelCount > 400000000.0);
    QVERIFY(plan.errorMessage.contains(QStringLiteral("14400 x 28800")));
    QVERIFY(plan.errorMessage.contains(QStringLiteral("300 DPI")));
    QVERIFY(plan.errorMessage.contains(QStringLiteral("exceeds the limit")));

    const pdf::PDFBleedFixupMath::PDFBleedRasterPlan wideFormat = pdf::PDFBleedFixupMath::planRaster(
            QSizeF(240.0 * 72.0, 60.0 * 72.0), 300, 250LL * 1000 * 1000, false, true);
    QVERIFY(!wideFormat.withinBudget);
    QVERIFY(wideFormat.imageSize.isEmpty());
    QVERIFY(wideFormat.errorMessage.contains(QStringLiteral("72000 x 18000")));
    QVERIFY(wideFormat.errorMessage.contains(QStringLiteral("exceeds the limit")));
}

void BleedFixupTest::rasterPlan_budgetChangesWithDpiAndLimit()
{
    const QSizeF mediaSize(48.0 * 72.0, 96.0 * 72.0);
    const pdf::PDFBleedFixupMath::PDFBleedRasterPlan lowerDpi = pdf::PDFBleedFixupMath::planRaster(
            mediaSize, 72, 250LL * 1000 * 1000, false, true);
    QVERIFY(lowerDpi.withinBudget);
    QCOMPARE(lowerDpi.imageSize, QSize(3456, 6912));

    const pdf::PDFBleedFixupMath::PDFBleedRasterPlan higherLimit = pdf::PDFBleedFixupMath::planRaster(
            mediaSize, 300, 500LL * 1000 * 1000, false, true);
    QVERIFY(higherLimit.withinBudget);
    QCOMPARE(higherLimit.imageSize, QSize(14400, 28800));
}

void BleedFixupTest::rasterPlan_invalidDimensionsFailBeforeNarrowing()
{
    const pdf::PDFBleedFixupMath::PDFBleedRasterPlan nonFinite = pdf::PDFBleedFixupMath::planRaster(
            QSizeF(std::numeric_limits<double>::infinity(), 100.0), 300, 250LL * 1000 * 1000, false, true);
    QVERIFY(!nonFinite.withinBudget);
    QVERIFY(!nonFinite.errorMessage.isEmpty());

    const pdf::PDFBleedFixupMath::PDFBleedRasterPlan extreme = pdf::PDFBleedFixupMath::planRaster(
            QSizeF(std::numeric_limits<double>::max(), std::numeric_limits<double>::max()), 300, 0, false, true);
    QVERIFY(!extreme.withinBudget);
    QVERIFY(!extreme.errorMessage.isEmpty());
}

void BleedFixupTest::analyzeOnly_largeFormatKeepsDocumentUnchanged()
{
    pdf::PDFDocumentBuilder builder;
    const QRectF media(0.0, 0.0, 48.0 * 72.0, 96.0 * 72.0);
    const pdf::PDFObjectReference pageReference = builder.appendPage(media);
    builder.setPageTrimBox(pageReference, media);
    pdf::PDFDocument document = builder.build();

    const QByteArray before = documentDigest(document);
    QVERIFY(!before.isEmpty());

    pdf::PDFBleedFixupSettings settings;
    settings.analyzeOnly = true;
    settings.force = true;
    settings.dpi = 300;
    settings.maxRasterPixels = 1;

    pdf::PDFBleedFixupReport report;
    const pdf::PDFOperationResult result = pdf::PDFBleedFixup::apply(&document, settings, &report);
    QVERIFY2(result, qPrintable(result.getErrorMessage()));
    QCOMPARE(report.pages.size(), 1);
    QVERIFY(!report.pages.front().sidesApplied.isEmpty());
    QCOMPARE(documentDigest(document), before);

    const pdf::PDFBleedFixupMath::PDFBleedRasterPlan plan = pdf::PDFBleedFixupMath::planRaster(
            media.size(), settings.dpi, settings.maxRasterPixels, settings.analyzeOnly, true);
    QVERIFY(!plan.rasterRequired);
    QVERIFY(plan.withinBudget);
    QVERIFY(plan.imageSize.isEmpty());
}

QTEST_APPLESS_MAIN(BleedFixupTest)

#include "tst_bleedfixuptest.moc"
