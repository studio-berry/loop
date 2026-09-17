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

#include "pdfobjectselector.h"
#include "pdfactionlist.h"
#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdfimageoptimizer.h"
#include "pdfobject.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QtTest>

namespace
{

QImage makeImage(int pixels, bool noisy)
{
    QImage image(pixels, pixels, QImage::Format_RGB32);
    for (int y = 0; y < pixels; ++y)
    {
        for (int x = 0; x < pixels; ++x)
        {
            if (!noisy)
            {
                image.setPixel(x, y, qRgb(30, 120, 40));
                continue;
            }
            const int r = (x * 17 + y * 13) % 256;
            const int g = (x * 7 + y * 29) % 256;
            const int b = (x * 31 + y * 3) % 256;
            image.setPixel(x, y, qRgb(r, g, b));
        }
    }
    return image;
}

pdf::PDFObjectReference addImageObject(pdf::PDFDocumentBuilder& builder, int pixels)
{
    const QImage image = makeImage(pixels, true);
    pdf::PDFImage::ImageEncodeOptions options;
    options.compression = pdf::PDFImage::ImageCompression::Flate;
    options.colorMode = pdf::PDFImage::ImageColorMode::Preserve;
    options.alphaHandling = pdf::PDFImage::AlphaHandling::FlattenToWhite;
    pdf::PDFStream imageStream = pdf::PDFImage::createStreamFromImage(image, options);
    return builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(imageStream))));
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

pdf::PDFDocument createSinglePageImageDocument(int pixels, pdf::PDFObjectReference* imageReference = nullptr)
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));
    const pdf::PDFObjectReference imageRef = addImageObject(builder, pixels);
    if (imageReference)
    {
        *imageReference = imageRef;
    }

    const QByteArray pageContent("q 144 0 0 144 0 0 cm /Im1 Do Q");
    pdf::PDFDictionary xObject;
    xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageRef));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
    setPageContent(builder,
                   pageReference,
                   pageContent,
                   pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    return builder.build();
}

pdf::PDFDocument createDocumentWithImage(int pixels, int pageCount = 1)
{
    if (pageCount <= 1)
    {
        return createSinglePageImageDocument(pixels);
    }

    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    pdf::PDFObjectReference sharedImageReference;
    for (int page = 0; page < pageCount; ++page)
    {
        const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));
        if (page == 0)
        {
            sharedImageReference = addImageObject(builder, pixels);
        }

        const QByteArray pageContent("q 144 0 0 144 0 0 cm /Im1 Do Q");
        pdf::PDFDictionary xObject;
        xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(sharedImageReference));
        pdf::PDFDictionary resources;
        resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                           pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
        setPageContent(builder,
                       pageReference,
                       pageContent,
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    }
    return builder.build();
}

pdf::PDFDocument createDocumentWithDistinctPageImages(int pixels, int pageCount)
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    for (int page = 0; page < pageCount; ++page)
    {
        const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));
        const pdf::PDFObjectReference imageReference = addImageObject(builder, pixels);
        const QByteArray pageContent("q 144 0 0 144 0 0 cm /Im1 Do Q");
        pdf::PDFDictionary xObject;
        xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));
        pdf::PDFDictionary resources;
        resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                           pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
        setPageContent(builder,
                       pageReference,
                       pageContent,
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    }
    return builder.build();
}

