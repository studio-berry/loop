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

#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "pdffont.h"
#include "pdfimagedecodeguard.h"
#include "pdfmeshqualitysettings.h"
#include "pdfoptionalcontent.h"
#include "pdfpagecontentprocessor.h"
#include "pdfprocessingbudget.h"

#include <QElapsedTimer>
#include <QtTest>

#include <algorithm>

namespace
{

pdf::PDFObject makeImageResources(const pdf::PDFObjectReference& imageReference)
{
    pdf::PDFDictionary xobjects;
    xobjects.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));

    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xobjects))));
    return pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources)));
}

void setPageContent(pdf::PDFDocumentBuilder& builder,
                    const pdf::PDFObjectReference& pageReference,
                    const QByteArray& content,
                    const pdf::PDFObject& resources)
{
    const pdf::PDFObject streamObject = pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(), QByteArray(content)));
    const pdf::PDFObjectReference contentStreamReference = builder.addObject(streamObject);

    pdf::PDFObjectFactory factory;
    factory.beginDictionary();
    factory.beginDictionaryItem("Contents");
    factory << contentStreamReference;
    factory.endDictionaryItem();
    factory.beginDictionaryItem("Resources");
    factory << resources;
    factory.endDictionaryItem();
    factory.endDictionary();
    builder.mergeTo(pageReference, factory.takeObject());
}

pdf::PDFObjectReference addRawImage(pdf::PDFDocumentBuilder& builder,
                                    pdf::PDFInteger width,
                                    pdf::PDFInteger height,
                                    pdf::PDFInteger bitsPerComponent,
                                    const QByteArray& colorSpaceName,
                                    const QByteArray& samples)
{
    pdf::PDFDictionary dictionary;
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Image"));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Width"), pdf::PDFObject::createInteger(width));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Height"), pdf::PDFObject::createInteger(height));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("BitsPerComponent"), pdf::PDFObject::createInteger(bitsPerComponent));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"), pdf::PDFObject::createName(colorSpaceName));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                        pdf::PDFObject::createInteger(samples.size()));

    return builder.addObject(pdf::PDFObject::createStream(
        std::make_shared<pdf::PDFStream>(std::move(dictionary), QByteArray(samples))));
}

QList<pdf::PDFRenderError> processPage(pdf::PDFDocument& document, pdf::PDFProcessingBudget* budget)
{
    const pdf::PDFPage* page = document.getCatalog()->getPage(0);
    pdf::PDFFontCache fontCache(pdf::DEFAULT_FONT_CACHE_LIMIT, pdf::DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    pdf::PDFOptionalContentActivity optionalContentActivity(&document, pdf::OCUsage::Export, nullptr);
    pdf::PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(&document);
    pdf::PDFCMSPointer cms = cmsManager.getCurrentCMS();
    pdf::PDFMeshQualitySettings meshQualitySettings;
    fontCache.setDocument(pdf::PDFModifiedDocument(&document, &optionalContentActivity));
    fontCache.setCacheShrinkEnabled(nullptr, false);

    pdf::PDFPageContentProcessor processor(page, &document, &fontCache, cms.get(), &optionalContentActivity,
                                           QTransform(), meshQualitySettings, budget);
    return processor.processContents();
}

bool hasMessageContaining(const QList<pdf::PDFRenderError>& errors, const QString& needle)
{
    return std::any_of(errors.cbegin(), errors.cend(), [&](const pdf::PDFRenderError& error)
                       { return error.message.contains(needle); });
}

}   // namespace

class ImageDecodeGuardTest : public QObject
{
    Q_OBJECT

private slots:
    void expectedMinSampleBytesMatchesGeometry();
    void truncatedMaxDimensionImageFailsFast();
    void lowRenderPixelBudgetRejectsLargeImage();
    void smallValidImageStillRenders();
    void excessiveCcittColumnsRejected();
};

void ImageDecodeGuardTest::expectedMinSampleBytesMatchesGeometry()
{
    QCOMPARE(pdf::PDFImageDecodeGuard::expectedMinImageSampleBytes(1, 1, 1, 1), quint64(1));
    QCOMPARE(pdf::PDFImageDecodeGuard::expectedMinImageSampleBytes(8, 1, 1, 1), quint64(1));
    QCOMPARE(pdf::PDFImageDecodeGuard::expectedMinImageSampleBytes(2, 2, 3, 8), quint64(12));

    QVERIFY_THROWS_EXCEPTION(pdf::PDFRendererException,
                             pdf::PDFImageDecodeGuard::requireSufficientSampleBytes(QByteArray(1, 0), 16384, 16384, 3, 8));
}

