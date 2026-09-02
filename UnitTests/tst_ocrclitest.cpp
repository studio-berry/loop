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

#include "processoutputcapture.h"

#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

class OcrCliTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void pdftoolOcr_withMockSidecar_emitsReport();
    void textOnlyFile_neverStartsSidecar();
    void malformedSidecarResponse_becomesPartialFailure();
    void sidecarStderrNoise_doesNotCorruptProtocol();

private:
    QString pdfToolPath() const;
    QString mockSidecarPath() const;
    QString fixturePdfPath() const;
    QString textOnlyPdfPath() const;
    QString missingSidecarPath() const;
    bool runMockOcr(const QString& mode, QJsonObject& envelope, int& exitCode, QString& error) const;
};

void OcrCliTest::initTestCase()
{
    if (!qEnvironmentVariableIsEmpty("LOOP_OCR_SKIP"))
    {
        QSKIP("LOOP_OCR_SKIP is set");
    }
}

QString OcrCliTest::pdfToolPath() const
{
    return QStringLiteral(PDFTOOL_EXECUTABLE_PATH);
}

QString OcrCliTest::mockSidecarPath() const
{
    // Drive the Python mock directly so CI does not depend on +x bits or
    // shebang/CRLF behavior of the thin .sh/.cmd wrappers.
    return QDir(QStringLiteral(LOOP_OCR_SOURCE_DIR))
        .filePath(QStringLiteral("tools/mock_ocr_sidecar.py"));
}

QString OcrCliTest::fixturePdfPath() const
{
    return QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR))
        .filePath(QStringLiteral("testdata/fixtures/image-dpi-low.pdf"));
}

QString OcrCliTest::textOnlyPdfPath() const
{
    return QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR))
        .filePath(QStringLiteral("testdata/fixtures/font-embedded.pdf"));
}

QString OcrCliTest::missingSidecarPath() const
{
    return QDir::temp().filePath(QStringLiteral("loop-ocr-sidecar-does-not-exist"));
}

bool OcrCliTest::runMockOcr(const QString& mode,
                            QJsonObject& envelope,
                            int& exitCode,
                            QString& error) const
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    if (!mode.isEmpty())
    {
        environment.insert(QStringLiteral("LOOP_OCR_MOCK_MODE"), mode);
    }
    process.setProcessEnvironment(environment);
    process.setProgram(pdfToolPath());
    process.setArguments({ QStringLiteral("ocr"),
                           fixturePdfPath(),
                           QStringLiteral("--console-format"),
                           QStringLiteral("json"),
                           QStringLiteral("--sidecar"),
                           mockSidecarPath() });
    process.start();
    QByteArray stdOut;
    QByteArray stdErr;
    if (!test_support::waitForFinishedAndCapture(process, 120000, stdOut, stdErr))
    {
        error = QStringLiteral("PdfTool did not finish: %1\nstderr: %2")
                    .arg(process.errorString(), QString::fromUtf8(stdErr));
        return false;
    }

    exitCode = process.exitCode();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdOut, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = parseError.errorString();
        return false;
    }

    envelope = document.object();
    return true;
}

void OcrCliTest::pdftoolOcr_withMockSidecar_emitsReport()
{
    QVERIFY2(QFileInfo::exists(pdfToolPath()), qPrintable(QStringLiteral("PdfTool not found at ") + pdfToolPath()));
    QVERIFY2(QFileInfo::exists(mockSidecarPath()), qPrintable(QStringLiteral("Mock sidecar not found at ") + mockSidecarPath()));
    QVERIFY2(QFileInfo::exists(fixturePdfPath()), qPrintable(QStringLiteral("Fixture not found at ") + fixturePdfPath()));

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    process.setProgram(pdfToolPath());
    process.setArguments({ QStringLiteral("ocr"),
                           fixturePdfPath(),
                           QStringLiteral("--console-format"),
                           QStringLiteral("json"),
                           QStringLiteral("--sidecar"),
                           mockSidecarPath() });
    process.start();
    QByteArray stdOut;
    QByteArray stdErr;
    QVERIFY(test_support::waitForFinishedAndCapture(process, 120000, stdOut, stdErr));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);

    QVERIFY2(process.exitCode() == 0 || process.exitCode() == 1,
             qPrintable(QStringLiteral("unexpected exit %1\nstdout: %2\nstderr: %3")
                            .arg(process.exitCode())
                            .arg(QString::fromUtf8(stdOut))
                            .arg(QString::fromUtf8(stdErr))));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdOut, &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));
    QVERIFY(document.isObject());

    const QJsonObject envelope = document.object();
    QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(envelope.value(QStringLiteral("status")).toString(), QStringLiteral("success"));

    const QJsonObject data = envelope.value(QStringLiteral("data")).toObject();
    QVERIFY(data.contains(QStringLiteral("report")));
    const QJsonObject report = data.value(QStringLiteral("report")).toObject();
    QVERIFY(report.contains(QStringLiteral("pages")));

    bool foundOcrPage = false;
    const auto pages = report.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue& pageValue : pages)
    {
        const QJsonObject pageObject = pageValue.toObject();
        if (pageObject.value(QStringLiteral("status")).toString() == QStringLiteral("ocr"))
        {
            foundOcrPage = true;
            QCOMPARE(pageObject.value(QStringLiteral("text")).toString(), QStringLiteral("MOCK OCR TEXT"));
        }
    }
    QVERIFY(foundOcrPage);
}