pdf::PDFDocument createLayeredImageDocument(const QString& layerName, int pixels)
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));

    pdf::PDFObjectFactory ocgFactory;
    ocgFactory.beginDictionary();
    ocgFactory.beginDictionaryItem("Type");
    ocgFactory << pdf::WrapName("OCG");
    ocgFactory.endDictionaryItem();
    ocgFactory.beginDictionaryItem("Name");
    ocgFactory << layerName;
    ocgFactory.endDictionaryItem();
    ocgFactory.endDictionary();
    const pdf::PDFObjectReference ocgReference = builder.addObject(ocgFactory.takeObject());

    pdf::PDFObjectFactory configFactory;
    configFactory.beginDictionary();
    configFactory.beginDictionaryItem("Order");
    configFactory.beginArray();
    configFactory << ocgReference;
    configFactory.endArray();
    configFactory.endDictionaryItem();
    configFactory.beginDictionaryItem("ON");
    configFactory.beginArray();
    configFactory << ocgReference;
    configFactory.endArray();
    configFactory.endDictionaryItem();
    configFactory.endDictionary();
    const pdf::PDFObjectReference configReference = builder.addObject(configFactory.takeObject());

    pdf::PDFObjectFactory ocPropertiesFactory;
    ocPropertiesFactory.beginDictionary();
    ocPropertiesFactory.beginDictionaryItem("OCGs");
    ocPropertiesFactory.beginArray();
    ocPropertiesFactory << ocgReference;
    ocPropertiesFactory.endArray();
    ocPropertiesFactory.endDictionaryItem();
    ocPropertiesFactory.beginDictionaryItem("D");
    ocPropertiesFactory << configReference;
    ocPropertiesFactory.endDictionaryItem();
    ocPropertiesFactory.endDictionary();
    builder.setCatalogOptionalContentProperties(builder.addObject(ocPropertiesFactory.takeObject()));

    const pdf::PDFObjectReference imageReference = addImageObject(builder, pixels);
    const QByteArray pageContent("/OC /Layer1 BDC q 144 0 0 144 0 0 cm /Im1 Do Q EMC");

    pdf::PDFDictionary properties;
    properties.addEntry(pdf::PDFInplaceOrMemoryString("Layer1"), pdf::PDFObject::createReference(ocgReference));
    pdf::PDFDictionary xObject;
    xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("Properties"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(properties))));
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
    setPageContent(builder,
                   pageReference,
                   pageContent,
                   pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    return builder.build();
}

pdf::PDFDocument createTextDocument(const QByteArray& fontName)
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));
    const pdf::PDFObjectReference glyphReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(), QByteArray("0 0 m 100 0 l 100 100 l 0 100 l h f"))));
    const pdf::PDFObjectReference fontReference = builder.addObject(pdf::PDFObject());

    pdf::PDFObjectFactory fontFactory;
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("Type");
    fontFactory << pdf::WrapName("Font");
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Subtype");
    fontFactory << pdf::WrapName("Type3");
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("FontMatrix");
    fontFactory.beginArray();
    fontFactory << 0.001 << 0.0 << 0.0 << 0.001 << 0.0 << 0.0;
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("FontBBox");
    fontFactory.beginArray();
    fontFactory << 0.0 << 0.0 << 1000.0 << 1000.0;
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("FirstChar");
    fontFactory << pdf::PDFInteger(0);
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("LastChar");
    fontFactory << pdf::PDFInteger(0);
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Widths");
    fontFactory.beginArray();
    fontFactory << 1000.0;
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("CharProcs");
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("A");
    fontFactory << glyphReference;
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Encoding");
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("Type");
    fontFactory << pdf::WrapName("Encoding");
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Differences");
    fontFactory.beginArray();
    fontFactory << pdf::PDFInteger(0) << pdf::PDFObject::createName(QByteArray("A"));
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    fontFactory.endDictionaryItem();

    const QByteArray toUnicodeData =
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<00> <00>\n"
        "endcodespacerange\n"
        "1 beginbfchar\n"
        "<00> <0041>\n"
        "endbfchar\n"
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end\n"
        "end\n";
    const pdf::PDFObjectReference toUnicodeReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(), QByteArray(toUnicodeData))));
    fontFactory.beginDictionaryItem("ToUnicode");
    fontFactory << toUnicodeReference;
    fontFactory.endDictionaryItem();

    fontFactory.beginDictionaryItem("FontDescriptor");
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("Type");
    fontFactory << pdf::WrapName("FontDescriptor");
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("FontName");
    fontFactory << pdf::WrapName(fontName);
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    builder.setObject(fontReference, fontFactory.takeObject());

    pdf::PDFDictionary pageFontResources;
    pageFontResources.addEntry(pdf::PDFInplaceOrMemoryString("F1"), pdf::PDFObject::createReference(fontReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("Font"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageFontResources))));
    setPageContent(builder,
                   pageReference,
                   QByteArray("BT /F1 12 Tf 1 0 0 1 50 50 Tm <00> Tj ET"),
                   pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    return builder.build();
}

