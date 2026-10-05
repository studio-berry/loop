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
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
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
    void realPdfaConversionTriad();
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
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::prepare(&document, settings, &report);
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
    const auto result = pdf::PDFStandardConversion::validateArtifact(QByteArrayLiteral("%PDF-1.7"), settings);
    QVERIFY(result.status != pdf::PDFArtifactValidationStatus::Passed);
}

void ConversionOracleTest::realPdfaConversionTriad()
{
    const QString program = QStandardPaths::findExecutable(QStringLiteral("verapdf"));
    const QString outputDirectory = QString::fromUtf8(qgetenv("LOOP_INDEPENDENT_CONVERSION_DIR"));
    if (program.isEmpty())
    {
        if (!outputDirectory.isEmpty())
            QFAIL("Qualification requires veraPDF; no missing-tool skip is permitted.");
        QSKIP("veraPDF is unavailable; real conversion qualification remains incomplete.");
    }
    const auto load = [](const QString& name)
    {
        QFile file(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/independent/") + name);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    };
    const QByteArray conformant = load(QStringLiteral("pdfa-2b-conformant.pdf"));
    const QByteArray forbidden = load(QStringLiteral("pdfa-2b-forbidden-action.pdf"));
    QVERIFY(!conformant.isEmpty());
    QVERIFY(!forbidden.isEmpty());
    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFA2b;
    settings.independentValidatorProgram = program;
    const auto positive = pdf::PDFStandardConversion::validateArtifact(conformant, settings);
    QVERIFY2(positive.status == pdf::PDFArtifactValidationStatus::Passed, qPrintable(positive.reason + QString::fromUtf8(QJsonDocument(positive.evidence).toJson())));
    const auto negative = pdf::PDFStandardConversion::validateArtifact(forbidden, settings);
    QCOMPARE(negative.status, pdf::PDFArtifactValidationStatus::Rejected);

    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, false, false);
    pdf::PDFDocument source = reader.readFromBuffer(conformant);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);
    pdf::PDFDocumentBuilder builder(&source);
    const auto catalogReference = builder.getCatalogReference();
    pdf::PDFDictionary catalog = *builder.getObject(pdf::PDFObject::createReference(catalogReference)).getDictionary();
    const auto* intents = source.getObject(catalog.get("OutputIntents")).getArray();
    QVERIFY(intents && intents->getCount() == 1);
    const auto* intent = source.getDictionaryFromObject(intents->getItem(0));
    QVERIFY(intent);
    const auto& profile = source.getObject(intent->get("DestOutputProfile"));
    QVERIFY(profile.isStream());
    settings.outputIntentIccData = source.getDecodedStream(profile.getStream());
    QVERIFY(!settings.outputIntentIccData.isEmpty());
    catalog.removeEntry("Metadata");
    builder.setObject(catalogReference, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(catalog))));
    pdf::PDFDocument convertible = builder.build();
    QByteArray convertibleBytes;
    QBuffer sourceBuffer(&convertibleBytes);
    QVERIFY(sourceBuffer.open(QIODevice::WriteOnly));
    pdf::PDFDocumentWriter writer(nullptr);
    QVERIFY(writer.write(&sourceBuffer, &convertible));
    QCOMPARE(pdf::PDFStandardConversion::validateArtifact(convertibleBytes, settings).status, pdf::PDFArtifactValidationStatus::Rejected);
    pdf::PDFStandardConversionReport preparation;
    QVERIFY(pdf::PDFStandardConversion::prepare(&convertible, settings, &preparation));
    QVERIFY(!preparation.independentValidationPassed);
    QTemporaryDir temporary;
    const QString directory = outputDirectory.isEmpty() ? temporary.path() : outputDirectory;
    QVERIFY(QDir().mkpath(directory));
    QJsonArray evidence;
    QByteArray outputBytes;
    const auto publication = pdf::PDFStandardConversion::writeCandidate(convertible, QDir(directory).filePath(QStringLiteral("safely-convertible.pdf")),
                                                                        { settings }, nullptr, &outputBytes, &evidence);
    QVERIFY2(publication, QJsonDocument(evidence).toJson().constData());
    QCOMPARE(pdf::PDFStandardConversion::validateArtifact(outputBytes, settings).status, pdf::PDFArtifactValidationStatus::Passed);

    pdf::PDFDocument unconvertible = reader.readFromBuffer(forbidden);
    QVERIFY(pdf::PDFStandardConversion::prepare(&unconvertible, settings));
    const QString destination = QDir(directory).filePath(QStringLiteral("must-preserve.pdf"));
    QFile existing(destination);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("existing destination"), qint64(20));
    existing.close();
    QVERIFY(!pdf::PDFStandardConversion::writeCandidate(unconvertible, destination, { settings }, nullptr, nullptr, &evidence));
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArrayLiteral("existing destination"));
    existing.close();
    if (!outputDirectory.isEmpty())
    {
        QFile input(QDir(directory).filePath(QStringLiteral("safely-convertible-input.pdf")));
        QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(convertibleBytes), qint64(convertibleBytes.size()));
        QFile report(QDir(directory).filePath(QStringLiteral("rejection.json")));
        QVERIFY(report.open(QIODevice::WriteOnly));
        QVERIFY(report.write(QJsonDocument(evidence).toJson()) > 0);
    }
}

QTEST_APPLESS_MAIN(ConversionOracleTest)
#include "tst_conversionoracletest.moc"
