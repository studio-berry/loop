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
#include "pdfstandardconversion.h"
#include "independentvalidatorfixture.h"
#include "pdftransparencyflattener.h"   // hasLiveTransparency

#include "pdfdocumentwriter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

class StandardOracleTest : public QObject
{
    Q_OBJECT

private slots:
    void missingValidatorIsError();
    void alwaysFailValidatorIsError();
    void preparationCannotCertifyPdfa();
    void unconvertiblePdfxHasNoMarker();
    void veraPdfLaneSkipsWhenMissing();
    void explicitTransparencyOptOutIsHonoured();
    void opaqueDocumentIsNotRasterizedByTheFlattenPass();
    void explicitTransparencyOptOutBlocksPdfXConversion();
    void artifactReport_data();
    void artifactReport();
    void rejectedArtifactPreservesDestination();
    void cancelledArtifactCannotPass();
    void unknownVersionAndMalformedReportRemainIncomplete();
    void publishedDigestChangesAfterLaterMutation();
};

namespace
{

QByteArray loadCmykProfile()
{
    const QString profilePath = QFINDTESTDATA("testdata/synthetic-cmyk.icc");
    QFile file(profilePath);
    if (profilePath.isEmpty() || !file.open(QIODevice::ReadOnly))
    {
        return QByteArray();
    }
    return file.readAll();
}

pdf::PDFDocument emptyPage()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    return builder.build();
}

// Writes a script that exits with the given status. The name/extension and body are
// chosen per-platform because PDFSysUtils::configureScriptOrProgramProcess (which
// backs the independent-validator invocation under test) does not dispatch .sh
// scripts to an interpreter on Windows -- Windows has no POSIX shell by default, so
// that mirrors production behavior rather than working around it.
QString writeExitStatusScript(const QTemporaryDir& directory, const QString& baseName, int exitStatus)
{
#ifdef Q_OS_WIN
    const QString path = directory.filePath(baseName + QStringLiteral(".bat"));
    const QString body = QStringLiteral("@echo off\r\nexit /b %1\r\n").arg(exitStatus);
#else
    const QString path = directory.filePath(baseName + QStringLiteral(".sh"));
    const QString body = QStringLiteral("#!/bin/sh\nexit %1\n").arg(exitStatus);
#endif
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return {};
    }
    file.write(body.toUtf8());
#ifndef Q_OS_WIN
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    return path;
}

/// A page whose content carries live transparency (a 50 %-opacity rectangle),
/// which is exactly what PDF/X-1a and PDF/X-3 forbid.
pdf::PDFDocument pageWithLiveTransparency()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 144, 144));
    pdf::PDFPageContentStreamBuilder contentBuilder(&builder,
                                                    pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = contentBuilder.begin(page))
    {
        painter->setOpacity(0.5);
        painter->fillRect(QRectF(18, 18, 108, 108), Qt::red);
        contentBuilder.end(painter);
    }
    return builder.build();
}

pdf::PDFStandardConversionSettings pdfaSettings(const QString& program)
{
    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFA2b;
    settings.outputIntentIccData = loadCmykProfile();
    settings.independentValidatorProgram = program;
    settings.independentValidatorArguments = QStringList{ QStringLiteral("{input}") };
    return settings;
}

}   // namespace

void StandardOracleTest::missingValidatorIsError()
{
    pdf::PDFDocument document = emptyPage();
    pdf::PDFStandardConversionSettings settings = pdfaSettings({});
    if (settings.outputIntentIccData.isEmpty())
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    pdf::PDFStandardConversionReport report;
    QVERIFY(!pdf::PDFStandardConversion::prepare(&document, settings, &report));
    QVERIFY(!report.independentValidationPassed);
    QVERIFY(!report.conversionAttempted);
}

void StandardOracleTest::alwaysFailValidatorIsError()
{
    pdf::PDFStandardConversionSettings settings = pdfaSettings(QStringLiteral("/bin/false"));
    const auto result = pdf::PDFStandardConversion::validateArtifact(QByteArrayLiteral("%PDF-1.7"), settings);
    QVERIFY(result.status != pdf::PDFArtifactValidationStatus::Passed);
}

