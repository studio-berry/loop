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

#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentsession.h"
#include "pdfdocumentwriter.h"
#include "pdfimagedownsamplefixup.h"
#include "pdfimage.h"
#include "pdfimageoptimizer.h"
#include "preflightengine.h"

#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>

namespace
{

QImage makeImage(int pixels, bool noisy, bool withAlpha)
{
    QImage image(pixels, pixels, QImage::Format_ARGB32);
    for (int y = 0; y < pixels; ++y)
    {
        for (int x = 0; x < pixels; ++x)
        {
            if (!noisy)
            {
                image.setPixel(x, y, withAlpha ? qRgba(30, 120, 200, 255) : qRgb(30, 120, 200));
                continue;
            }

            const int r = (x * 17 + y * 13) % 256;
            const int g = (x * 7 + y * 29) % 256;
            const int b = (x * 31 + y * 3) % 256;
            const int alpha = withAlpha ? (80 + ((x + y) % 176)) : 255;
            image.setPixel(x, y, qRgba(r, g, b, alpha));
        }
    }
    return image;
}

pdf::PDFDocument createDocumentWithImage(int pixels, bool noisy, bool withAlpha)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));

    const QImage image = makeImage(pixels, noisy, withAlpha);
    pdf::PDFImage::ImageEncodeOptions options;
    options.compression = pdf::PDFImage::ImageCompression::Flate;
    options.colorMode = pdf::PDFImage::ImageColorMode::Preserve;
    options.alphaHandling = withAlpha
        ? pdf::PDFImage::AlphaHandling::DropAlphaPreserveColors
        : pdf::PDFImage::AlphaHandling::FlattenToWhite;
    pdf::PDFStream imageStream = pdf::PDFImage::createStreamFromImage(image, options);

    if (withAlpha)
    {
        QImage mask = image.createAlphaMask();
        pdf::PDFImage::ImageEncodeOptions maskOptions;
        maskOptions.compression = pdf::PDFImage::ImageCompression::Flate;
        maskOptions.colorMode = pdf::PDFImage::ImageColorMode::Grayscale;
        maskOptions.alphaHandling = pdf::PDFImage::AlphaHandling::FlattenToWhite;
        pdf::PDFStream maskStream = pdf::PDFImage::createStreamFromImage(mask, maskOptions);
        const pdf::PDFObjectReference maskReference = builder.addObject(
            pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(maskStream))));
        pdf::PDFDictionary imageDictionary = *imageStream.getDictionary();
        imageDictionary.setEntry(pdf::PDFInplaceOrMemoryString("SMask"), pdf::PDFObject::createReference(maskReference));
        const QByteArray content = imageStream.getContent() ? *imageStream.getContent() : QByteArray();
        imageStream = pdf::PDFStream(std::move(imageDictionary), content);
    }

    const pdf::PDFObjectReference imageReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(imageStream))));
    const QByteArray pageContent("q 144 0 0 144 0 0 cm /Im1 Do Q");
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                                pdf::PDFObject::createInteger(pageContent.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFStream(std::move(contentDictionary), pageContent))));

    pdf::PDFDictionary xObject;
    xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(pageReference,
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    return builder.build();
}

} // namespace

class ImageDownsampleFixupTest : public QObject
{
    Q_OBJECT

private slots:
    void thresholdBehavior_data();
    void thresholdBehavior();
    void downsampleHighDpiImage();
    void preservesImageBelowThreshold();
    void preservesColorCharacteristics();
    void preservesSoftMask();
    void keepsOriginalWhenOutputIsLarger();
    void rejectsInvalidTargetDpi();
    void doesNotMutateSourceDocument();
    void writesReopensAndPassesPostFixPreflight();
};

void ImageDownsampleFixupTest::thresholdBehavior_data()
{
    QTest::addColumn<int>("sourcePixels");
    QTest::addColumn<bool>("expectedCandidate");

    QTest::newRow("299 DPI") << 598 << false;
    QTest::newRow("300 DPI") << 600 << false;
    QTest::newRow("330 DPI") << 660 << false;
    QTest::newRow("344 DPI") << 688 << false;
    QTest::newRow("346 DPI") << 692 << true;
    QTest::newRow("600 DPI") << 1200 << true;
}

void ImageDownsampleFixupTest::thresholdBehavior()
{
    QFETCH(int, sourcePixels);
    QFETCH(bool, expectedCandidate);

    pdf::PDFDocument document = createDocumentWithImage(sourcePixels, true, false);
    const std::vector<pdf::PDFImageOptimizer::ImageInfo> infos = pdf::PDFImageOptimizer::collectImageInfos(&document);
    QCOMPARE(infos.size(), 1u);

    pdf::PDFImageDownsampleFixupSettings settings;
    settings.targetDpi = 300;
    const pdf::PDFDocument original = document;
    pdf::PDFImageDownsampleFixupReport report;
    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&document, settings, &report));
    QCOMPARE(report.imagesExamined, 1);
    QCOMPARE(report.imagesChanged > 0, expectedCandidate);
    if (!expectedCandidate)
    {
        QCOMPARE(document, original);
    }
}