pdf::PDFDocument createVectorAndTextDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));
    const pdf::PDFObjectReference glyphReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(), QByteArray("0 0 m 10 0 l 10 10 l 0 10 l h f"))));
    const pdf::PDFObjectReference fontReference = builder.addObject(pdf::PDFObject());

    pdf::PDFObjectFactory fontFactory;
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("Type");
    fontFactory << pdf::WrapName("Font");
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Subtype");
    fontFactory << pdf::WrapName("Type3");
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("FontMatrix");
    fontFactory.beginArray();
    fontFactory << 0.001 << 0.0 << 0.0 << 0.001 << 0.0 << 0.0;
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("FontBBox");
    fontFactory.beginArray();
    fontFactory << 0.0 << 0.0 << 1000.0 << 1000.0;
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("FirstChar");
    fontFactory << pdf::PDFInteger(0);
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("LastChar");
    fontFactory << pdf::PDFInteger(0);
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Widths");
    fontFactory.beginArray();
    fontFactory << 1000.0;
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("CharProcs");
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("A");
    fontFactory << glyphReference;
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Encoding");
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("Type");
    fontFactory << pdf::WrapName("Encoding");
    fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Differences");
    fontFactory.beginArray();
    fontFactory << pdf::PDFInteger(0) << pdf::PDFObject::createName(QByteArray("A"));
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    builder.setObject(fontReference, fontFactory.takeObject());

    pdf::PDFDictionary pageFontResources;
    pageFontResources.addEntry(pdf::PDFInplaceOrMemoryString("F1"), pdf::PDFObject::createReference(fontReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("Font"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageFontResources))));
    setPageContent(builder,
                   pageReference,
                   QByteArray("1 0 0 RG 0 0 m 200 0 l S BT /F1 12 Tf 1 0 0 1 50 50 Tm <00> Tj ET"),
                   pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    return builder.build();
}

pdf::PDFObjectReference addTintFunction(pdf::PDFDocumentBuilder& builder)
{
    pdf::PDFObjectFactory tintFactory;
    tintFactory.beginDictionary();
    tintFactory.beginDictionaryItem("FunctionType");
    tintFactory << pdf::PDFInteger(2);
    tintFactory.endDictionaryItem();
    tintFactory.beginDictionaryItem("Domain");
    tintFactory.beginArray();
    tintFactory << 0.0 << 1.0;
    tintFactory.endArray();
    tintFactory.endDictionaryItem();
    tintFactory.beginDictionaryItem("C0");
    tintFactory.beginArray();
    tintFactory << 0.0 << 0.0 << 0.0 << 0.0;
    tintFactory.endArray();
    tintFactory.endDictionaryItem();
    tintFactory.beginDictionaryItem("C1");
    tintFactory.beginArray();
    tintFactory << 0.0 << 1.0 << 0.0 << 0.0;
    tintFactory.endArray();
    tintFactory.endDictionaryItem();
    tintFactory.beginDictionaryItem("N");
    tintFactory << pdf::PDFInteger(1);
    tintFactory.endDictionaryItem();
    tintFactory.endDictionary();
    return builder.addObject(tintFactory.takeObject());
}

pdf::PDFDocument createSpotColorPathDocument(const QByteArray& spotName)
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));
    const pdf::PDFObjectReference tintReference = addTintFunction(builder);

    pdf::PDFObjectFactory separationFactory;
    separationFactory.beginArray();
    separationFactory << pdf::WrapName("Separation");
    separationFactory << pdf::WrapName(spotName);
    separationFactory << pdf::WrapName("DeviceCMYK");
    separationFactory << tintReference;
    separationFactory.endArray();
    const pdf::PDFObjectReference separationReference = builder.addObject(separationFactory.takeObject());

    pdf::PDFDictionary colorSpaces;
    colorSpaces.addEntry(pdf::PDFInplaceOrMemoryString("CS0"), pdf::PDFObject::createReference(separationReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(colorSpaces))));
    setPageContent(builder,
                   pageReference,
                   QByteArray("/CS0 cs 1 scn 20 20 120 120 re f"),
                   pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    return builder.build();
}