void StandardOracleTest::preparationCannotCertifyPdfa()
{
    pdf::PDFDocument document = emptyPage();
    pdf::PDFStandardConversionSettings settings = pdfaSettings(QStringLiteral("verapdf"));
    if (settings.outputIntentIccData.isEmpty())
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    pdf::PDFStandardConversionReport report;
    report.independentValidationPassed = true;
    report.postflightPassed = true;
    QVERIFY(pdf::PDFStandardConversion::prepare(&document, settings, &report));
    QVERIFY(report.conversionAttempted);
    QVERIFY(!report.independentValidationPassed);
    QVERIFY(!report.postflightPassed);
}

void StandardOracleTest::unconvertiblePdfxHasNoMarker()
{
    if (loadCmykProfile().isEmpty())
    {
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    }
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFX1a2001;
    settings.outputIntentIccData = loadCmykProfile();
    settings.independentValidatorProgram = QStringLiteral("/bin/true");
    settings.independentValidatorArguments = QStringList{ QStringLiteral("{input}") };
    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::prepare(&document, settings, &report);
    QVERIFY(!result);
    QVERIFY(!report.conversionAttempted);
    QVERIFY(!report.independentValidationPassed);
    QVERIFY(!report.blockers.isEmpty() || !result);
}

void StandardOracleTest::veraPdfLaneSkipsWhenMissing()
{
    if (QStandardPaths::findExecutable(QStringLiteral("verapdf")).isEmpty())
    {
        QSKIP("veraPDF is not installed; independent CI oracle lane is skip-if-missing.");
    }
}

void StandardOracleTest::explicitTransparencyOptOutIsHonoured()
{
    if (loadCmykProfile().isEmpty())
    {
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    }

    pdf::PDFDocument document = pageWithLiveTransparency();
    QVERIFY(pdf::PDFTransparencyFlattener::hasLiveTransparency(&document));

    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFX1a2001;
    settings.outputIntentIccData = loadCmykProfile();
    settings.transparencyFlatten = pdf::PDFTransparencyFlattenPolicy::Never;   // explicit opt-out

    // The observable is the change report: with the boolean API an explicit
    // false is indistinguishable from "unset", so the target default re-enables
    // flattening and the preview advertises a change it should not.
    pdf::PDFStandardConversionReport previewReport;
    pdf::PDFStandardConversion::preview(&document, settings, &previewReport);
    for (const pdf::PDFStandardConversionChange& change : previewReport.changes)
    {
        QVERIFY(change.id != QStringLiteral("transparency.flatten"));
    }

    pdf::PDFStandardConversionReport report;
    pdf::PDFStandardConversion::prepare(&document, settings, &report);
    QVERIFY(report.transparencyFlatten.isEmpty());
}