void ImageDecodeGuardTest::truncatedMaxDimensionImageFailsFast()
{
    // Declared near-max geometry with a one-byte stream must fail before a huge QImage.
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFObjectReference imageReference = addRawImage(builder, 16384, 16384, 8, "DeviceRGB", QByteArray(1, char(0)));
    setPageContent(builder, pageReference, "q 100 0 0 100 0 0 cm /Im1 Do Q", makeImageResources(imageReference));
    pdf::PDFDocument document = builder.build();

    QElapsedTimer timer;
    timer.start();
    const QList<pdf::PDFRenderError> errors = processPage(document, nullptr);
    QVERIFY2(timer.elapsed() < 5000, "truncated max-dimension image must fail without long work");
    QVERIFY2(hasMessageContaining(errors, QStringLiteral("too short")) ||
                 hasMessageContaining(errors, QStringLiteral("Can't decode")) ||
                 hasMessageContaining(errors, QStringLiteral("Invalid")),
             "truncated image must be reported as a render failure");
}

void ImageDecodeGuardTest::lowRenderPixelBudgetRejectsLargeImage()
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 200, 200));

    // 64x64 DeviceGray needs 4096 sample bytes — enough to pass sample-length checks.
    QByteArray samples(4096, char(0x7f));
    const pdf::PDFObjectReference imageReference = addRawImage(builder, 64, 64, 8, "DeviceGray", samples);
    setPageContent(builder, pageReference, "q 100 0 0 100 0 0 cm /Im1 Do Q", makeImageResources(imageReference));
    pdf::PDFDocument document = builder.build();

    pdf::PDFProcessingLimits limits;
    limits.maxRenderPixels = 100; // well below 64*64
    pdf::PDFProcessingBudget budget(limits);

    bool threwBudget = false;
    try
    {
        processPage(document, &budget);
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        threwBudget = true;
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::RenderPixels);
    }

    QVERIFY2(threwBudget, "low maxRenderPixels must reject before successful large paint");
}

void ImageDecodeGuardTest::smallValidImageStillRenders()
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 200, 200));
    QByteArray samples(4, char(0x20)); // 2x2 DeviceGray
    const pdf::PDFObjectReference imageReference = addRawImage(builder, 2, 2, 8, "DeviceGray", samples);
    setPageContent(builder, pageReference, "q 50 0 0 50 10 10 cm /Im1 Do Q", makeImageResources(imageReference));
    pdf::PDFDocument document = builder.build();

    pdf::PDFProcessingLimits limits;
    limits.maxRenderPixels = 1000;
    pdf::PDFProcessingBudget budget(limits);

    const QList<pdf::PDFRenderError> errors = processPage(document, &budget);
    QVERIFY2(!hasMessageContaining(errors, QStringLiteral("too short")), "valid small image must not trip sample-length guard");
    QVERIFY2(!hasMessageContaining(errors, QStringLiteral("Can't decode the image")), "valid small image must decode");
}

void ImageDecodeGuardTest::excessiveCcittColumnsRejected()
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 200, 200));

    pdf::PDFDictionary decodeParms;
    decodeParms.setEntry(pdf::PDFInplaceOrMemoryString("K"), pdf::PDFObject::createInteger(-1));
    decodeParms.setEntry(pdf::PDFInplaceOrMemoryString("Columns"), pdf::PDFObject::createInteger(100000));
    decodeParms.setEntry(pdf::PDFInplaceOrMemoryString("Rows"), pdf::PDFObject::createInteger(1));

    pdf::PDFDictionary dictionary;
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Image"));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Width"), pdf::PDFObject::createInteger(100000));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Height"), pdf::PDFObject::createInteger(1));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("BitsPerComponent"), pdf::PDFObject::createInteger(1));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"), pdf::PDFObject::createName("DeviceGray"));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_FILTER), pdf::PDFObject::createName("CCITTFaxDecode"));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_DECODE_PARMS),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(decodeParms)));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH), pdf::PDFObject::createInteger(1));

    const pdf::PDFObjectReference imageReference = builder.addObject(pdf::PDFObject::createStream(
        std::make_shared<pdf::PDFStream>(std::move(dictionary), QByteArray(1, char(0)))));
    setPageContent(builder, pageReference, "q 100 0 0 100 0 0 cm /Im1 Do Q", makeImageResources(imageReference));
    pdf::PDFDocument document = builder.build();

    QElapsedTimer timer;
    timer.start();
    const QList<pdf::PDFRenderError> errors = processPage(document, nullptr);
    QVERIFY2(timer.elapsed() < 5000, "excessive CCITT columns must fail closed quickly");
    QVERIFY2(hasMessageContaining(errors, QStringLiteral("Invalid size")) ||
                 hasMessageContaining(errors, QStringLiteral("Can't decode")) ||
                 hasMessageContaining(errors, QStringLiteral("Invalid")),
             "excessive CCITT columns must be refused");
}

QTEST_GUILESS_MAIN(ImageDecodeGuardTest)

#include "tst_imagedecodeguardtest.moc"
