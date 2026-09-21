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

// CTest corpus for bizarre page boxes: inverted, zero-area, negative-origin,
// rotated, ArtBox outside MediaBox, and invalid rotation fail-closed.
// Fixtures live in loop-preflight/testdata/page-box-corpus/ and are regenerated
// with loop-preflight/tools/generate_page_box_corpus.py.

#include "pdfcms.h"
#include "pdfdocumentreader.h"
#include "pdfoptionalcontent.h"
#include "pdfpage.h"
#include "pdfrenderer.h"
#include "pdfdocumentsession.h"
#include "preflightengine.h"

#include <QtTest>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace
{

constexpr qreal BOX_EPSILON = 0.01;

QString corpusDir()
{
    return QStringLiteral(PAGE_BOX_CORPUS_DIR);
}

bool fuzzyRectEqual(const QRectF& actual, const QJsonArray& expected)
{
    if (expected.size() != 4)
    {
        return false;
    }

    const QRectF rect(expected.at(0).toDouble(),
                      expected.at(1).toDouble(),
                      expected.at(2).toDouble(),
                      expected.at(3).toDouble());
    return qAbs(actual.x() - rect.x()) < BOX_EPSILON && qAbs(actual.y() - rect.y()) < BOX_EPSILON && qAbs(actual.width() - rect.width()) < BOX_EPSILON && qAbs(actual.height() - rect.height()) < BOX_EPSILON;
}

pdf::PageRotation rotationFromString(const QString& value)
{
    if (value == QLatin1String("Rotate90"))
    {
        return pdf::PageRotation::Rotate90;
    }
    if (value == QLatin1String("Rotate180"))
    {
        return pdf::PageRotation::Rotate180;
    }
    if (value == QLatin1String("Rotate270"))
    {
        return pdf::PageRotation::Rotate270;
    }
    return pdf::PageRotation::None;
}

int countNonWhitePixels(const QImage& image)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    int count = 0;
    for (int y = 0; y < rgba.height(); ++y)
    {
        const uchar* line = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
        {
            const int offset = x * 4;
            if (line[offset] < 250 || line[offset + 1] < 250 || line[offset + 2] < 250)
            {
                ++count;
            }
        }
    }
    return count;
}

QImage renderPage(pdf::PDFDocument* document, pdf::PDFRenderer::Features features)
{
    pdf::PDFCMSGeneric cms;
    pdf::PDFOptionalContentActivity activity(document, pdf::OCUsage::View, nullptr);
    pdf::PDFModifiedDocument modifiedDocument(document, &activity);
    pdf::PDFFontCache fontCache(16, 16);
    fontCache.setDocument(modifiedDocument);

    pdf::PDFRenderer renderer(document,
                              &fontCache,
                              &cms,
                              &activity,
                              features,
                              pdf::PDFMeshQualitySettings());

    const QSize imageSize(128, 128);
    QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);

    QPainter painter(&image);
    renderer.render(&painter, QRectF(QPointF(0, 0), imageSize), 0);
    painter.end();

    return image;
}

QJsonArray loadManifest()
{
    QFile manifestFile(corpusDir() + QStringLiteral("/manifest.json"));
    if (!manifestFile.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QJsonDocument::fromJson(manifestFile.readAll()).array();
}

}   // namespace

class PageBoxCorpusTest : public QObject
{
    Q_OBJECT

private slots:
    void readerMatchesManifest_data();
    void readerMatchesManifest();

    void bleedPreflightPassesOnInvertedMediaBox();
    void cropClipReducesPaintedAreaOnInvertedMediaBox();
};

void PageBoxCorpusTest::readerMatchesManifest_data()
{
    QTest::addColumn<QString>("fixtureId");

    QFile manifestFile(corpusDir() + QStringLiteral("/manifest.json"));
    if (!manifestFile.open(QIODevice::ReadOnly))
    {
        QSKIP("Page-box corpus manifest is missing");
    }
    const QJsonArray manifest = QJsonDocument::fromJson(manifestFile.readAll()).array();
    for (const QJsonValue& entryValue : manifest)
    {
        const QJsonObject entry = entryValue.toObject();
        QTest::newRow(qPrintable(entry.value(QStringLiteral("id")).toString()))
            << entry.value(QStringLiteral("id")).toString();
    }
}