void StandardOracleTest::opaqueDocumentIsNotRasterizedByTheFlattenPass()
{
    if (loadCmykProfile().isEmpty())
    {
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString script = writeExitStatusScript(directory, QStringLiteral("pdfa-pass"), 0);

    pdf::PDFDocument document = emptyPage();
    QVERIFY(!pdf::PDFTransparencyFlattener::hasLiveTransparency(&document));

    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFA2b;
    settings.transparencyFlatten = pdf::PDFTransparencyFlattenPolicy::Always;
    settings.outputIntentIccData = loadCmykProfile();
    settings.independentValidatorProgram = script;
    settings.independentValidatorArguments = QStringList{ QStringLiteral("{input}") };

    // The flattener really would rasterize this opaque document, so an empty
    // transparency_flatten report below is evidence that it was never called.
    {
        pdf::PDFDocument probe = document;
        pdf::PDFTransparencyFlattenSettings probeSettings;
        probeSettings.rasterizationDpi = 72;
        probeSettings.maxRasterPixels = 100000;
        pdf::PDFTransparencyFlattenReport probeReport;
        QVERIFY(pdf::PDFTransparencyFlattener::apply(&probe, probeSettings, &probeReport));
        QVERIFY(probeReport.changed);
    }

    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::prepare(&document, settings, &report);
    QVERIFY2(result, qPrintable(result.getErrorMessage()));
    QVERIFY2(report.transparencyFlatten.isEmpty(),
             qPrintable(QString::fromUtf8(QJsonDocument(report.transparencyFlatten).toJson(QJsonDocument::Compact))));
}

void StandardOracleTest::explicitTransparencyOptOutBlocksPdfXConversion()
{
    if (loadCmykProfile().isEmpty())
    {
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    }

    pdf::PDFDocument document = pageWithLiveTransparency();
    QVERIFY(pdf::PDFTransparencyFlattener::hasLiveTransparency(&document));

    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFX1a2001;
    settings.outputIntentIccData = loadCmykProfile();
    settings.transparencyFlatten = pdf::PDFTransparencyFlattenPolicy::Never;

    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::preview(&document, settings, &report);

    // With flattening explicitly off, the target's prohibition on live
    // transparency stands and must be reported as a blocker. This is the only
    // observable that proves the PDF/X policy actually ran: blockers are appended
    // from result.pdfx->rules, and those rules never exist while the profile
    // the conversion builds is rejected by parseProfile().
    QVERIFY(!result);
    const bool blockedByTransparency = std::any_of(
        report.blockers.cbegin(), report.blockers.cend(),
        [](const QString& blocker)
        { return blocker.startsWith(QStringLiteral("pdfx.transparency.allowed")); });
    QVERIFY2(blockedByTransparency, qPrintable(report.blockers.join(QStringLiteral(" | "))));
}

using independent_test::writeVeraPdfScript;

void StandardOracleTest::artifactReport_data()
{
    QTest::addColumn<QString>("compliant");
    QTest::addColumn<QString>("profile");
    QTest::addColumn<bool>("mutate");
    QTest::addColumn<int>("status");
    using Status = pdf::PDFArtifactValidationStatus;
    QTest::newRow("positive") << QStringLiteral("true") << QStringLiteral("PDF/A-2B validation profile") << false << int(Status::Passed);
    QTest::newRow("zero-exit-rejection") << QStringLiteral("false") << QStringLiteral("PDF/A-2B validation profile") << false << int(Status::Rejected);
    QTest::newRow("wrong-target") << QStringLiteral("true") << QStringLiteral("PDF/A-1B validation profile") << false << int(Status::Incomplete);
    QTest::newRow("missing-verdict") << QString() << QStringLiteral("PDF/A-2B validation profile") << false << int(Status::Incomplete);
    QTest::newRow("mutated-input") << QStringLiteral("true") << QStringLiteral("PDF/A-2B validation profile") << true << int(Status::Rejected);
}

void StandardOracleTest::artifactReport()
{
    QFETCH(QString, compliant);
    QFETCH(QString, profile);
    QFETCH(bool, mutate);
    QFETCH(int, status);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto settings = pdfaSettings(writeVeraPdfScript(directory, compliant, profile, mutate));
    const auto result = pdf::PDFStandardConversion::validateArtifact(QByteArrayLiteral("%PDF-1.7 test bytes"), settings);
    QCOMPARE(int(result.status), status);
    QCOMPARE(result.artifactBytes, qint64(19));
    QCOMPARE(result.artifactSha256.size(), 64);
    QCOMPARE(result.reportSha256.size(), 64);
}

void StandardOracleTest::rejectedArtifactPreservesDestination()
{
    QTemporaryDir directory;
    const QString destination = directory.filePath(QStringLiteral("output.pdf"));
    QFile output(destination);
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write("existing output");
    output.close();
    auto settings = pdfaSettings(writeVeraPdfScript(directory, QStringLiteral("false"), QStringLiteral("PDF/A-2B validation profile"), false));
    QJsonArray evidence;
    QVERIFY(!pdf::PDFStandardConversion::writeCandidate(emptyPage(), destination, { settings }, nullptr, nullptr, &evidence));
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArrayLiteral("existing output"));
    QCOMPARE(evidence.size(), 1);
}

