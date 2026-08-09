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

#include <QtTest>
#include <QDataStream>

#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocument.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfexception.h"
#include "pdffont.h"
#include "pdfmeshqualitysettings.h"
#include "pdfobject.h"
#include "pdfoptionalcontent.h"
#include "pdfpagecontentprocessor.h"

#include <vector>

namespace
{

/// Builds a resource dictionary with an XObject subdictionary mapping the
/// given names to the given form references.
pdf::PDFObject makeResourcesDictionary(const std::vector<std::pair<QByteArray, pdf::PDFObjectReference>>& xobjects)
{
    pdf::PDFDictionary xobjectsDictionary;
    for (const auto& [name, reference] : xobjects)
    {
        xobjectsDictionary.addEntry(pdf::PDFInplaceOrMemoryString(name), pdf::PDFObject::createReference(reference));
    }

    pdf::PDFDictionary resourcesDictionary;
    resourcesDictionary.addEntry(pdf::PDFInplaceOrMemoryString("XObject"), pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xobjectsDictionary))));
    return pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resourcesDictionary)));
}

/// Creates a form XObject stream object with the given content and optional resources.
pdf::PDFObject makeFormStreamObject(const QByteArray& content, const pdf::PDFObject& resources)
{
    pdf::PDFObjectFactory factory;
    factory.beginDictionary();
    factory.beginDictionaryItem("Type"); factory << pdf::WrapName("XObject"); factory.endDictionaryItem();
    factory.beginDictionaryItem("Subtype"); factory << pdf::WrapName("Form"); factory.endDictionaryItem();
    factory.beginDictionaryItem("FormType"); factory << pdf::PDFInteger(1); factory.endDictionaryItem();
    factory.beginDictionaryItem("BBox"); factory << QRectF(0, 0, 100, 100); factory.endDictionaryItem();
    if (resources.isDictionary())
    {
        factory.beginDictionaryItem("Resources"); factory << resources; factory.endDictionaryItem();
    }
    factory.endDictionary();

    pdf::PDFObject dictionary = factory.takeObject();
    return pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(*dictionary.getDictionary()), QByteArray(content)));
}

/// Fills a previously allocated placeholder form object with its final value.
void setFormObject(pdf::PDFDocumentBuilder& builder, pdf::PDFObjectReference reference, const QByteArray& content, const pdf::PDFObject& resources)
{
    builder.setObject(reference, makeFormStreamObject(content, resources));
}

/// Sets the page content stream and resources of the given page.
void setPageContent(pdf::PDFDocumentBuilder& builder, const pdf::PDFObjectReference& pageReference, const QByteArray& content, const pdf::PDFObject& resources)
{
    const pdf::PDFObject streamObject = pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(), QByteArray(content)));
    const pdf::PDFObjectReference contentStreamReference = builder.addObject(streamObject);

    pdf::PDFObjectFactory factory;
    factory.beginDictionary();
    factory.beginDictionaryItem("Contents"); factory << contentStreamReference; factory.endDictionaryItem();
    factory.beginDictionaryItem("Resources"); factory << resources; factory.endDictionaryItem();
    factory.endDictionary();
    builder.mergeTo(pageReference, factory.takeObject());
}

/// Processes the content streams of the first page of the document and returns
/// the list of render errors.
QList<pdf::PDFRenderError> processPage(pdf::PDFDocument& document)
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

    pdf::PDFPageContentProcessor processor(page, &document, &fontCache, cms.get(), &optionalContentActivity, QTransform(), meshQualitySettings);
    return processor.processContents();
}

}   // namespace

class ContentProcessorLimitsTest : public QObject
{
    Q_OBJECT

private slots:
    void test_selfReferencingFormXObject_isRejected();
    void test_mutuallyRecursiveForms_areRejected();
    void test_deeplyNestedForms_areBounded();
    void test_recursiveType3Font_isRejected();
    void test_objectStreamWithHugeObjectCount_isRejected();
};

void ContentProcessorLimitsTest::test_selfReferencingFormXObject_isRejected()
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));

    // The form paints only itself, directly.
    const pdf::PDFObjectReference form1 = builder.addObject(pdf::PDFObject());
    setFormObject(builder, form1, "/N Do", makeResourcesDictionary({ { QByteArray("N"), form1 } }));

    setPageContent(builder, pageReference, "/N Do", makeResourcesDictionary({ { QByteArray("N"), form1 } }));

    pdf::PDFDocument document = builder.build();
    const QList<pdf::PDFRenderError> errors = processPage(document);

    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().message.contains(QStringLiteral("Recursive form XObject")));
}