void PageBoxCorpusTest::readerMatchesManifest()
{
    QFETCH(QString, fixtureId);

    const QJsonArray manifest = loadManifest();
    QJsonObject entry;
    for (const QJsonValue& entryValue : manifest)
    {
        const QJsonObject candidate = entryValue.toObject();
        if (candidate.value(QStringLiteral("id")).toString() == fixtureId)
        {
            entry = candidate;
            break;
        }
    }
    QVERIFY2(!entry.isEmpty(), qPrintable(QStringLiteral("Missing manifest entry for %1").arg(fixtureId)));

    const QJsonObject reader = entry.value(QStringLiteral("reader")).toObject();
    const QString pdfPath = corpusDir() + QLatin1Char('/') + entry.value(QStringLiteral("pdf")).toString();

    pdf::PDFDocumentReader readerEngine(nullptr, [](bool*)
                                        { return QString(); }, true, false);
    const pdf::PDFDocument document = readerEngine.readFromFile(pdfPath);

    if (reader.value(QStringLiteral("result")).toString() == QLatin1String("failed"))
    {
        QCOMPARE(readerEngine.getReadingResult(), pdf::PDFDocumentReader::Result::Failed);
        const QString errorContains = reader.value(QStringLiteral("error_contains")).toString();
        if (!errorContains.isEmpty())
        {
            QVERIFY(readerEngine.getErrorMessage().contains(errorContains));
        }
        return;
    }

    QCOMPARE(readerEngine.getReadingResult(), pdf::PDFDocumentReader::Result::OK);
    QVERIFY(document.getCatalog() != nullptr);
    QCOMPARE(document.getCatalog()->getPageCount(), size_t(1));

    const pdf::PDFPage* page = document.getCatalog()->getPage(0);
    QVERIFY(page != nullptr);

    if (reader.contains(QStringLiteral("media_box")))
    {
        QVERIFY(fuzzyRectEqual(page->getMediaBox(), reader.value(QStringLiteral("media_box")).toArray()));
    }
    if (reader.contains(QStringLiteral("crop_box")))
    {
        QVERIFY(fuzzyRectEqual(page->getCropBox(), reader.value(QStringLiteral("crop_box")).toArray()));
    }
    if (reader.contains(QStringLiteral("bleed_box")))
    {
        QVERIFY(fuzzyRectEqual(page->getBleedBox(), reader.value(QStringLiteral("bleed_box")).toArray()));
    }
    if (reader.contains(QStringLiteral("trim_box")))
    {
        QVERIFY(fuzzyRectEqual(page->getTrimBox(), reader.value(QStringLiteral("trim_box")).toArray()));
    }
    if (reader.contains(QStringLiteral("art_box")))
    {
        QVERIFY(fuzzyRectEqual(page->getArtBox(), reader.value(QStringLiteral("art_box")).toArray()));
    }
    if (reader.contains(QStringLiteral("rotation")))
    {
        QCOMPARE(page->getPageRotation(), rotationFromString(reader.value(QStringLiteral("rotation")).toString()));
    }
}

void PageBoxCorpusTest::bleedPreflightPassesOnInvertedMediaBox()
{
    const QString pdfPath = corpusDir() + QStringLiteral("/pagebox-inverted-media.pdf");

    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(pdfPath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Inverted page-box bleed") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("bleed") },
                                            { QStringLiteral("amount_pt"), 9 },
                                            { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QVERIFY(result.errors.isEmpty());
}

void PageBoxCorpusTest::cropClipReducesPaintedAreaOnInvertedMediaBox()
{
    const QString pdfPath = corpusDir() + QStringLiteral("/pagebox-inverted-media.pdf");

    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(pdfPath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFRenderer::Features withoutClip = pdf::PDFRenderer::getDefaultFeatures();
    withoutClip.setFlag(pdf::PDFRenderer::ClipToCropBox, false);
    const QImage unclipped = renderPage(&document, withoutClip);
    QVERIFY(!unclipped.isNull());

    pdf::PDFRenderer::Features withClip = pdf::PDFRenderer::getDefaultFeatures();
    withClip.setFlag(pdf::PDFRenderer::ClipToCropBox, true);
    const QImage clipped = renderPage(&document, withClip);
    QVERIFY(!clipped.isNull());

    const int unclippedPixels = countNonWhitePixels(unclipped);
    const int clippedPixels = countNonWhitePixels(clipped);
    QVERIFY(clippedPixels < unclippedPixels);
    QVERIFY(unclippedPixels > 0);
}

QTEST_APPLESS_MAIN(PageBoxCorpusTest)
#include "tst_pageboxcorpustest.moc"
