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
#include "pdfpreflightverdict.h"
#include "pdfstandardconversion.h"

#include <QStandardPaths>
#include <QtTest>

class ConversionOracleTest : public QObject
{
    Q_OBJECT

private slots:
    void missingOracleCannotSelfCertify();
    void oracleMismatchIsErrorNotPass();
    void veraPdfLaneSkipsIfMissing();
};

static pdf::PDFDocument emptyPageDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    return builder.build();
}

void ConversionOracleTest::missingOracleCannotSelfCertify()
{
    pdf::PDFDocument document = emptyPageDocument();
    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFA2b;
    settings.independentValidatorProgram.clear();
    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::apply(&document, settings, &report);
    QVERIFY(!result);
    QVERIFY(!report.independentValidationPassed);
}

void ConversionOracleTest::oracleMismatchIsErrorNotPass()
{
    pdf::PDFDocument document = emptyPageDocument();
    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFA2b;
    settings.independentValidatorProgram = QStringLiteral("/bin/false");
    settings.independentValidatorArguments = QStringList{ QStringLiteral("{input}") };
    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::apply(&document, settings, &report);
    QVERIFY(!result);
    QVERIFY(!report.independentValidationPassed);
}

void ConversionOracleTest::veraPdfLaneSkipsIfMissing()
{
    if (QStandardPaths::findExecutable(QStringLiteral("verapdf")).isEmpty())
    {
        QSKIP("veraPDF is not bundled; the independent oracle lane is skip-if-missing.");
    }
    QVERIFY(!QStandardPaths::findExecutable(QStringLiteral("verapdf")).isEmpty());
}

QTEST_APPLESS_MAIN(ConversionOracleTest)
#include "tst_conversionoracletest.moc"