void ContentProcessorLimitsTest::test_mutuallyRecursiveForms_areRejected()
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));

    const pdf::PDFObjectReference formA = builder.addObject(pdf::PDFObject());
    const pdf::PDFObjectReference formB = builder.addObject(pdf::PDFObject());

    setFormObject(builder, formA, "/B Do", makeResourcesDictionary({ { QByteArray("B"), formB } }));
    setFormObject(builder, formB, "/A Do", makeResourcesDictionary({ { QByteArray("A"), formA } }));

    setPageContent(builder, pageReference, "/A Do", makeResourcesDictionary({ { QByteArray("A"), formA } }));

    pdf::PDFDocument document = builder.build();
    const QList<pdf::PDFRenderError> errors = processPage(document);

    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().message.contains(QStringLiteral("Recursive form XObject")));
}

void ContentProcessorLimitsTest::test_deeplyNestedForms_areBounded()
{
    constexpr int deepChainLength = 40;
    {
        pdf::PDFDocumentBuilder builder;
        builder.createDocument();
        const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));

        std::vector<pdf::PDFObjectReference> forms;
        forms.reserve(deepChainLength);
        for (int i = 0; i < deepChainLength; ++i)
        {
            forms.push_back(builder.addObject(pdf::PDFObject()));
        }
        for (int i = 0; i < deepChainLength - 1; ++i)
        {
            setFormObject(builder, forms[i], "/N Do", makeResourcesDictionary({ { QByteArray("N"), forms[i + 1] } }));
        }
        setFormObject(builder, forms.back(), QByteArray(), pdf::PDFObject());

        setPageContent(builder, pageReference, "/N Do", makeResourcesDictionary({ { QByteArray("N"), forms.front() } }));

        pdf::PDFDocument document = builder.build();
        const QList<pdf::PDFRenderError> errors = processPage(document);

        // The content stream depth cap is hit exactly once (when processing the
        // 33rd level). No error is reported on the way back up.
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.constFirst().message.contains(QStringLiteral("Maximum content stream nesting depth")));
    }

    constexpr int legalChainLength = 16;
    {
        pdf::PDFDocumentBuilder builder;
        builder.createDocument();
        const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));

        std::vector<pdf::PDFObjectReference> forms;
        forms.reserve(legalChainLength);
        for (int i = 0; i < legalChainLength; ++i)
        {
            forms.push_back(builder.addObject(pdf::PDFObject()));
        }
        for (int i = 0; i < legalChainLength - 1; ++i)
        {
            setFormObject(builder, forms[i], "/N Do", makeResourcesDictionary({ { QByteArray("N"), forms[i + 1] } }));
        }
        setFormObject(builder, forms.back(), QByteArray(), pdf::PDFObject());

        setPageContent(builder, pageReference, "/N Do", makeResourcesDictionary({ { QByteArray("N"), forms.front() } }));

        pdf::PDFDocument document = builder.build();
        const QList<pdf::PDFRenderError> errors = processPage(document);

        // A moderately deep acyclic nesting is a legal document.
        QVERIFY(errors.isEmpty());
    }
}