void OcrCliTest::textOnlyFile_neverStartsSidecar()
{
    QVERIFY2(QFileInfo::exists(pdfToolPath()), qPrintable(QStringLiteral("PdfTool not found at ") + pdfToolPath()));
    QVERIFY2(QFileInfo::exists(textOnlyPdfPath()), qPrintable(QStringLiteral("Fixture not found at ") + textOnlyPdfPath()));

    // A text-based document must never require (or launch) an OCR sidecar. Use a
    // path that cannot exist: on a file that needs OCR this run would abort with
    // a processing failure (sidecar unavailable), so exit 0 here is direct proof
    // that the engine was never consulted.
    QFile::remove(missingSidecarPath());
    QVERIFY2(!QFileInfo::exists(missingSidecarPath()),
             qPrintable(QStringLiteral("Sidecar path unexpectedly exists at ") + missingSidecarPath()));

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    process.setProgram(pdfToolPath());
    process.setArguments({ QStringLiteral("ocr"),
                           textOnlyPdfPath(),
                           QStringLiteral("--console-format"),
                           QStringLiteral("json"),
                           QStringLiteral("--sidecar"),
                           missingSidecarPath() });
    process.start();
    QByteArray stdOut;
    QByteArray stdErr;
    QVERIFY(test_support::waitForFinishedAndCapture(process, 120000, stdOut, stdErr));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);

    QVERIFY2(process.exitCode() == 0,
             qPrintable(QStringLiteral("unexpected exit %1\nstdout: %2\nstderr: %3")
                            .arg(process.exitCode())
                            .arg(QString::fromUtf8(stdOut))
                            .arg(QString::fromUtf8(stdErr))));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdOut, &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));
    QVERIFY(document.isObject());

    const QJsonObject envelope = document.object();
    QCOMPARE(envelope.value(QStringLiteral("status")).toString(), QStringLiteral("success"));

    const QJsonObject data = envelope.value(QStringLiteral("data")).toObject();
    QVERIFY(data.contains(QStringLiteral("report")));
    const QJsonObject report = data.value(QStringLiteral("report")).toObject();
    QVERIFY(report.contains(QStringLiteral("pages")));

    bool anyOcrPage = false;
    bool anySkippedPage = false;
    const auto pages = report.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue& pageValue : pages)
    {
        const QJsonObject pageObject = pageValue.toObject();
        const QString status = pageObject.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("ocr"))
        {
            anyOcrPage = true;
        }
        else if (status == QStringLiteral("skipped_has_text") || status == QStringLiteral("skipped_empty"))
        {
            anySkippedPage = true;
        }
    }
    QVERIFY2(!anyOcrPage, "a text-only file must have no OCR'd pages");
    QVERIFY2(anySkippedPage, "every page of a text-only file must be reported as skipped");
}

void OcrCliTest::malformedSidecarResponse_becomesPartialFailure()
{
    QJsonObject envelope;
    int exitCode = -1;
    QString error;
    QVERIFY2(runMockOcr(QStringLiteral("malformed-json"), envelope, exitCode, error), qPrintable(error));
    QCOMPARE(exitCode, 5);
    QCOMPARE(envelope.value(QStringLiteral("status")).toString(), QStringLiteral("partial-output"));

    const QJsonObject report = envelope.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QVERIFY(!report.value(QStringLiteral("errors")).toArray().isEmpty());
    bool foundFailedPage = false;
    for (const QJsonValue& pageValue : report.value(QStringLiteral("pages")).toArray())
    {
        if (pageValue.toObject().value(QStringLiteral("status")).toString() == QStringLiteral("failed"))
        {
            foundFailedPage = true;
        }
    }
    QVERIFY(foundFailedPage);
}

void OcrCliTest::sidecarStderrNoise_doesNotCorruptProtocol()
{
    QJsonObject envelope;
    int exitCode = -1;
    QString error;
    QVERIFY2(runMockOcr(QStringLiteral("stderr-noise"), envelope, exitCode, error), qPrintable(error));
    QCOMPARE(exitCode, 0);

    const QJsonObject report = envelope.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    bool foundOcrPage = false;
    for (const QJsonValue& pageValue : report.value(QStringLiteral("pages")).toArray())
    {
        if (pageValue.toObject().value(QStringLiteral("status")).toString() == QStringLiteral("ocr"))
        {
            foundOcrPage = true;
        }
    }
    QVERIFY(foundOcrPage);
}

QTEST_MAIN(OcrCliTest)
#include "tst_ocrclitest.moc"
