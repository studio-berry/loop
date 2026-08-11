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

#include "pdfdocumentbuilder.h"
#include "pdftransparencyflattener.h"

#include <QtTest>
#include <QPainter>

class TransparencyFlattenerTest : public QObject
{
    Q_OBJECT

private slots:
    void flattenOpaquePage_reportsFullPageRegion();
    void dryRun_doesNotMutateDocument();
};

namespace
{

pdf::PDFDocument buildDocument()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 144, 144));
    pdf::PDFPageContentStreamBuilder contentBuilder(&builder,
                                                    pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = contentBuilder.begin(page))
    {
        painter->setOpacity(0.5);
        painter->fillRect(QRectF(18, 18, 108, 108), Qt::red);
        contentBuilder.end(painter);
    }
    return builder.build();
}

} // namespace

void TransparencyFlattenerTest::flattenOpaquePage_reportsFullPageRegion()
{
    pdf::PDFDocument document = buildDocument();
    pdf::PDFTransparencyFlattenSettings settings;
    settings.rasterizationDpi = 72;
    settings.maxRasterPixels = 100000;

    pdf::PDFTransparencyFlattenReport report;
    const pdf::PDFOperationResult result = pdf::PDFTransparencyFlattener::apply(&document, settings, &report);
    QVERIFY(result);
    QVERIFY(report.changed);
    QVERIFY(report.fullyOpaque);
    QCOMPARE(report.pages.size(), 1);
    QCOMPARE(report.pages.front().regions.size(), 1);
    QVERIFY(!pdf::PDFTransparencyFlattener::hasLiveTransparency(&document));
}

void TransparencyFlattenerTest::dryRun_doesNotMutateDocument()
{
    pdf::PDFDocument document = buildDocument();
    const pdf::PDFDocument before = document;
    pdf::PDFTransparencyFlattenSettings settings;
    settings.rasterizationDpi = 72;
    settings.maxRasterPixels = 100000;
    settings.analyzeOnly = true;

    const pdf::PDFOperationResult result = pdf::PDFTransparencyFlattener::apply(&document, settings);
    QVERIFY(result);
    QVERIFY(document == before);
}

QTEST_GUILESS_MAIN(TransparencyFlattenerTest)

#include "tst_transparencyflattenertest.moc"
