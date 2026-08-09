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
#include "pdfdocumentreader.h"
#include "pdfexception.h"
#include "pdffont.h"
#include "pdfoptionalcontent.h"
#include "pdfrenderer.h"

#include <QtTest>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace
{

pdf::PDFObject createBoundingBox()
{
    pdf::PDFArray boundingBox;
    boundingBox.appendItem(pdf::PDFObject::createReal(0.0));
    boundingBox.appendItem(pdf::PDFObject::createReal(0.0));
    boundingBox.appendItem(pdf::PDFObject::createReal(100.0));
    boundingBox.appendItem(pdf::PDFObject::createReal(100.0));
    return pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(boundingBox)));
}

pdf::PDFObject createForm(const char* childName,
                          const pdf::PDFObjectReference& childReference)
{
    pdf::PDFDictionary xObjects;
    xObjects.addEntry(pdf::PDFInplaceOrMemoryString(childName), pdf::PDFObject::createReference(childReference));

    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObjects))));

    pdf::PDFDictionary dictionary;
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Form"));
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("BBox"), createBoundingBox());
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));

    QByteArray content("/");
    content.append(childName);
    content.append(" Do");
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                        pdf::PDFObject::createInteger(content.size()));

    return pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(dictionary), std::move(content)));
}

pdf::PDFDocument createRecursiveFormDocument(bool mutual)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFObjectReference firstFormReference = builder.addObject(pdf::PDFObject::createNull());
    const pdf::PDFObjectReference secondFormReference = builder.addObject(pdf::PDFObject::createNull());

    builder.setObject(firstFormReference, createForm(mutual ? "F2" : "F1",
                                                     mutual ? secondFormReference : firstFormReference));
    if (mutual)
    {
        builder.setObject(secondFormReference, createForm("F1", firstFormReference));
    }

    pdf::PDFDictionary pageXObjects;
    pageXObjects.addEntry(pdf::PDFInplaceOrMemoryString("F1"), pdf::PDFObject::createReference(firstFormReference));
    pdf::PDFDictionary pageResources;
    pageResources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                           pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageXObjects))));
    builder.mergeTo(pageReference, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageResources))));

    QByteArray pageContent = "/F1 Do";
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(pageContent.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(contentDictionary), std::move(pageContent))));
    pdf::PDFDictionary pageContentUpdate;
    pageContentUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(pageReference, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageContentUpdate))));

    return builder.build();
}

pdf::PDFDocument createNestedFormDocument(int depth)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 100, 100));

    std::vector<pdf::PDFObjectReference> formReferences;
    formReferences.reserve(depth);
    for (int i = 0; i < depth; ++i)
    {
        formReferences.push_back(builder.addObject(pdf::PDFObject::createNull()));
    }

    for (int i = depth - 1; i >= 0; --i)
    {
        if (i + 1 < depth)
        {
            builder.setObject(formReferences[i], createForm("F1", formReferences[i + 1]));
        }
        else
        {
            QByteArray content = "0 0 10 10 re f";
            pdf::PDFDictionary dictionary;
            dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
            dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Form"));
            dictionary.addEntry(pdf::PDFInplaceOrMemoryString("BBox"), createBoundingBox());
            dictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                                pdf::PDFObject::createInteger(content.size()));
            builder.setObject(formReferences[i], pdf::PDFObject::createStream(
                std::make_shared<pdf::PDFStream>(std::move(dictionary), std::move(content))));
        }
    }

    pdf::PDFDictionary pageXObjects;
    pageXObjects.addEntry(pdf::PDFInplaceOrMemoryString("F1"), pdf::PDFObject::createReference(formReferences.front()));
    pdf::PDFDictionary pageResources;
    pageResources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                           pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageXObjects))));
    builder.mergeTo(pageReference, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageResources))));

    QByteArray pageContent = "/F1 Do";
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(pageContent.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(contentDictionary), std::move(pageContent))));
    pdf::PDFDictionary pageContentUpdate;
    pageContentUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(pageReference, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageContentUpdate))));

    return builder.build();
}