pdf::PDFDocument createDeviceNColorPathDocument(const QByteArray& colorantName)
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));
    const pdf::PDFObjectReference tintReference = addTintFunction(builder);

    pdf::PDFObjectFactory colorantsFactory;
    colorantsFactory.beginArray();
    colorantsFactory << pdf::WrapName(colorantName);
    colorantsFactory.endArray();
    const pdf::PDFObjectReference colorantsReference = builder.addObject(colorantsFactory.takeObject());

    pdf::PDFObjectFactory deviceNFactory;
    deviceNFactory.beginArray();
    deviceNFactory << pdf::WrapName("DeviceN");
    deviceNFactory << colorantsReference;
    deviceNFactory << pdf::WrapName("DeviceCMYK");
    deviceNFactory << tintReference;
    deviceNFactory.endArray();
    const pdf::PDFObjectReference deviceNReference = builder.addObject(deviceNFactory.takeObject());

    pdf::PDFDictionary colorSpaces;
    colorSpaces.addEntry(pdf::PDFInplaceOrMemoryString("CS0"), pdf::PDFObject::createReference(deviceNReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(colorSpaces))));
    setPageContent(builder,
                   pageReference,
                   QByteArray("/CS0 cs 1 scn 20 20 120 120 re f"),
                   pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    return builder.build();
}

QString revisionDigestForDocument(const pdf::PDFDocument& document)
{
    const pdf::PDFRevisionIdentity revision = pdf::revisionIdentityForDocument(document);
    return QString::fromLatin1(QCryptographicHash::hash(revision.toString().toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool resolveSelector(const pdf::PDFDocument& document,
                     const QJsonObject& selectorJson,
                     pdf::PDFObjectSelectionResult* result,
                     const pdf::PDFRevisionIdentity& revision = pdf::PDFRevisionIdentity())
{
    pdf::PDFObjectSelector selector;
    if (!pdf::PDFObjectSelector::fromJson(selectorJson, &selector))
    {
        return false;
    }
    const pdf::PDFRevisionIdentity activeRevision = revision.isValid() ? revision : pdf::revisionIdentityForDocument(document);
    return static_cast<bool>(pdf::PDFObjectSelector::resolve(selector, document, activeRevision, result));
}

QJsonObject selectorJson(const QJsonObject& predicate, const QJsonObject& extra = QJsonObject())
{
    QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("predicate"), predicate }
    };
    for (auto it = extra.begin(); it != extra.end(); ++it)
    {
        json.insert(it.key(), it.value());
    }
    return json;
}

}   // namespace

class ObjectSelectorTest : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsSelectorJson();
    void pagesPredicateLimitsCandidates();
    void objectClassAndColorSpacePredicates();
    void layerFontSpotColorAndDpiPredicates();
    void spotColorMatchesNamedSeparationOnly();
    void isVectorObjectRefAndRegionPredicates();
    void compositionAndNamedSet();
    void notCompositionExcludesMatches();
    void unknownNamedSetFailsParse();
    void emptySelectionIsVisibleOutcome();
    void staleRevisionFailsClosed();
    void adversarialUnknownPredicateFailsParse();
    void adversarialMalformedSelectorInputsFailParse();
    void digestIsDeterministicAcrossRepeatedResolution();
    void actionListV2RoundTripsWithSelect();
    void executorPreviewNeverMutatesOutsideSelection();
};

void ObjectSelectorTest::roundTripsSelectorJson()
{
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } }
    };
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(json, &selector));
    QCOMPARE(QJsonDocument(selector.toJson()).toJson(QJsonDocument::Compact),
             QJsonDocument(json).toJson(QJsonDocument::Compact));
}

void ObjectSelectorTest::pagesPredicateLimitsCandidates()
{
    const pdf::PDFDocument document = createDocumentWithImage(600, 3);
    const std::vector<pdf::PDFImageOptimizer::ImageInfo> imageInfos = pdf::PDFImageOptimizer::collectImageInfos(&document);
    QVERIFY(!imageInfos.empty());
    pdf::PDFObjectSelectionResult result;
    QVERIFY(resolveSelector(document,
                            selectorJson(QJsonObject{ { QStringLiteral("pages"), QStringLiteral("2") } }),
                            &result));
    QVERIFY(result.ok);
    QVERIFY(!result.candidates.isEmpty());
    for (const pdf::PDFObjectSelectorCandidate& candidate : result.candidates)
    {
        QCOMPARE(candidate.pageIndex, 1);
    }
}

