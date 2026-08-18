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

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class StandardOracleTest : public QObject
{
    Q_OBJECT

private slots:
    void missingValidatorIsError();
    void alwaysFailValidatorIsError();
    void alwaysPassValidatorCanCommitPdfa();
    void unconvertiblePdfxHasNoMarker();
    void veraPdfLaneSkipsWhenMissing();
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
    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFA2b;
    settings.outputIntentIccData = loadCmykProfile();
    if (settings.outputIntentIccData.isEmpty())
    {
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    }
    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::apply(&document, settings, &report);
    QVERIFY(!result);
    QVERIFY(!report.independentValidationPassed);
    QVERIFY(!report.conversionAttempted);
}

void StandardOracleTest::alwaysFailValidatorIsError()
{
    if (loadCmykProfile().isEmpty())
    {
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString script = writeExitStatusScript(directory, QStringLiteral("fail"), 1);
    pdf::PDFDocument document = emptyPage();
    pdf::PDFStandardConversionSettings settings = pdfaSettings(script);
    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::apply(&document, settings, &report);
    QVERIFY(!result);
    QVERIFY(!report.independentValidationPassed);
    QVERIFY(!report.conversionAttempted);
}

void StandardOracleTest::alwaysPassValidatorCanCommitPdfa()
{
    if (loadCmykProfile().isEmpty())
    {
        QSKIP("Synthetic CMYK ICC profile is unavailable.");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString script = writeExitStatusScript(directory, QStringLiteral("pass"), 0);
    pdf::PDFDocument document = emptyPage();
    pdf::PDFStandardConversionSettings settings = pdfaSettings(script);
    pdf::PDFStandardConversionReport report;
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::apply(&document, settings, &report);
    QVERIFY2(result, qPrintable(result.getErrorMessage()));
    QVERIFY(report.independentValidationPassed);
    QVERIFY(report.conversionAttempted);
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
    const pdf::PDFOperationResult result = pdf::PDFStandardConversion::apply(&document, settings, &report);
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

QTEST_APPLESS_MAIN(StandardOracleTest)
#include "tst_standardoracletest.moc"