QList<pdf::PDFRenderError> render(const pdf::PDFDocument& document)
{
    pdf::PDFCMSGeneric cms;
    pdf::PDFFontCache fontCache(32, 32);
    pdf::PDFOptionalContentActivity activity(&document, pdf::OCUsage::View, nullptr);
    fontCache.setDocument(pdf::PDFModifiedDocument(const_cast<pdf::PDFDocument*>(&document), &activity));

    pdf::PDFRenderer renderer(&document, &fontCache, &cms, &activity,
                              pdf::PDFRenderer::Features(), pdf::PDFMeshQualitySettings());
    QImage image(100, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    const QList<pdf::PDFRenderError> errors = renderer.render(&painter, QRectF(0, 0, 100, 100), 0);
    painter.end();
    return errors;
}

void appendBigEndian(QByteArray& data, quint64 value, int byteCount)
{
    for (int i = byteCount - 1; i >= 0; --i)
    {
        data.append(char((value >> (i * 8)) & 0xff));
    }
}

QByteArray createObjectStreamWithInvalidCount()
{
    QByteArray document("%PDF-1.7\n");
    std::array<int, 6> offsets = {};
    auto appendObject = [&document, &offsets](int objectNumber, const QByteArray& body)
    {
        offsets[objectNumber] = document.size();
        document += QByteArray::number(objectNumber) + " 0 obj\n" + body + "\nendobj\n";
    };

    appendObject(1, "<< /Type /Catalog /Pages 2 0 R >>");
    appendObject(2, "<< /Type /Pages /Count 0 /Kids [] >>");
    appendObject(3, "<< /Type /ObjStm /N 2147483647 /First 4 /Length 3 >>\nstream\n1 0\nendstream");

    const int xrefOffset = document.size();
    offsets[4] = xrefOffset;
    QByteArray xrefData;
    appendBigEndian(xrefData, 0, 1);
    appendBigEndian(xrefData, 0, 4);
    appendBigEndian(xrefData, 65535, 2);
    for (int objectNumber = 1; objectNumber <= 4; ++objectNumber)
    {
        appendBigEndian(xrefData, 1, 1);
        appendBigEndian(xrefData, offsets[objectNumber], 4);
        appendBigEndian(xrefData, 0, 2);
    }
    appendBigEndian(xrefData, 2, 1);
    appendBigEndian(xrefData, 3, 4);
    appendBigEndian(xrefData, 0, 2);

    QByteArray xrefBody = "<< /Type /XRef /Size 6 /Root 1 0 R /W [1 4 2] /Length 42 >>\nstream\n";
    xrefBody += xrefData;
    xrefBody += "\nendstream";
    appendObject(4, xrefBody);

    document += "startxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return document;
}

} // namespace

class ContentProcessorLimitsTest : public QObject
{
    Q_OBJECT

private slots:
    void selfReferencingFormIsRejected();
    void mutuallyReferencingFormsAreRejected();
    void deeplyNestedFormsAreBounded();
    void shallowNestedFormsRender();
    void objectStreamCountIsBounded();
};

void ContentProcessorLimitsTest::selfReferencingFormIsRejected()
{
    const QList<pdf::PDFRenderError> errors = render(createRecursiveFormDocument(false));
    QVERIFY(!errors.isEmpty());
    QVERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const pdf::PDFRenderError& error)
    {
        return error.message.contains("Recursive Form XObject reference");
    }));
}

void ContentProcessorLimitsTest::mutuallyReferencingFormsAreRejected()
{
    const QList<pdf::PDFRenderError> errors = render(createRecursiveFormDocument(true));
    QVERIFY(!errors.isEmpty());
    QVERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const pdf::PDFRenderError& error)
    {
        return error.message.contains("Recursive Form XObject reference");
    }));
}

void ContentProcessorLimitsTest::deeplyNestedFormsAreBounded()
{
    const QList<pdf::PDFRenderError> errors = render(createNestedFormDocument(40));
    QVERIFY(!errors.isEmpty());
    QVERIFY(std::any_of(errors.cbegin(), errors.cend(), [](const pdf::PDFRenderError& error)
    {
        return error.message.contains("Maximum content stream nesting depth exceeded");
    }));
}

void ContentProcessorLimitsTest::shallowNestedFormsRender()
{
    QVERIFY(render(createNestedFormDocument(16)).isEmpty());
}

void ContentProcessorLimitsTest::objectStreamCountIsBounded()
{
    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, false, false);
    reader.readFromBuffer(createObjectStreamWithInvalidCount());

    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::Failed);
    QVERIFY(reader.getErrorMessage().contains("Object stream 3 is invalid."));
}

QTEST_MAIN(ContentProcessorLimitsTest)

#include "tst_contentprocessorlimitstest.moc"