void ObjectSelectorTest::objectClassAndColorSpacePredicates()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    pdf::PDFObjectSelectionResult result;
    QVERIFY(resolveSelector(document,
                            selectorJson(QJsonObject{
                                { QStringLiteral("and"), QJsonArray{
                                                             QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } },
                                                             QJsonObject{ { QStringLiteral("colorSpace"), QStringLiteral("DeviceRGB") } } } } }),
                            &result));
    QVERIFY(result.ok);
    QVERIFY(!result.empty);
    QCOMPARE(result.candidates.front().objectClass, QStringLiteral("image"));
    QCOMPARE(result.candidates.front().colorSpaceName, QStringLiteral("DeviceRGB"));
}

void ObjectSelectorTest::layerFontSpotColorAndDpiPredicates()
{
    const pdf::PDFDocument layeredDocument = createLayeredImageDocument(QStringLiteral("Artwork"), 600);
    pdf::PDFObjectSelectionResult layerResult;
    QVERIFY(resolveSelector(layeredDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("layer"), QStringLiteral("Artwork") } }),
                            &layerResult));
    QVERIFY(layerResult.ok);
    QVERIFY(!layerResult.empty);
    QCOMPARE(layerResult.candidates.front().layerName, QStringLiteral("Artwork"));

    const pdf::PDFDocument textDocument = createTextDocument(QByteArray("LoopSelectorFont"));
    pdf::PDFObjectSelectionResult fontResult;
    QVERIFY(resolveSelector(textDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("font"), QStringLiteral("LoopSelector") } }),
                            &fontResult));
    QVERIFY(fontResult.ok);
    QVERIFY(!fontResult.empty);
    QVERIFY(fontResult.candidates.front().fontName.contains(QStringLiteral("LoopSelector"), Qt::CaseInsensitive));

    const pdf::PDFDocument spotDocument = createSpotColorPathDocument(QByteArray("CutGreen"));
    pdf::PDFObjectSelectionResult spotResult;
    QVERIFY(resolveSelector(spotDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("spotColor"), QStringLiteral("CutGreen") } }),
                            &spotResult));
    QVERIFY(spotResult.ok);
    QVERIFY(!spotResult.empty);
    QCOMPARE(spotResult.candidates.front().colorSpaceName, QStringLiteral("Separation"));
    QCOMPARE(spotResult.candidates.front().spotColorName, QStringLiteral("CutGreen"));

    const pdf::PDFDocument highDpiDocument = createSinglePageImageDocument(600);
    pdf::PDFObjectSelectionResult minDpiResult;
    QVERIFY(resolveSelector(highDpiDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("minEffectiveDpi"), 250.0 } }),
                            &minDpiResult));
    QVERIFY(minDpiResult.ok);
    QVERIFY(!minDpiResult.empty);

    pdf::PDFObjectSelectionResult maxDpiResult;
    QVERIFY(resolveSelector(highDpiDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("maxEffectiveDpi"), 200.0 } }),
                            &maxDpiResult));
    QVERIFY(maxDpiResult.ok);
    QVERIFY(maxDpiResult.empty);
}

