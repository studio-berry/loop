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
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"

#include <QtTest>
#include <QBuffer>

class IncrementalSaveTest : public QObject
{
    Q_OBJECT

private slots:
    void preservesOriginalPrefixAndChangedObjects();
    void rejectsChangedSourceBytes();
    void selectsSafeWritePolicy();
    void explicitPoliciesCannotBeDowngradedToIncremental();
};

namespace
{

pdf::PDFDocument createDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    return builder.build();
}

pdf::PDFDocumentPointer createModifiedDocument(const pdf::PDFDocument& source)
{
    pdf::PDFDocumentModifier modifier(&source);
    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Rotate"), pdf::PDFObject::createInteger(90));
    modifier.getBuilder()->mergeTo(source.getCatalog()->getPage(0)->getPageReference(),
                                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    modifier.markPageContentsChanged();
    if (!modifier.finalize())
    {
        return {};
    }

    return modifier.getDocument();
}

pdf::PDFDocument readDocument(const QByteArray& data)
{
    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    return reader.readFromBuffer(data);
}

QByteArray writeDocument(const pdf::PDFDocument& document)
{
    pdf::PDFDocumentWriter writer(nullptr);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    Q_ASSERT(writer.write(&buffer, &document));
    return buffer.data();
}

} // namespace

void IncrementalSaveTest::preservesOriginalPrefixAndChangedObjects()
{
    const QByteArray originalData = writeDocument(createDocument());
    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    const pdf::PDFDocument original = reader.readFromBuffer(originalData);
    QVERIFY(reader.getReadingResult() == pdf::PDFDocumentReader::Result::OK);

    const pdf::PDFDocumentPointer modified = createModifiedDocument(original);
    QVERIFY(modified);

    pdf::PDFDocumentWriter writer(nullptr);
    QBuffer output;
    output.open(QIODevice::WriteOnly);
    QVERIFY(writer.writeIncremental(&output, originalData, &original, modified.data()));

    QVERIFY(output.data().size() > originalData.size());
    QCOMPARE(output.data().left(originalData.size()), originalData);
    QVERIFY(output.data().contains("/Prev"));

    const pdf::PDFDocument written = readDocument(output.data());
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&written, false, false),
             pdf::PDFDocumentWriter::WriteMode::Incremental);
    const pdf::PDFPage* page = written.getCatalog()->getPage(0);
    const pdf::PDFObject pageObject = written.getObjectByReference(page->getPageReference());
    const pdf::PDFObject rotation = written.getObject(pageObject.getDictionary()->get("Rotate"));
    QCOMPARE(rotation.getInteger(), pdf::PDFInteger(90));
}

void IncrementalSaveTest::rejectsChangedSourceBytes()
{
    const QByteArray originalData = writeDocument(createDocument());
    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    const pdf::PDFDocument original = reader.readFromBuffer(originalData);
    const pdf::PDFDocumentPointer modified = createModifiedDocument(original);
    QVERIFY(modified);

    pdf::PDFDocumentWriter writer(nullptr);
    QBuffer output;
    output.open(QIODevice::WriteOnly);
    QVERIFY(!writer.writeIncremental(&output, originalData + QByteArrayLiteral("changed"), &original, modified.data()));
    QVERIFY(output.data().isEmpty());
}

void IncrementalSaveTest::selectsSafeWritePolicy()
{
    const pdf::PDFDocument unsignedDocument = createDocument();
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&unsignedDocument, false, false),
             pdf::PDFDocumentWriter::WriteMode::FullRewrite);
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&unsignedDocument, true, false),
             pdf::PDFDocumentWriter::WriteMode::FullRewrite);
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&unsignedDocument, false, true),
             pdf::PDFDocumentWriter::WriteMode::FullRewrite);

    pdf::PDFDocumentBuilder signedBuilder;
    signedBuilder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDictionary signature;
    signature.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("Sig"));
    signedBuilder.addObject(pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(signature))));
    const pdf::PDFDocument signedDocument = signedBuilder.build();
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&signedDocument, false, false),
             pdf::PDFDocumentWriter::WriteMode::Incremental);
}

void IncrementalSaveTest::explicitPoliciesCannotBeDowngradedToIncremental()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument document = builder.build();

    const pdf::PDFOperationSavePolicy incremental = pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("annotation edit"));
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&document, incremental, false),
             pdf::PDFDocumentWriter::WriteMode::FullRewrite);

    const pdf::PDFOperationSavePolicy full = pdf::PDFOperationSavePolicy::fullRewrite(QStringLiteral("redaction"));
    QCOMPARE(full.mode, pdf::PDFSaveMode::FullRewrite);
    QVERIFY(full.invalidatesSignatures);
    QVERIFY(!full.reversibleInSession);
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&document, full, false),
             pdf::PDFDocumentWriter::WriteMode::FullRewrite);

    const pdf::PDFOperationSavePolicy newArtifact = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("production correction"));
    QCOMPARE(newArtifact.mode, pdf::PDFSaveMode::SaveAsNewArtifact);
    QVERIFY(newArtifact.invalidatesSignatures);
    QVERIFY(newArtifact.reversibleInSession);
    QCOMPARE(pdf::PDFDocumentWriter::getRecommendedWriteMode(&document, newArtifact, false),
             pdf::PDFDocumentWriter::WriteMode::FullRewrite);
    QCOMPARE(QString::fromLatin1(pdf::getPDFSaveModeName(newArtifact.mode)), QStringLiteral("save-as-new-artifact"));
}

QTEST_MAIN(IncrementalSaveTest)

#include "tst_incrementalsavetest.moc"