void ContentProcessorLimitsTest::test_recursiveType3Font_isRejected()
{
    pdf::PDFDocumentBuilder builder;
    builder.createDocument();
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 400, 400));

    // A single glyph whose content paints character 0 with the very same font,
    // creating an unbounded recursion. It is bounded by the content stream
    // nesting depth, not by any form detection (no forms are involved here).
    const pdf::PDFObjectReference glyphReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(pdf::PDFDictionary(), QByteArray("/F 12 Tf 1 0 0 1 0 0 Tm <00> Tj"))));

    const pdf::PDFObjectReference fontReference = builder.addObject(pdf::PDFObject());

    pdf::PDFObjectFactory fontFactory;
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("Type"); fontFactory << pdf::WrapName("Font"); fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Subtype"); fontFactory << pdf::WrapName("Type3"); fontFactory.endDictionaryItem();

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

    fontFactory.beginDictionaryItem("FirstChar"); fontFactory << pdf::PDFInteger(0); fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("LastChar"); fontFactory << pdf::PDFInteger(0); fontFactory.endDictionaryItem();

    fontFactory.beginDictionaryItem("Widths");
    fontFactory.beginArray();
    fontFactory << 1000.0;
    fontFactory.endArray();
    fontFactory.endDictionaryItem();

    fontFactory.beginDictionaryItem("CharProcs");
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("A"); fontFactory << glyphReference; fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    fontFactory.endDictionaryItem();

    fontFactory.beginDictionaryItem("Encoding");
    fontFactory.beginDictionary();
    fontFactory.beginDictionaryItem("Type"); fontFactory << pdf::WrapName("Encoding"); fontFactory.endDictionaryItem();
    fontFactory.beginDictionaryItem("Differences");
    fontFactory.beginArray();
    fontFactory << pdf::PDFInteger(0) << pdf::PDFObject::createName(QByteArray("A"));
    fontFactory.endArray();
    fontFactory.endDictionaryItem();
    fontFactory.endDictionary();
    fontFactory.endDictionaryItem();

    // The font paints itself, so it must reference itself from its resources.
    pdf::PDFDictionary fontResources;
    fontResources.addEntry(pdf::PDFInplaceOrMemoryString("F"), pdf::PDFObject::createReference(fontReference));
    fontFactory.beginDictionaryItem("Resources"); fontFactory << pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(fontResources))); fontFactory.endDictionaryItem();

    fontFactory.endDictionary();
    builder.setObject(fontReference, fontFactory.takeObject());

    pdf::PDFDictionary pageFontResources;
    pageFontResources.addEntry(pdf::PDFInplaceOrMemoryString("F"), pdf::PDFObject::createReference(fontReference));
    pdf::PDFDictionary pageResources;
    pageResources.addEntry(pdf::PDFInplaceOrMemoryString("Font"), pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageFontResources))));

    setPageContent(builder, pageReference, "BT /F 12 Tf 1 0 0 1 0 0 Tm <00> Tj ET",
                   pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageResources))));

    pdf::PDFDocument document = builder.build();
    const QList<pdf::PDFRenderError> errors = processPage(document);

    for (const pdf::PDFRenderError& error : errors)
    {
        qWarning() << "TYPE3ERROR:" << error.message;
    }
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().message.contains(QStringLiteral("Maximum content stream nesting depth")));
}

void ContentProcessorLimitsTest::test_objectStreamWithHugeObjectCount_isRejected()
{
    QByteArray buffer("%PDF-1.7\n");

    auto appendObject = [&buffer](const QByteArray& text) -> qint64
    {
        const qint64 offset = buffer.size();
        buffer.append(text);
        return offset;
    };

    const qint64 catalogOffset = appendObject("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    const qint64 pageTreeOffset = appendObject("2 0 obj\n<< /Type /Pages /Count 0 /Kids [] >>\nendobj\n");

    // Object stream declaring an absurd object count. Without a bound this
    // triggers a multi-gigabyte vector allocation and a signed overflow guard.
    const qint64 objectStreamOffset = appendObject("3 0 obj\n<< /Type /ObjStm /N 1000000000 /First 1 /Length 0 >>\nstream\nendstream\nendobj\n");

    const qint64 xrefStreamOffset = buffer.size();

    // Cross reference stream with W = [1 2 2], 6 entries (objects 0..5). Object
    // 4 is stored as a compressed entry in object stream 3.
    QByteArray xrefStreamData;
    QDataStream xrefDataStream(&xrefStreamData, QIODevice::WriteOnly);
    xrefDataStream.setByteOrder(QDataStream::BigEndian);

    auto appendXrefEntry = [&xrefDataStream](quint8 type, quint32 value, quint32 second)
    {
        xrefDataStream << type;
        xrefDataStream << quint16(value);
        xrefDataStream << quint16(second);
    };

    appendXrefEntry(0, 0, 0);
    appendXrefEntry(1, quint32(catalogOffset), 0);
    appendXrefEntry(1, quint32(pageTreeOffset), 0);
    appendXrefEntry(1, quint32(objectStreamOffset), 0);
    appendXrefEntry(2, 3, 0);
    appendXrefEntry(1, quint32(xrefStreamOffset), 0);

    appendObject(QString("5 0 obj\n<< /Type /XRef /Size 6 /Root 1 0 R /W [1 2 2] /Index [0 6] /Length %1 >>\nstream\n").arg(xrefStreamData.size()).toLatin1());
    buffer.append(xrefStreamData);
    appendObject("endstream\nendobj\n");

    appendObject("startxref\n");
    appendObject(QString("%1\n").arg(xrefStreamOffset).toLatin1());
    appendObject("%%EOF\n");

    pdf::PDFDocumentReader reader(nullptr, nullptr, false, false);
    pdf::PDFDocument document = reader.readFromBuffer(buffer);

    // The document reader swallows object stream errors internally, so the
    // failure is reported through the reader state, not via an exception.
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::Failed);
    QVERIFY(reader.getErrorMessage().contains(QStringLiteral("Object stream")));
}

QTEST_GUILESS_MAIN(ContentProcessorLimitsTest)

#include "tst_contentprocessorlimitstest.moc"