void ObjectSelectorTest::spotColorMatchesNamedSeparationOnly()
{
    const pdf::PDFDocument separationDocument = createSpotColorPathDocument(QByteArray("CutGreen"));
    pdf::PDFObjectSelectionResult matchingResult;
    QVERIFY(resolveSelector(separationDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("spotColor"), QStringLiteral("CutGreen") } }),
                            &matchingResult));
    QVERIFY(matchingResult.ok);
    QVERIFY(!matchingResult.empty);
    QCOMPARE(matchingResult.candidates.front().colorSpaceName, QStringLiteral("Separation"));
    QCOMPARE(matchingResult.candidates.front().spotColorName, QStringLiteral("CutGreen"));

    pdf::PDFObjectSelectionResult mismatchedNameResult;
    QVERIFY(resolveSelector(separationDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("spotColor"), QStringLiteral("DieLine") } }),
                            &mismatchedNameResult));
    QVERIFY(mismatchedNameResult.ok);
    QVERIFY(mismatchedNameResult.empty);

    const pdf::PDFDocument deviceNDocument = createDeviceNColorPathDocument(QByteArray("Spot"));
    pdf::PDFObjectSelectionResult deviceNResult;
    QVERIFY(resolveSelector(deviceNDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("spotColor"), QStringLiteral("Spot") } }),
                            &deviceNResult));
    QVERIFY(deviceNResult.ok);
    QVERIFY(deviceNResult.empty);
    QCOMPARE(deviceNResult.candidates.size(), 0);

    pdf::PDFObjectSelectionResult deviceNColorSpaceResult;
    QVERIFY(resolveSelector(deviceNDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("colorSpace"), QStringLiteral("DeviceN") } }),
                            &deviceNColorSpaceResult));
    QVERIFY(deviceNColorSpaceResult.ok);
    QVERIFY(!deviceNColorSpaceResult.empty);
    QCOMPARE(deviceNColorSpaceResult.candidates.front().colorSpaceName, QStringLiteral("DeviceN"));
    QVERIFY(deviceNColorSpaceResult.candidates.front().spotColorName.isEmpty());
}

void ObjectSelectorTest::isVectorObjectRefAndRegionPredicates()
{
    const pdf::PDFDocument mixedDocument = createVectorAndTextDocument();
    pdf::PDFObjectSelectionResult vectorResult;
    QVERIFY(resolveSelector(mixedDocument,
                            selectorJson(QJsonObject{ { QStringLiteral("isVector"), true } }),
                            &vectorResult));
    QVERIFY(vectorResult.ok);
    QVERIFY(!vectorResult.empty);
    for (const pdf::PDFObjectSelectorCandidate& candidate : vectorResult.candidates)
    {
        QVERIFY(candidate.isVector);
        QCOMPARE(candidate.objectClass, QStringLiteral("vector"));
    }

    pdf::PDFObjectReference imageReference;
    const pdf::PDFDocument imageDocument = createSinglePageImageDocument(600, &imageReference);
    pdf::PDFObjectSelectionResult objectRefResult;
    QVERIFY(resolveSelector(imageDocument,
                            selectorJson(QJsonObject{
                                { QStringLiteral("objectRef"), QJsonObject{
                                                                   { QStringLiteral("object"), static_cast<int>(imageReference.objectNumber) },
                                                                   { QStringLiteral("generation"), static_cast<int>(imageReference.generation) } } } }),
                            &objectRefResult));
    QVERIFY(objectRefResult.ok);
    QCOMPARE(objectRefResult.candidates.size(), 1);
    QCOMPARE(objectRefResult.candidates.front().objectReference, imageReference);

    pdf::PDFObjectSelectionResult includeRegionResult;
    QVERIFY(resolveSelector(imageDocument,
                            selectorJson(QJsonObject{
                                { QStringLiteral("region"), QJsonObject{
                                                                { QStringLiteral("rect_pt"), QJsonArray{ 0.0, 0.0, 72.0, 72.0 } },
                                                                { QStringLiteral("anchor"), QStringLiteral("media") },
                                                                { QStringLiteral("mode"), QStringLiteral("include") } } } }),
                            &includeRegionResult));
    QVERIFY(includeRegionResult.ok);
    QVERIFY(!includeRegionResult.empty);

    pdf::PDFObjectSelectionResult excludeRegionResult;
    QVERIFY(resolveSelector(imageDocument,
                            selectorJson(QJsonObject{
                                { QStringLiteral("region"), QJsonObject{
                                                                { QStringLiteral("rect_pt"), QJsonArray{ 200.0, 200.0, 20.0, 20.0 } },
                                                                { QStringLiteral("anchor"), QStringLiteral("media") },
                                                                { QStringLiteral("mode"), QStringLiteral("exclude") } } } }),
                            &excludeRegionResult));
    QVERIFY(excludeRegionResult.ok);
    QVERIFY(!excludeRegionResult.empty);
}