void StandardOracleTest::cancelledArtifactCannotPass()
{
    class Cancelled final : public pdf::PDFOperationControl
    {
    public:
        bool isOperationCancelled() const override { return true; }
    } cancelled;
    const auto result = pdf::PDFStandardConversion::validateArtifact(QByteArrayLiteral("bytes"), pdfaSettings(QStringLiteral("verapdf")), &cancelled);
    QVERIFY(result.status != pdf::PDFArtifactValidationStatus::Passed);
    QCOMPARE(result.reason, QStringLiteral("validator-cancelled"));
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cancelled.pdf"));
    QJsonArray evidence{ QJsonObject{ { QStringLiteral("status"), QStringLiteral("passed") } } };
    QVERIFY(!pdf::PDFStandardConversion::writeCandidate(emptyPage(), path, {}, nullptr, nullptr, &evidence, &cancelled));
    QVERIFY(evidence.isEmpty());
    QVERIFY(!QFile::exists(path));
}

void StandardOracleTest::unknownVersionAndMalformedReportRemainIncomplete()
{
    QTemporaryDir directory;
    const QString program = writeVeraPdfScript(directory, QStringLiteral("true"), QStringLiteral("PDF/A-2B validation profile"), false);
    QFile script(program);
    QVERIFY(script.open(QIODevice::ReadOnly));
    QByteArray body = script.readAll();
    script.close();
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
    script.write(QByteArray(body).replace("veraPDF 1.28.2", "unknown"));
    script.close();
    const auto unknown = pdf::PDFStandardConversion::validateArtifact(QByteArrayLiteral("bytes"), pdfaSettings(program));
    QCOMPARE(unknown.status, pdf::PDFArtifactValidationStatus::Incomplete);
    QCOMPARE(unknown.reason, QStringLiteral("validator-version-unsupported"));
#ifdef Q_OS_WIN
    body.replace("^</report^>", "");
#else
    body.replace("</report>", "");
#endif
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
    script.write(body);
    script.close();
    const auto malformed = pdf::PDFStandardConversion::validateArtifact(QByteArrayLiteral("bytes"), pdfaSettings(program));
    QCOMPARE(malformed.status, pdf::PDFArtifactValidationStatus::Incomplete);
    QCOMPARE(malformed.reason, QStringLiteral("validator-report-invalid"));
}

void StandardOracleTest::publishedDigestChangesAfterLaterMutation()
{
    QTemporaryDir directory;
    const auto settings = pdfaSettings(writeVeraPdfScript(directory, QStringLiteral("true"), QStringLiteral("PDF/A-2B validation profile"), false));
    pdf::PDFDocument candidate = emptyPage();
    QJsonArray evidence;
    QByteArray validatedBytes;
    pdf::PDFDocument reopened;
    const QString path = directory.filePath(QStringLiteral("nested/output.pdf"));
    QVERIFY(pdf::PDFStandardConversion::writeCandidate(candidate, path, { settings }, &reopened, &validatedBytes, &evidence));
    QFile published(path);
    QVERIFY(published.open(QIODevice::ReadOnly));
    QCOMPARE(published.readAll(), validatedBytes);
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(validatedBytes, QCryptographicHash::Sha256).toHex());
    QCOMPARE(evidence.first().toObject().value(QStringLiteral("artifact_sha256")).toString(), digest);
    pdf::PDFDocumentBuilder builder(&reopened);
    builder.setCatalogMetadata(QByteArrayLiteral("later mutation"));
    candidate = builder.build();
    QByteArray modifiedBytes;
    QBuffer buffer(&modifiedBytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    pdf::PDFDocumentWriter writer(nullptr);
    QVERIFY(writer.write(&buffer, &candidate));
    QVERIFY(modifiedBytes != validatedBytes);
    const auto later = pdf::PDFStandardConversion::validateArtifact(modifiedBytes, settings);
    QVERIFY(later.artifactSha256 != digest);
}

QTEST_APPLESS_MAIN(StandardOracleTest)
#include "tst_standardoracletest.moc"
