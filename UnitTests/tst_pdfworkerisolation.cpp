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

#ifdef Q_OS_WIN
#include <winsock2.h>
#endif
#include <QScopeGuard>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QCryptographicHash>
#include "pdfworkerclient.h"
#include "pdfworkerprocess.h"
#include <QUuid>
#include "pdfworkerprotocol.h"
#include "pdfpreflightverdict.h"
#include <future>
#include <thread>
#include <chrono>

class PdfWorkerIsolationTest : public QObject
{
    Q_OBJECT

private slots:
    void workerPingSucceeds();
    void workerOpenReturnsArtifactIdentity();
    void workerPreflightRunsIsolated();
    void crashingWorkerDoesNotKillSupervisor();
    void workerSourcesOmitSentry();
    void supervisorFaults_data();
    void supervisorFaults();
    void supervisorCancellation();
    void oversizedRequestDoesNotPublish();
    void sandboxDenials();
    void workerExtrasAreNotPublished();
    void malformedPdfDoesNotLeak();
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
    const auto data = run.json.value(QStringLiteral("data")).toObject();
    pdf::PreflightInspectionReceipt receipt;
    QString error;
    QVERIFY2(pdf::preflightInspectionReceiptFromJson(data.value(QStringLiteral("receipt")).toObject(), receipt, error), qPrintable(error));
    QFile input(pdf);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QCOMPARE(receipt.inputDigest, QString::fromLatin1(QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256).toHex()));
    QVERIFY(!receipt.checks.isEmpty());
    QCOMPARE(run.exitCode, pdf::preflightVerdictProcessExitCode(receipt.verdict.state));
}

void PdfWorkerIsolationTest::crashingWorkerDoesNotKillSupervisor()
{
    QTemporaryDir temp, output;
    pdftool::PdfWorkerClient client;
    QString error;
    QVERIFY2(client.start(QStringLiteral(PDFWORKER_PROBE_PATH), temp.path(), temp.path(), output.path(), &error), qPrintable(error));
    const auto result = client.preflight(fixturePdf(), defaultProfile(), output.path(), QStringLiteral("crash"));
    QCOMPARE(result.outcome, pdftool::WorkerClientOutcome::Incomplete);
    QVERIFY(!client.isRunning());
    QCOMPARE(result.response.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("incomplete"));
    QVERIFY2(client.replaceWorker(&error), qPrintable(error));
    QCOMPARE(client.ping().outcome, pdftool::WorkerClientOutcome::Success);
}

void PdfWorkerIsolationTest::supervisorFaults_data()
{
    QTest::addColumn<QString>("mode");
    for (const QString& mode : { QStringLiteral("hang"), QStringLiteral("memory"), QStringLiteral("malformed"),
                                 QStringLiteral("oversized"), QStringLiteral("wrong-id"), QStringLiteral("wrong-op"),
                                 QStringLiteral("wrong-version"), QStringLiteral("wrong-status"), QStringLiteral("raw-error"),
                                 QStringLiteral("cpu") })
    {
        QTest::newRow(qPrintable(mode)) << mode;
    }
}

void PdfWorkerIsolationTest::supervisorFaults()
{
    QFETCH(QString, mode);
    QTemporaryDir temp, output;
    pdftool::PdfWorkerClient client;
    QString error;
    QVERIFY2(client.start(QStringLiteral(PDFWORKER_PROBE_PATH), temp.path(), temp.path(), output.path(), &error), qPrintable(error));
    const int timeout = mode == QLatin1String("cpu") ? 135000 : mode == QLatin1String("hang") ? 150
                                                                                              : 20000;
    const auto result = client.preflight(fixturePdf(), defaultProfile(), output.path(), mode, false, timeout);
    QCOMPARE(result.outcome, pdftool::WorkerClientOutcome::Incomplete);
    const auto receipt = result.response.value(QStringLiteral("receipt")).toObject();
    QCOMPARE(receipt.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("incomplete"));
    QCOMPARE(receipt.value(QStringLiteral("fidelity")).toString(), QStringLiteral("not-recorded"));
    QVERIFY(!QJsonDocument(result.response).toJson().contains("LOOP_CUSTOMER_SECRET_20"));
    QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
    if (mode == QLatin1String("cpu") || mode == QLatin1String("memory"))
    {
        QCOMPARE(result.code, QStringLiteral("worker.exited"));
    }
    QVERIFY2(client.replaceWorker(&error), qPrintable(error));
    QCOMPARE(client.ping().outcome, pdftool::WorkerClientOutcome::Success);
}

