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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

class PdfWorkerIsolationTest : public QObject
{
    Q_OBJECT

private slots:
    void workerPingSucceeds();
    void workerOpenReturnsArtifactIdentity();
    void workerPreflightRunsIsolated();
    void crashingWorkerDoesNotKillSupervisor();
    void workerSourcesOmitSentry();
};

namespace
{

struct ToolRun
{
    int exitCode = -1;
    QByteArray stdoutData;
    QByteArray stderrData;
    QJsonObject json;
};

ToolRun runPdfTool(const QStringList& arguments, const QProcessEnvironment& extra = {})
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    const QStringList keys = extra.keys();
    for (const QString& key : keys)
    {
        environment.insert(key, extra.value(key));
    }
    process.setProcessEnvironment(environment);
    process.setProgram(QStringLiteral(PDFTOOL_EXECUTABLE_PATH));
    process.setArguments(arguments);
    process.start();

    ToolRun run;
    if (!test_support::waitForFinishedAndCapture(process, 120000, run.stdoutData, run.stderrData))
    {
        run.stderrData.append(process.errorString().toUtf8());
        return run;
    }
    run.exitCode = process.exitCode();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(run.stdoutData, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject())
    {
        run.json = document.object();
    }
    return run;
}

QString fixturePdf()
{
    return QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/font-embedded.pdf"));
}

QString defaultProfile()
{
    return QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
}

}   // namespace

void PdfWorkerIsolationTest::workerPingSucceeds()
{
#ifndef Q_OS_LINUX
    QSKIP("Release-worker sandbox proof is Linux-first for #618.");
#endif
    const ToolRun run = runPdfTool({ QStringLiteral("worker-ping"), QStringLiteral("--console-format"), QStringLiteral("json") });
    QCOMPARE(run.exitCode, 0);
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    const QJsonObject data = run.json.value(QStringLiteral("data")).toObject();
    QVERIFY(data.value(QStringLiteral("ok")).toBool());
    const QJsonObject sandbox = data.value(QStringLiteral("sandbox")).toObject();
    QVERIFY(sandbox.value(QStringLiteral("applied")).toBool());
}

void PdfWorkerIsolationTest::workerOpenReturnsArtifactIdentity()
{
#ifndef Q_OS_LINUX
    QSKIP("Release-worker sandbox proof is Linux-first for #618.");
#endif
    const QString pdf = fixturePdf();
    QVERIFY(QFileInfo::exists(pdf));
    const ToolRun run = runPdfTool({ QStringLiteral("worker-open"), pdf, QStringLiteral("--console-format"), QStringLiteral("json") });
    QCOMPARE(run.exitCode, 0);
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    const QJsonObject data = run.json.value(QStringLiteral("data")).toObject();
    const QJsonObject artifact = data.value(QStringLiteral("artifact")).toObject();
    QCOMPARE(artifact.value(QStringLiteral("sha256")).toString().size(), 64);
    QVERIFY(data.value(QStringLiteral("page_count")).toInt() > 0);
}

void PdfWorkerIsolationTest::workerPreflightRunsIsolated()
{
#ifndef Q_OS_LINUX
    QSKIP("Release-worker sandbox proof is Linux-first for #618.");
#endif
    const QString pdf = fixturePdf();
    const QString profile = defaultProfile();
    QVERIFY(QFileInfo::exists(pdf));
    QVERIFY(QFileInfo::exists(profile));
    const ToolRun run = runPdfTool({
        QStringLiteral("worker-preflight"),
        pdf,
        QStringLiteral("--profile"),
        profile,
        QStringLiteral("--console-format"),
        QStringLiteral("json"),
    });
    QVERIFY2(run.exitCode == 0 || run.exitCode == 1 || run.exitCode == 8,
             qPrintable(QString::fromUtf8(run.stdoutData) + QString::fromUtf8(run.stderrData)));
    const QString status = run.json.value(QStringLiteral("status")).toString();
    QVERIFY(status == QLatin1String("success") || status == QLatin1String("findings") ||
            status == QLatin1String("preflight-incomplete"));
    // Never a silent PASS when the worker is unavailable.
    QVERIFY(status != QLatin1String("unavailable") || run.exitCode != 0);
}

void PdfWorkerIsolationTest::crashingWorkerDoesNotKillSupervisor()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString crashWorker = temp.filePath(QStringLiteral("crash-worker.sh"));
    {
        QFile file(crashWorker);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("#!/bin/sh\n# Hostile stand-in for loop-pdf-worker.\nkill -SEGV $$\n");
        file.close();
    }
    QVERIFY(QFile::setPermissions(crashWorker,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                      QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                      QFileDevice::ExeOther));

    QProcessEnvironment extra;
    extra.insert(QStringLiteral("LOOP_PDF_WORKER_PATH"), crashWorker);
    const ToolRun run = runPdfTool(
        { QStringLiteral("worker-open"), fixturePdf(), QStringLiteral("--console-format"), QStringLiteral("json") },
        extra);

    // Supervisor must exit normally with a typed failure — never crash itself.
    QVERIFY(run.exitCode != 0);
    QCOMPARE(run.json.value(QStringLiteral("command")).toString(), QStringLiteral("worker-open"));
    const QString status = run.json.value(QStringLiteral("status")).toString();
    QVERIFY(status == QLatin1String("processing-failure") || status == QLatin1String("preflight-incomplete"));
    const QJsonObject data = run.json.value(QStringLiteral("data")).toObject();
    const QString code = data.value(QStringLiteral("code")).toString(data.value(QStringLiteral("worker_code")).toString());
    QVERIFY(code.contains(QStringLiteral("worker")));
}

void PdfWorkerIsolationTest::workerSourcesOmitSentry()
{
    const QStringList sources = {
        QStringLiteral(LOOP_SOURCE_DIR "/PdfTool/loop-pdf-worker-main.cpp"),
        QStringLiteral(LOOP_SOURCE_DIR "/PdfTool/pdfworkerruntime.cpp"),
        QStringLiteral(LOOP_SOURCE_DIR "/PdfTool/pdfworkersandbox.cpp"),
    };
    for (const QString& path : sources)
    {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        const QByteArray text = file.readAll();
        QVERIFY2(!text.contains("pdfsentry.h"), qPrintable(path));
        QVERIFY2(!text.contains("PDFSentrySession"), qPrintable(path));
        QVERIFY2(!text.contains("sentry_init"), qPrintable(path));
        QVERIFY2(!text.contains("crashpad"), qPrintable(path));
    }
}

QTEST_MAIN(PdfWorkerIsolationTest)
#include "tst_pdfworkerisolation.moc"