void ImageDownsampleFixupTest::downsampleHighDpiImage()
{
    pdf::PDFDocument document = createDocumentWithImage(1200, true, false);
    pdf::PDFImageDownsampleFixupSettings settings;
    settings.targetDpi = 300;
    pdf::PDFImageDownsampleFixupReport report;

    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&document, settings, &report));
    QCOMPARE(report.imagesExamined, 1);
    QCOMPARE(report.imagesChanged, 1);
    QVERIFY(report.resultingBytes < report.originalBytes);
}

void ImageDownsampleFixupTest::preservesImageBelowThreshold()
{
    pdf::PDFDocument document = createDocumentWithImage(600, true, false);
    const pdf::PDFDocument original = document;
    pdf::PDFImageDownsampleFixupReport report;

    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&document,
                                                pdf::PDFImageDownsampleFixupSettings(),
                                                &report));
    QCOMPARE(report.imagesSkipped, 1);
    QCOMPARE(report.imagesChanged, 0);
    QCOMPARE(document, original);
}

void ImageDownsampleFixupTest::preservesColorCharacteristics()
{
    pdf::PDFDocument original = createDocumentWithImage(1200, true, false);
    const std::vector<pdf::PDFImageOptimizer::ImageInfo> infos = pdf::PDFImageOptimizer::collectImageInfos(&original);
    QVERIFY(!infos.empty());
    const pdf::PDFObject& originalObject = original.getObjectByReference(infos.front().reference);
    const QByteArray originalColorSpace = original.getObject(originalObject.getStream()->getDictionary()->get("ColorSpace")).getString();

    pdf::PDFDocument optimized = original;
    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&optimized,
                                                pdf::PDFImageDownsampleFixupSettings(),
                                                nullptr));
    const pdf::PDFObject& optimizedObject = optimized.getObjectByReference(infos.front().reference);
    const QByteArray optimizedColorSpace = optimized.getObject(optimizedObject.getStream()->getDictionary()->get("ColorSpace")).getString();
    QCOMPARE(optimizedColorSpace, originalColorSpace);
}

void ImageDownsampleFixupTest::preservesSoftMask()
{
    pdf::PDFDocument document = createDocumentWithImage(1200, true, true);
    const std::vector<pdf::PDFImageOptimizer::ImageInfo> infos = pdf::PDFImageOptimizer::collectImageInfos(&document);
    QVERIFY(!infos.empty());

    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&document,
                                                pdf::PDFImageDownsampleFixupSettings(),
                                                nullptr));
    const pdf::PDFObject& imageObject = document.getObjectByReference(infos.front().reference);
    QVERIFY(imageObject.isStream());
    QVERIFY(imageObject.getStream()->getDictionary()->hasKey("SMask"));
}

void ImageDownsampleFixupTest::keepsOriginalWhenOutputIsLarger()
{
    pdf::PDFDocument document = createDocumentWithImage(1200, false, false);
    pdf::PDFImageDownsampleFixupReport report;
    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&document,
                                                pdf::PDFImageDownsampleFixupSettings(),
                                                &report));
    QCOMPARE(report.imagesSkipped, 1);
    QCOMPARE(report.imagesChanged, 0);
}

void ImageDownsampleFixupTest::rejectsInvalidTargetDpi()
{
    pdf::PDFDocument document = createDocumentWithImage(1200, true, false);
    for (const int targetDpi : { 0, 71, 1201 })
    {
        pdf::PDFImageDownsampleFixupSettings settings;
        settings.targetDpi = targetDpi;
        QVERIFY(!pdf::PDFImageDownsampleFixup::apply(&document, settings));
    }
}

void ImageDownsampleFixupTest::doesNotMutateSourceDocument()
{
    const pdf::PDFDocument source = createDocumentWithImage(1200, true, false);
    pdf::PDFDocument candidate = source;
    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&candidate,
                                                pdf::PDFImageDownsampleFixupSettings(),
                                                nullptr));
    QVERIFY(candidate != source);
}

void ImageDownsampleFixupTest::writesReopensAndPassesPostFixPreflight()
{
    const pdf::PDFDocument source = createDocumentWithImage(1200, true, false);
    pdf::PDFDocument candidate = source;
    pdf::PDFImageDownsampleFixupReport report;
    QVERIFY(pdf::PDFImageDownsampleFixup::apply(&candidate,
                                                pdf::PDFImageDownsampleFixupSettings(),
                                                &report));
    QCOMPARE(report.imagesChanged, 1);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString outputPath = directory.filePath(QStringLiteral("downsampled.pdf"));

    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult writeResult = writer.write(outputPath, &candidate, true);
    QVERIFY2(writeResult, qPrintable(writeResult.getErrorMessage()));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument reopened = reader.readFromFile(outputPath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&reopened);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Post-fix image resolution") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("image-resolution") },
                { QStringLiteral("min_dpi"), 300 },
                { QStringLiteral("severity"), QStringLiteral("error") }
            }
        } },
        { QStringLiteral("fixups"), QJsonArray{} }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.inspectionComplete);
    QVERIFY(std::none_of(result.errors.cbegin(), result.errors.cend(), [](const pdf::PreflightFinding& finding)
    {
        return finding.checkId == QStringLiteral("image-resolution");
    }));
}

QTEST_MAIN(ImageDownsampleFixupTest)
#include "tst_imagedownsamplefixuptest.moc"