void PdfWorkerIsolationTest::oversizedRequestDoesNotPublish()
{
    QTemporaryDir temp, output;
    pdftool::PdfWorkerClient client;
    QString error;
    QVERIFY2(client.start(QStringLiteral(PDFWORKER_EXECUTABLE_PATH), temp.path(), temp.path(), output.path(), &error), qPrintable(error));
    const auto result = client.preflight(fixturePdf(), defaultProfile(), output.path(),
                                         QString(pdftool::worker::MAX_REQUEST_BYTES, QLatin1Char('x')));
    QCOMPARE(result.outcome, pdftool::WorkerClientOutcome::Incomplete);
    QCOMPARE(result.code, QStringLiteral("worker.request-limit"));
    pdf::PreflightInspectionReceipt receipt;
    QVERIFY(pdf::preflightInspectionReceiptFromJson(result.response.value(QStringLiteral("receipt")).toObject(), receipt, error));
    QVERIFY(!receipt.verdict.isPass());
    QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
    QVERIFY2(client.replaceWorker(&error), qPrintable(error));
    QCOMPARE(client.ping().outcome, pdftool::WorkerClientOutcome::Success);
}

void PdfWorkerIsolationTest::supervisorCancellation()
{
    std::promise<pdftool::PdfWorkerClient*> ready;
    auto clientFuture = ready.get_future();
    auto result = std::async(std::launch::async, [&]
                             {
        QTemporaryDir temp, output;
        pdftool::PdfWorkerClient client;
        QString error;
        if (!client.start(QStringLiteral(PDFWORKER_PROBE_PATH), temp.path(), temp.path(), output.path(), &error))
        {
            ready.set_value(nullptr);
            return pdftool::WorkerClientResult{};
        }
        ready.set_value(&client);
        return client.preflight(fixturePdf(), defaultProfile(), output.path(), QStringLiteral("hang"), false, 60000); });
    auto* client = clientFuture.get();
    QVERIFY(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client->cancel();
    const auto cancelled = result.get();
    QCOMPARE(cancelled.outcome, pdftool::WorkerClientOutcome::Cancelled);
    QCOMPARE(cancelled.response.value(QStringLiteral("receipt")).toObject().value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("incomplete"));
}

void PdfWorkerIsolationTest::sandboxDenials()
{
    QTemporaryDir temp, output, outside;
    quint16 port = 9;
#ifdef Q_OS_WIN
    WSADATA winsock{};
    QCOMPARE(WSAStartup(MAKEWORD(2, 2), &winsock), 0);
    const auto cleanupWinsock = qScopeGuard([]
                                            { WSACleanup(); });
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    QVERIFY(listener != INVALID_SOCKET);
    const auto cleanupListener = qScopeGuard([&]
                                             { closesocket(listener); });
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    QCOMPARE(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    QCOMPARE(listen(listener, 1), 0);
    int addressSize = sizeof(address);
    QCOMPARE(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize), 0);
    port = ntohs(address.sin_port);
    const SOCKET control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    QVERIFY(control != INVALID_SOCKET);
    const auto cleanupControl = qScopeGuard([&]
                                            { closesocket(control); });
    QCOMPARE(::connect(control, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    const SOCKET accepted = accept(listener, nullptr, nullptr);
    QVERIFY(accepted != INVALID_SOCKET);
    closesocket(accepted);
#endif
    QFile secret(outside.filePath(QStringLiteral("secret.txt")));
    QVERIFY(secret.open(QIODevice::WriteOnly));
    secret.write("LOOP_CUSTOMER_SECRET_20");
    secret.close();
    QTemporaryDir inputDirectory;
    const QString snapshot = inputDirectory.filePath(QStringLiteral("input.pdf"));
    QVERIFY(QFile::copy(fixturePdf(), snapshot));
    QVERIFY(QFile::setPermissions(snapshot, QFileDevice::ReadOwner));
    const auto cleanupSnapshot = qScopeGuard([&]
                                             { QFile::setPermissions(snapshot, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
    pdftool::WorkerProcess process;
    QString error;
    QVERIFY2(process.start(QStringLiteral(PDFWORKER_PROBE_PATH),
                           { QStringLiteral("--sandbox-input"), inputDirectory.path(), QStringLiteral("--sandbox-temp"), temp.path(),
                             QStringLiteral("--sandbox-output"), output.path() },
                           inputDirectory.path(), temp.path(), output.path(), error),
             qPrintable(error));
    QElapsedTimer timer;
    timer.start();
    const std::atomic_bool cancelled{ false };
    const QJsonObject request{ { QStringLiteral("v"), pdftool::worker::PROTOCOL_VERSION },
                               { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                               { QStringLiteral("op"), QStringLiteral("open") },
                               { QStringLiteral("input_path"), snapshot },
                               { QStringLiteral("password"), QStringLiteral("probe:%1:").arg(port) + secret.fileName() } };
    QVERIFY(process.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n', timer, 10000, cancelled));
    QByteArray frame;
    while (!frame.contains('\n') && timer.elapsed() < 10000 && frame.size() <= pdftool::worker::MAX_RESPONSE_BYTES)
    {
        frame.append(process.read(pdftool::worker::MAX_RESPONSE_BYTES + 1 - frame.size()));
        QTest::qWait(10);
    }
    QVERIFY(frame.size() <= pdftool::worker::MAX_RESPONSE_BYTES);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(frame, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    const QJsonObject response = document.object();
    QCOMPARE(response.value(QStringLiteral("id")), request.value(QStringLiteral("id")));
    QVERIFY(response.value(QStringLiteral("ok")).toBool());
    const auto probe = response.value(QStringLiteral("probe")).toObject();
    for (const QString& key : { QStringLiteral("outside_denied"), QStringLiteral("network_denied"), QStringLiteral("child_denied"),
                                QStringLiteral("runtime_denied"), QStringLiteral("snapshot_denied"), QStringLiteral("profile_denied"), QStringLiteral("temp_allowed") })
    {
        QVERIFY2(probe.value(key) == QJsonValue(true), qPrintable(key + QString::fromUtf8(QJsonDocument(probe).toJson(QJsonDocument::Compact))));
    }
#ifdef Q_OS_WIN
    const QString runtimePath = response.value(QStringLiteral("runtime_path")).toString();
    const QString profilePath = response.value(QStringLiteral("profile_path")).toString();
    QVERIFY(QFileInfo(runtimePath).isDir());
    QVERIFY2(QFileInfo(profilePath).isDir(), qPrintable(profilePath));
    process.stop();
    QVERIFY(!QFileInfo::exists(runtimePath));
    QVERIFY(!QFileInfo::exists(profilePath));
#endif
}

void PdfWorkerIsolationTest::workerExtrasAreNotPublished()
{
    QTemporaryDir temp, output;
    pdftool::PdfWorkerClient client;
    QString error;
    QVERIFY2(client.start(QStringLiteral(PDFWORKER_PROBE_PATH), temp.path(), temp.path(), output.path(), &error), qPrintable(error));
    const auto result = client.openDocument(fixturePdf(), QStringLiteral("raw-success"));
    QCOMPARE(result.outcome, pdftool::WorkerClientOutcome::Success);
    QVERIFY(!result.response.contains(QStringLiteral("verdict")));
    QVERIFY(!result.response.contains(QStringLiteral("report_path")));
    QVERIFY(!QJsonDocument(result.response).toJson().contains("LOOP_CUSTOMER_SECRET_20"));
    QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
    QCOMPARE(client.ping().outcome, pdftool::WorkerClientOutcome::Success);
}

void PdfWorkerIsolationTest::malformedPdfDoesNotLeak()
{
    QTemporaryDir fixture;
    QFile malformed(fixture.filePath(QStringLiteral("malformed.pdf")));
    QVERIFY(malformed.open(QIODevice::WriteOnly));
    malformed.write("%PDF-1.7\nLOOP_CUSTOMER_SECRET_20\ninvalid-object\n%%EOF");
    malformed.close();
    QProcessEnvironment environment;
    const QString logDirectory = fixture.filePath(QStringLiteral("logs"));
    environment.insert(QStringLiteral("LOOP_LOG_DIR"), logDirectory);
    environment.insert(QStringLiteral("LOOP_LOG_LEVEL"), QStringLiteral("debug"));
    environment.insert(QStringLiteral("SENTRY_DSN"), QStringLiteral("https://public@127.0.0.1:1/1"));
    environment.insert(QStringLiteral("SENTRY_DEBUG"), QStringLiteral("1"));
    const auto run = runPdfTool({ QStringLiteral("worker-preflight"), malformed.fileName(), QStringLiteral("--profile"),
                                  defaultProfile(), QStringLiteral("--console-format"), QStringLiteral("json") },
                                environment);
    QVERIFY(!QFileInfo::exists(logDirectory));
    QVERIFY(run.exitCode != 0);
    QVERIFY(!run.stdoutData.contains("LOOP_CUSTOMER_SECRET_20"));
    QVERIFY(!run.stderrData.contains("LOOP_CUSTOMER_SECRET_20"));
    const auto receipt = run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("receipt")).toObject();
    QCOMPARE(receipt.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("incomplete"));
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
