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

pdf::PDFDocument createSinglePageImageDocument(int pixels)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));

    const QImage image = makeImage(pixels, true);
    pdf::PDFImage::ImageEncodeOptions options;
    options.compression = pdf::PDFImage::ImageCompression::Flate;
    options.colorMode = pdf::PDFImage::ImageColorMode::Preserve;
    options.alphaHandling = pdf::PDFImage::AlphaHandling::FlattenToWhite;
    pdf::PDFStream imageStream = pdf::PDFImage::createStreamFromImage(image, options);

    const pdf::PDFObjectReference imageReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(imageStream))));
    const QByteArray pageContent("q 144 0 0 144 0 0 cm /Im1 Do Q");
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(pageContent.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFStream(std::move(contentDictionary), QByteArray(pageContent)))));

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

pdf::PDFDocument createDocumentWithImage(int pixels, int pageCount = 1)
{
    if (pageCount <= 1)
    {
        return createSinglePageImageDocument(pixels);
    }

    pdf::PDFDocumentBuilder builder;
    pdf::PDFObjectReference sharedImageReference;
    for (int page = 0; page < pageCount; ++page)
    {
        const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));
        if (page == 0)
        {
            const QImage image = makeImage(pixels, true);
            pdf::PDFImage::ImageEncodeOptions options;
            options.compression = pdf::PDFImage::ImageCompression::Flate;
            options.colorMode = pdf::PDFImage::ImageColorMode::Preserve;
            options.alphaHandling = pdf::PDFImage::AlphaHandling::FlattenToWhite;
            pdf::PDFStream imageStream = pdf::PDFImage::createStreamFromImage(image, options);
            sharedImageReference = builder.addObject(
                pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(imageStream))));
        }

        const QByteArray pageContent("q 144 0 0 144 0 0 cm /Im1 Do Q");
        pdf::PDFDictionary contentDictionary;
        contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                                   pdf::PDFObject::createInteger(pageContent.size()));
        const pdf::PDFObjectReference contentReference = builder.addObject(
            pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
                pdf::PDFStream(std::move(contentDictionary), QByteArray(pageContent)))));

        pdf::PDFDictionary xObject;
        xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(sharedImageReference));
        pdf::PDFDictionary resources;
        resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                           pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
        pdf::PDFDictionary pageUpdate;
        pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                            pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
        pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
        builder.mergeTo(pageReference,
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    }
    return builder.build();
}

}   // namespace

class ObjectSelectorTest : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsSelectorJson();
    void pagesPredicateLimitsCandidates();
    void objectClassAndColorSpacePredicates();
    void compositionAndNamedSet();
    void emptySelectionIsVisibleOutcome();
    void staleRevisionFailsClosed();
    void adversarialUnknownPredicateFailsParse();
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
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("pages"), QStringLiteral("2") } } }
    };
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(json, &selector));
    pdf::PDFObjectSelectionResult result;
    const pdf::PDFRevisionIdentity revision = pdf::revisionIdentityForDocument(document);
    QVERIFY(pdf::PDFObjectSelector::resolve(selector, document, revision, &result));
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
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("predicate"), QJsonObject{
            { QStringLiteral("and"), QJsonArray{
                QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } },
                QJsonObject{ { QStringLiteral("colorSpace"), QStringLiteral("DeviceRGB") } }
            } }
        } }
    };
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(json, &selector));
    pdf::PDFObjectSelectionResult result;
    QVERIFY(pdf::PDFObjectSelector::resolve(selector, document, pdf::revisionIdentityForDocument(document), &result));
    QVERIFY(result.ok);
    QVERIFY(!result.empty);
    QCOMPARE(result.candidates.front().objectClass, QStringLiteral("image"));
    QCOMPARE(result.candidates.front().colorSpaceName, QStringLiteral("DeviceRGB"));
}

void ObjectSelectorTest::compositionAndNamedSet()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("sets"), QJsonObject{
            { QStringLiteral("rgbOnly"), QJsonObject{ { QStringLiteral("colorSpace"), QStringLiteral("DeviceRGB") } } }
        } },
        { QStringLiteral("predicate"), QJsonObject{
            { QStringLiteral("or"), QJsonArray{
                QJsonObject{ { QStringLiteral("set"), QStringLiteral("rgbOnly") } },
                QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("vector") } }
            } }
        } }
    };
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(json, &selector));
    pdf::PDFObjectSelectionResult result;
    QVERIFY(pdf::PDFObjectSelector::resolve(selector, document, pdf::revisionIdentityForDocument(document), &result));
    QVERIFY(result.ok);
    QVERIFY(!result.empty);
}

void ObjectSelectorTest::emptySelectionIsVisibleOutcome()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("annotation") } } }
    };
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(json, &selector));
    pdf::PDFObjectSelectionResult result;
    QVERIFY(pdf::PDFObjectSelector::resolve(selector, document, pdf::revisionIdentityForDocument(document), &result));
    QVERIFY(result.ok);
    QVERIFY(result.empty);
    QCOMPARE(result.previewScope().value(QStringLiteral("count")).toInt(), 0);
}

void ObjectSelectorTest::staleRevisionFailsClosed()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    const pdf::PDFRevisionIdentity revision = pdf::revisionIdentityForDocument(document);
    const QString staleDigest = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("stale"), QCryptographicHash::Sha256).toHex());
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("revisionDigest"), staleDigest },
        { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } }
    };
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(json, &selector));
    pdf::PDFObjectSelectionResult result;
    QVERIFY(!pdf::PDFObjectSelector::resolve(selector, document, revision, &result));
    QVERIFY(result.staleRevision);
}

void ObjectSelectorTest::adversarialUnknownPredicateFailsParse()
{
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("freeText"), QStringLiteral("drop tables") } } }
    };
    pdf::PDFObjectSelector selector;
    QStringList errors;
    QVERIFY(!pdf::PDFObjectSelector::fromJson(json, &selector, &errors));
    QVERIFY(!errors.isEmpty());
}

void ObjectSelectorTest::digestIsDeterministicAcrossRepeatedResolution()
{
    const pdf::PDFDocument document = createDocumentWithImage(600);
    const QJsonObject json{
        { QStringLiteral("schema"), pdf::PDFObjectSelector::schemaVersion() },
        { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } }
    };
    pdf::PDFObjectSelector selector;
    QVERIFY(pdf::PDFObjectSelector::fromJson(json, &selector));
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
                { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("image") } } }
            } }
        } } }
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
                { QStringLiteral("predicate"), QJsonObject{ { QStringLiteral("objectClass"), QStringLiteral("annotation") } } }
            } }
        } } }
    }, &actionList));

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