void ObjectSelectorTest::compositionAndNamedSet()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    pdf::PDFObjectSelectionResult result;
    QVERIFY(resolveSelector(document,
                            selectorJson(QJsonObject{
                                             { QStringLiteral("or"), QJsonArray{
                                                                         QJsonObject{ { QStringLiteral("set"), QStringLiteral("rgbOnly") } },
                                                                         QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("vector") } } } } },
                                         QJsonObject{ { QStringLiteral("sets"), QJsonObject{ { QStringLiteral("rgbOnly"), QJsonObject{ { QStringLiteral("colorSpace"), QStringLiteral("DeviceRGB") } } } } } }),
                            &result));
    QVERIFY(result.ok);
    QVERIFY(!result.empty);
}

void ObjectSelectorTest::notCompositionExcludesMatches()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    pdf::PDFObjectSelectionResult result;
    QVERIFY(resolveSelector(document,
                            selectorJson(QJsonObject{
                                { QStringLiteral("not"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } } }),
                            &result));
    QVERIFY(result.ok);
    QVERIFY(result.empty);
}

void ObjectSelectorTest::unknownNamedSetFailsParse()
{
    const QJsonObject json = selectorJson(QJsonObject{ { QStringLiteral("set"), QStringLiteral("missing") } });
    pdf::PDFObjectSelector selector;
    QStringList errors;
    QVERIFY(!pdf::PDFObjectSelector::fromJson(json, &selector, &errors));
    QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("unknown set")));
}

void ObjectSelectorTest::emptySelectionIsVisibleOutcome()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    pdf::PDFObjectSelectionResult result;
    QVERIFY(resolveSelector(document,
                            selectorJson(QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("annotation") } }),
                            &result));
    QVERIFY(result.ok);
    QVERIFY(result.empty);
    QCOMPARE(result.previewScope().value(QStringLiteral("count")).toInt(), 0);
}

void ObjectSelectorTest::staleRevisionFailsClosed()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    const pdf::PDFRevisionIdentity revision = pdf::revisionIdentityForDocument(document);
    const QString staleDigest = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("stale"), QCryptographicHash::Sha256).toHex());
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(selectorJson(QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } },
                                                          QJsonObject{ { QStringLiteral("revisionDigest"), staleDigest } }),
                                             &selector));
    pdf::PDFObjectSelectionResult result;
    QVERIFY(!pdf::PDFObjectSelector::resolve(selector, document, revision, &result));
    QVERIFY(result.staleRevision);
}

void ObjectSelectorTest::adversarialUnknownPredicateFailsParse()
{
    const QJsonObject json = selectorJson(QJsonObject{ { QStringLiteral("freeText"), QStringLiteral("drop tables") } });
    pdf::PDFObjectSelector selector;
    QStringList errors;
    QVERIFY(!pdf::PDFObjectSelector::fromJson(json, &selector, &errors));
    QVERIFY(!errors.isEmpty());
}

void ObjectSelectorTest::adversarialMalformedSelectorInputsFailParse()
{
    {
        pdf::PDFObjectSelector selector;
        QStringList errors;
        const QJsonObject json = selectorJson(QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } },
                                              QJsonObject{ { QStringLiteral("revisionDigest"), QStringLiteral("not-a-digest") } });
        QVERIFY(!pdf::PDFObjectSelector::fromJson(json, &selector, &errors));
        QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("revisionDigest")));
    }
    {
        pdf::PDFObjectSelector selector;
        QStringList errors;
        const QJsonObject json = selectorJson(QJsonObject{
            { QStringLiteral("and"), QJsonArray{} },
            { QStringLiteral("objectClass"), QStringLiteral("image") } });
        QVERIFY(!pdf::PDFObjectSelector::fromJson(json, &selector, &errors));
        QVERIFY(!errors.isEmpty());
    }
    {
        pdf::PDFObjectSelector selector;
        QStringList errors;
        const QJsonObject json = selectorJson(QJsonObject{ { QStringLiteral("and"), QJsonArray{} } });
        QVERIFY(!pdf::PDFObjectSelector::fromJson(json, &selector, &errors));
        QVERIFY(errors.join(QLatin1Char('\n')).contains(QStringLiteral("at least one predicate")));
    }
    {
        const pdf::PDFDocument document = createDocumentWithImage(600);
        pdf::PDFObjectSelector selector;
        QVERIFY(pdf::PDFObjectSelector::fromJson(selectorJson(QJsonObject{ { QStringLiteral("pages"), QStringLiteral("999") } }), &selector));
        pdf::PDFObjectSelectionResult result;
        QVERIFY(pdf::PDFObjectSelector::resolve(selector, document, pdf::revisionIdentityForDocument(document), &result));
        QVERIFY(result.ok);
        QVERIFY(result.empty);
    }
}

