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

#include "pdfrgbtocmykfixup.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"

#include <QtTest>

class RgbToCmykFixupTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectsMissingTargetProfile();
    void analyzesWithoutMutating();
    void convertsVectorPaintAndEmbedsOutputIntent();
};

namespace
{

QByteArray loadCmykProfile()
{
    const QString fixture = QFINDTESTDATA("../loupe-preflight/testdata/fixtures/output-intent-cmyk.pdf");
    if (fixture.isEmpty())
    {
        return QByteArray();
    }

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixture);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK
        || document.getCatalog()->getOutputIntents().empty())
    {
        return QByteArray();
    }

    const pdf::PDFObject profileObject = document.getObject(
        document.getCatalog()->getOutputIntents().front().getOutputProfile());
    return profileObject.isStream() ? document.getDecodedStream(profileObject.getStream()) : QByteArray();
}

pdf::PDFDocument buildRgbDocument()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 200, 200));

    pdf::PDFDictionary streamDictionary;
    const QByteArray content("1 0 0 rg\n0 0 200 200 re\nf\n");
    streamDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"),
                              pdf::PDFObject::createInteger(content.size()));
    const pdf::PDFObjectReference streamReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            std::move(streamDictionary), QByteArray(content))));

    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"),
                        pdf::PDFObject::createReference(streamReference));
    builder.mergeTo(pageReference,
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(
                        std::move(pageUpdate))));
    return builder.build();
}

pdf::PDFRgbToCmykSettings settingsWithProfile()
{
    pdf::PDFRgbToCmykSettings settings;
    settings.targetIccData = loadCmykProfile();
    settings.targetIccId = QByteArrayLiteral("test-cmyk");
    settings.targetProfileName = QStringLiteral("Test CMYK");
    return settings;
}

QByteArray firstPageContent(const pdf::PDFDocument& document)
{
    const pdf::PDFPage* page = document.getCatalog()->getPage(0);
    const pdf::PDFObject content = document.getObject(page->getContents());
    return content.isStream() ? document.getDecodedStream(content.getStream()) : QByteArray();
}

} // namespace

void RgbToCmykFixupTest::rejectsMissingTargetProfile()
{
    pdf::PDFDocument document = buildRgbDocument();
    pdf::PDFRgbToCmykSettings settings;
    pdf::PDFRgbToCmykReport report;
    const pdf::PDFOperationResult result = pdf::PDFRgbToCmykFixup::writeRgbToCmyk(&document, settings, &report);
    QVERIFY(!result);
    QVERIFY(result.getErrorMessage().contains(QStringLiteral("required"), Qt::CaseInsensitive));
}

void RgbToCmykFixupTest::analyzesWithoutMutating()
{
    pdf::PDFRgbToCmykSettings settings = settingsWithProfile();
    if (settings.targetIccData.isEmpty())
    {
        QSKIP("CMYK output-intent fixture is unavailable.");
    }
    settings.dryRunOnly = true;
    pdf::PDFDocument document = buildRgbDocument();
    const QByteArray before = firstPageContent(document);
    pdf::PDFRgbToCmykReport report;
    const pdf::PDFOperationResult writeResult = pdf::PDFRgbToCmykFixup::writeRgbToCmyk(&document, settings, &report);
    qWarning().noquote() << "TEMP-DIAG writeRgbToCmyk(analyzesWithoutMutating):" << writeResult.getErrorMessage();
    QVERIFY(writeResult);
    QCOMPARE(report.vectorPaintsConverted, 1);
    QCOMPARE(firstPageContent(document), before);
}

void RgbToCmykFixupTest::convertsVectorPaintAndEmbedsOutputIntent()
{
    pdf::PDFRgbToCmykSettings settings = settingsWithProfile();
    if (settings.targetIccData.isEmpty())
    {
        QSKIP("CMYK output-intent fixture is unavailable.");
    }
    pdf::PDFDocument document = buildRgbDocument();
    pdf::PDFRgbToCmykReport report;
    QVERIFY(pdf::PDFRgbToCmykFixup::writeRgbToCmyk(&document, settings, &report));
    const QByteArray content = firstPageContent(document);
    QVERIFY(!content.contains("rg"));
    QVERIFY(content.contains("k"));
    QVERIFY(report.outputIntentChanged);
    QVERIFY(report.postflightPassed);
    QVERIFY(!document.getCatalog()->getOutputIntents().empty());
}

QTEST_MAIN(RgbToCmykFixupTest)
#include "tst_rgbtocmykfixup.moc"