void ObjectSelectorTest::digestIsDeterministicAcrossRepeatedResolution()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(selectorJson(QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } }), &selector));
    pdf::PDFObjectSelectionResult first;
    pdf::PDFObjectSelectionResult second;
    const pdf::PDFRevisionIdentity revision = pdf::revisionIdentityForDocument(document);
    QVERIFY(pdf::PDFObjectSelector::resolve(selector, document, revision, &first));
    QVERIFY(pdf::PDFObjectSelector::resolve(selector, document, revision, &second));
    QCOMPARE(first.digest, second.digest);
    QVERIFY(!first.digest.isEmpty());
}

void ObjectSelectorTest::actionListV2RoundTripsWithSelect()
{
    const QJsonObject json{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
        { QStringLiteral("id"), QStringLiteral("select-recipe") },
        { QStringLiteral("name"), QStringLiteral("Select recipe") },
        { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                       { QStringLiteral("id"), QStringLiteral("downsample") },
                                       { QStringLiteral("operation"), QStringLiteral("downsample-images") },
                                       { QStringLiteral("params"), QJsonObject{ { QStringLiteral("target_dpi"), 150 } } },
                                       { QStringLiteral("select"), QJsonObject{
                                                                       { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
                                                                       { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } } } } } } }
    };
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(json, &actionList));
    QCOMPARE(actionList.schema, QStringLiteral("loop-action-list/2"));
    QCOMPARE(actionList.steps.size(), 1);
    QCOMPARE(actionList.steps.front().select.value(QStringLiteral("schema")).toString(), pdf::PDFObjectSelector::schemaVersion());
    QVERIFY(pdf::PDFActionListExecutor().validate(actionList, {}));
}

void ObjectSelectorTest::executorPreviewNeverMutatesOutsideSelection()
{
    const pdf::PDFDocument source = createDocumentWithImage(600);
    pdf::PDFActionList actionList;
    QVERIFY(pdf::PDFActionList::fromJson(QJsonObject{
                                             { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                             { QStringLiteral("id"), QStringLiteral("scope") },
                                             { QStringLiteral("name"), QStringLiteral("Scope") },
                                             { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                                                            { QStringLiteral("id"), QStringLiteral("downsample") },
                                                                            { QStringLiteral("operation"), QStringLiteral("downsample-images") },
                                                                            { QStringLiteral("params"), QJsonObject{ { QStringLiteral("target_dpi"), 72 } } },
                                                                            { QStringLiteral("select"), QJsonObject{
                                                                                                            { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
                                                                                                            { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("annotation") } } } } } } } } },
                                         &actionList));

    pdf::PDFActionListExecutionOptions options;
    options.revision = pdf::revisionIdentityForDocument(source);
    pdf::PDFActionListExecutionResult result;
    pdf::PDFDocument candidate;
    const pdf::PDFDocument sourceCopy = source;
    QVERIFY(pdf::PDFActionListExecutor().execute(actionList, source, options, &candidate, &result));
    QCOMPARE(result.status, QStringLiteral("succeeded"));
    QCOMPARE(result.steps.front().status, pdf::PDFActionListStepStatus::Succeeded);
    QVERIFY(result.steps.front().selectionScope.value(QStringLiteral("empty")).toBool());
    QCOMPARE(source.getCatalog()->getPageCount(), sourceCopy.getCatalog()->getPageCount());
}

QTEST_GUILESS_MAIN(ObjectSelectorTest)

#include "tst_objectselectortest.moc"
