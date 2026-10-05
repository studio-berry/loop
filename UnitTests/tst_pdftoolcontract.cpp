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

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include "pdfoperationhistorystore.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPair>
#include <QTemporaryDir>
#include <QTest>
#include <QVector>

#include <algorithm>

namespace
{

struct ToolRun
{
    int exitCode = -1;
    QByteArray stdoutData;
    QByteArray stderrData;
    QJsonObject json;
};

ToolRun runPdfTool(const QStringList& arguments)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    process.setProgram(QStringLiteral(PDFTOOL_EXECUTABLE_PATH));
    process.setArguments(arguments);
    process.start();

    ToolRun run;
    if (!test_support::waitForFinishedAndCapture(process, 120000, run.stdoutData, run.stderrData))
    {
        if (!run.stderrData.isEmpty())
        {
            run.stderrData.append('\n');
        }
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

void verifyEnvelope(const ToolRun& run, int expectedExitCode, const QString& command)
{
    QCOMPARE(run.exitCode, expectedExitCode);
    QVERIFY2(!run.json.isEmpty(), qPrintable(QStringLiteral("stdout was not one JSON object: %1").arg(QString::fromUtf8(run.stdoutData))));
    QCOMPARE(run.json.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(run.json.value(QStringLiteral("command")).toString(), command);
    QVERIFY(!run.json.value(QStringLiteral("version")).toString().isEmpty());
    QCOMPARE(run.json.value(QStringLiteral("exit_code")).toInt(), expectedExitCode);
    QVERIFY(!run.json.value(QStringLiteral("diagnostics")).isNull());
    QVERIFY(!run.json.value(QStringLiteral("outputs")).isNull());
    QVERIFY(run.json.value(QStringLiteral("data")).isObject());
    QVERIFY2(run.stderrData.isEmpty(), qPrintable(QStringLiteral("JSON mode wrote stderr: %1").arg(QString::fromUtf8(run.stderrData))));
}

class PdfToolContractTest : public QObject
{
    Q_OBJECT

private slots:
    void helpIsWrapped();
    void equalsFormIsDetected();
    void capabilitiesIsWrapped();
    void capabilitiesCanFilterCommand();
    void capabilitiesRejectUnknownCommand();
    void capabilitiesAreDeterministicallySorted();
    void capabilitiesExposeSensitiveOptionMetadata();
    void unknownCommandIsInvalidInvocation();
    void malformedInvocationIsWrapped();
    void defaultPreflightMalformedInvocationIsWrapped();
    void fetchImagesOnVectorOnlyDocumentNotesEmptyResult();
    void fetchImagesFailIfEmptyIsFindings();
    void fetchTextFailIfEmptyKeepsSuccessWhenTextExists();
    void preflightRejectsNonJsonOutput();
    void preflightKeepsNestedReportBoundary();
    void preflightPageSelectorsNarrowReportScope();
    void preflightRestrictedAuditBindsEffectiveScope();
    void preflightWritesTheCanonicalReport();
    void preflightRejectsReportFileAliasingInput();
    void addBleedDoesNotAdvertiseReportFile();
    void schemaRejectsNonJsonOutput();
    void schemaReportsTheMatrixForEveryKind();
    void schemaReportsUnsupportedMajorIdenticallyToCore();
    void schemaReportsUnreadyForAnUnusableVersion();
    void schemaAcceptsCurrentAndPreviousGoldens();
    void capabilitiesReportMatrixVersions();
    void redactRefusesToWriteOverItsOwnInput();
    void addBleedRefusesToWriteOverItsOwnInput();
    void rgbToCmykRefusesToWriteOverItsOwnInput();
    void repairRefusesToWriteOverItsOwnInput();
    void actionListBatchReportsRefusedOutputAsFailed();
    void repairRefusesRepeatedParameterAssignment();
    void repairRefusesStaleApprovalBeforeWrite();
    void repairPublicationBindsCompleteEventIdentities();
    void evidenceBundleExportVerifyPair();
    void evidenceBundleRejectsNonJsonOutput();
    void benchmarkWithoutPreflightProfileIsIncomplete();
    void benchmarkWithPreflightProfileIsComplete();
    void rollbackRejectsNonJsonOutput();
    void rollbackAdvertisesJsonOnlyGovernedSurface();
    void rollbackRestoresRecordedRevisionWithGovernedReceipt();
};

namespace
{

QJsonObject runBenchmarkEnvelope(const QStringList& extraArguments)
{
    const QString fixture = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/image-dpi-low.pdf");
    QStringList arguments{ QStringLiteral("benchmark"), fixture,
                           QStringLiteral("--render-hw-accel"), QStringLiteral("0"),
                           QStringLiteral("--console-format"), QStringLiteral("json") };
    arguments << extraArguments;
    const ToolRun run = runPdfTool(arguments);
    verifyEnvelope(run, 0, QStringLiteral("benchmark"));
    return run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("workload_envelope")).toObject();
}

}   // namespace

void PdfToolContractTest::benchmarkWithoutPreflightProfileIsIncomplete()
{
    const QJsonObject envelope = runBenchmarkEnvelope({});
    QVERIFY(!envelope.isEmpty());
    QCOMPARE(envelope.value(QStringLiteral("status")).toString(), QStringLiteral("incomplete"));
    QCOMPARE(envelope.value(QStringLiteral("incomplete_reason")).toString(), QStringLiteral("preflight-measurement-unavailable"));
    QCOMPARE(envelope.value(QStringLiteral("preflight_high_water_bytes")).toInteger(), -1);
}

void PdfToolContractTest::benchmarkWithPreflightProfileIsComplete()
{
    const QString profile = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/profiles/loop-default.json");
    const QJsonObject envelope = runBenchmarkEnvelope({ QStringLiteral("--profile"), profile });
    QVERIFY(!envelope.isEmpty());
    QCOMPARE(envelope.value(QStringLiteral("status")).toString(), QStringLiteral("complete"));
    QVERIFY(envelope.value(QStringLiteral("incomplete_reason")).toString().isEmpty());
    const qint64 preflightHighWater = envelope.value(QStringLiteral("preflight_high_water_bytes")).toInteger();
    QVERIFY2(preflightHighWater > 0, qPrintable(QString::number(preflightHighWater)));
    QVERIFY(envelope.value(QStringLiteral("rss_high_water_bytes")).toInteger() >= preflightHighWater);
    QCOMPARE(envelope.value(QStringLiteral("pages_materialized")).toInteger(), envelope.value(QStringLiteral("page_count")).toInteger());
}

void PdfToolContractTest::helpIsWrapped()
{
    const ToolRun run = runPdfTool({ QStringLiteral("help"), QStringLiteral("--console-format"), QStringLiteral("json") });
    verifyEnvelope(run, 0, QStringLiteral("help"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
}

void PdfToolContractTest::equalsFormIsDetected()
{
    const ToolRun run = runPdfTool({ QStringLiteral("help"), QStringLiteral("--console-format=json") });
    verifyEnvelope(run, 0, QStringLiteral("help"));
}

void PdfToolContractTest::capabilitiesIsWrapped()
{
    const ToolRun run = runPdfTool({ QStringLiteral("capabilities") });
    verifyEnvelope(run, 0, QStringLiteral("capabilities"));

    const QJsonObject data = run.json.value(QStringLiteral("data")).toObject();
    QCOMPARE(data.value(QStringLiteral("discovery_schema_version")).toInt(), 1);
    QCOMPARE(data.value(QStringLiteral("product")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("PdfTool"));
    QVERIFY(!data.value(QStringLiteral("commands")).toArray().isEmpty());
    QVERIFY(!data.value(QStringLiteral("build_capabilities")).toArray().isEmpty());
    QVERIFY(!data.value(QStringLiteral("schemas")).toArray().isEmpty());
}

void PdfToolContractTest::capabilitiesCanFilterCommand()
{
    const ToolRun run = runPdfTool({ QStringLiteral("capabilities"), QStringLiteral("--command"), QStringLiteral("preflight") });
    verifyEnvelope(run, 0, QStringLiteral("capabilities"));

    const QJsonObject data = run.json.value(QStringLiteral("data")).toObject();
    const QJsonArray commands = data.value(QStringLiteral("commands")).toArray();
    QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("preflight"));
    QVERIFY(commands.first().toObject().value(QStringLiteral("options")).isArray());
    QVERIFY(commands.first().toObject().value(QStringLiteral("positionals")).isArray());
}

void PdfToolContractTest::capabilitiesRejectUnknownCommand()
{
    const ToolRun run = runPdfTool({ QStringLiteral("capabilities"), QStringLiteral("--command"), QStringLiteral("does-not-exist") });
    verifyEnvelope(run, 2, QStringLiteral("capabilities"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("invalid-invocation"));
    const QJsonArray diagnostics = run.json.value(QStringLiteral("diagnostics")).toArray();
    QVERIFY(!diagnostics.isEmpty());
    QCOMPARE(diagnostics.first().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("cli.unknown-discovery-command"));
}

void PdfToolContractTest::capabilitiesAreDeterministicallySorted()
{
    const ToolRun first = runPdfTool({ QStringLiteral("capabilities") });
    const ToolRun second = runPdfTool({ QStringLiteral("capabilities") });
    verifyEnvelope(first, 0, QStringLiteral("capabilities"));
    verifyEnvelope(second, 0, QStringLiteral("capabilities"));

    const QJsonArray firstCommands = first.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("commands")).toArray();
    const QJsonArray secondCommands = second.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("commands")).toArray();
    QVERIFY(firstCommands == secondCommands);
    for (int i = 1; i < firstCommands.size(); ++i)
    {
        QVERIFY(firstCommands.at(i - 1).toObject().value(QStringLiteral("id")).toString() < firstCommands.at(i).toObject().value(QStringLiteral("id")).toString());
    }
}

void PdfToolContractTest::capabilitiesExposeSensitiveOptionMetadata()
{
    const ToolRun run = runPdfTool({ QStringLiteral("capabilities"), QStringLiteral("--command"), QStringLiteral("info") });
    verifyEnvelope(run, 0, QStringLiteral("capabilities"));

    const QJsonArray options = run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("commands")).toArray().first().toObject().value(QStringLiteral("options")).toArray();
    bool foundPassword = false;
    for (const QJsonValue& value : options)
    {
        const QJsonObject option = value.toObject();
        if (option.value(QStringLiteral("id")).toString() == QStringLiteral("pswd"))
        {
            foundPassword = true;
            QVERIFY(option.value(QStringLiteral("sensitive")).toBool());
        }
    }
    QVERIFY(foundPassword);
}

void PdfToolContractTest::unknownCommandIsInvalidInvocation()
{
    const ToolRun run = runPdfTool({ QStringLiteral("frobnicate"), QStringLiteral("--console-format"), QStringLiteral("json") });
    verifyEnvelope(run, 2, QStringLiteral("frobnicate"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("invalid-invocation"));
    const QJsonArray diagnostics = run.json.value(QStringLiteral("diagnostics")).toArray();
    QVERIFY(!diagnostics.isEmpty());
    QCOMPARE(diagnostics.first().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("cli.unknown-command"));
}

void PdfToolContractTest::malformedInvocationIsWrapped()
{
    const ToolRun run = runPdfTool({ QStringLiteral("help"), QStringLiteral("--console-format"), QStringLiteral("json"), QStringLiteral("--not-an-option") });
    verifyEnvelope(run, 2, QStringLiteral("help"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("invalid-invocation"));
}

void PdfToolContractTest::defaultPreflightMalformedInvocationIsWrapped()
{
    const ToolRun run = runPdfTool({ QStringLiteral("preflight"), QStringLiteral("--profile") });
    verifyEnvelope(run, 2, QStringLiteral("preflight"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("invalid-invocation"));
}

namespace
{

QString textOnlyFixturePath()
{
    return QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/font-embedded.pdf"));
}

QJsonObject findDiagnostic(const ToolRun& run, const QString& code)
{
    for (const QJsonValue& value : run.json.value(QStringLiteral("diagnostics")).toArray())
    {
        const QJsonObject diagnostic = value.toObject();
        if (diagnostic.value(QStringLiteral("code")).toString() == code)
        {
            return diagnostic;
        }
    }

    return QJsonObject();
}

QByteArray fileDigest(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return QByteArray();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

}   // namespace

void PdfToolContractTest::fetchImagesOnVectorOnlyDocumentNotesEmptyResult()
{
    // A text-only document has no images to extract. That is a legitimate
    // answer, so the run still succeeds - but it must say so in a way a
    // machine consumer can see, instead of being indistinguishable from a
    // successful extraction of zero files.
    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());

    const ToolRun run = runPdfTool({ QStringLiteral("fetch-images"),
                                     textOnlyFixturePath(),
                                     QStringLiteral("--image-output-dir"), outputDirectory.path(),
                                     QStringLiteral("--console-format"), QStringLiteral("json") });

    verifyEnvelope(run, 0, QStringLiteral("fetch-images"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("success"));

    const QJsonObject diagnostic = findDiagnostic(run, QStringLiteral("output.empty-result"));
    QVERIFY2(!diagnostic.isEmpty(), "fetch-images produced no output.empty-result diagnostic");
    QCOMPARE(diagnostic.value(QStringLiteral("severity")).toString(), QStringLiteral("info"));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("fail_if_empty")).toBool(), false);
    QVERIFY(run.json.value(QStringLiteral("outputs")).toArray().isEmpty());
}

void PdfToolContractTest::fetchImagesFailIfEmptyIsFindings()
{
    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());

    const ToolRun run = runPdfTool({ QStringLiteral("fetch-images"),
                                     textOnlyFixturePath(),
                                     QStringLiteral("--image-output-dir"), outputDirectory.path(),
                                     QStringLiteral("--fail-if-empty"),
                                     QStringLiteral("--console-format"), QStringLiteral("json") });

    verifyEnvelope(run, 1, QStringLiteral("fetch-images"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("findings"));

    const QJsonObject diagnostic = findDiagnostic(run, QStringLiteral("output.empty-result"));
    QVERIFY2(!diagnostic.isEmpty(), "fetch-images produced no output.empty-result diagnostic");
    QCOMPARE(diagnostic.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("subject")).toString(), QStringLiteral("images"));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("fail_if_empty")).toBool(), true);
}

void PdfToolContractTest::fetchTextFailIfEmptyKeepsSuccessWhenTextExists()
{
    // The flag must not turn a document that does have text into a finding -
    // it only reports on the empty case.
    const ToolRun run = runPdfTool({ QStringLiteral("fetch-text"),
                                     textOnlyFixturePath(),
                                     QStringLiteral("--fail-if-empty"),
                                     QStringLiteral("--console-format"), QStringLiteral("json") });

    verifyEnvelope(run, 0, QStringLiteral("fetch-text"));
    QVERIFY(findDiagnostic(run, QStringLiteral("output.empty-result")).isEmpty());
}

void PdfToolContractTest::preflightRejectsNonJsonOutput()
{
    const ToolRun run = runPdfTool({ QStringLiteral("preflight"), QStringLiteral("--console-format"), QStringLiteral("text") });
    QCOMPARE(run.exitCode, 2);
    QVERIFY(run.json.isEmpty());
    QVERIFY2(!run.stderrData.isEmpty(), qPrintable(QStringLiteral("text-mode rejection did not write stderr")));
}

void PdfToolContractTest::preflightKeepsNestedReportBoundary()
{
    const ToolRun run = runPdfTool({ QStringLiteral("preflight"), QStringLiteral("--console-format"), QStringLiteral("json") });
    verifyEnvelope(run, 3, QStringLiteral("preflight"));
    QVERIFY(run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).isUndefined());
}

void PdfToolContractTest::preflightPageSelectorsNarrowReportScope()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString fixture = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/image-dpi-low.pdf");
    const QString pdfPath = temporary.filePath(QStringLiteral("artwork.pdf"));
    QVERIFY(QFile::copy(fixture, pdfPath));

    const QString profilePath = temporary.filePath(QStringLiteral("profile.json"));
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Scoped image preflight") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 300 } } } }
    };
    const QByteArray profileBytes = QJsonDocument(profile).toJson();
    QCOMPARE(profileFile.write(profileBytes), profileBytes.size());
    profileFile.close();

    const QStringList base{ QStringLiteral("preflight"), pdfPath,
                            QStringLiteral("--profile"), profilePath,
                            QStringLiteral("--console-format"), QStringLiteral("json") };
    QStringList first = base;
    first << QStringLiteral("--page-select") << QStringLiteral("1");
    const ToolRun inspected = runPdfTool(first);
    QVERIFY2(inspected.exitCode >= 0 && inspected.exitCode != 2, inspected.stderrData.constData());
    const QJsonObject inspectedReport = inspected.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QVERIFY(!inspectedReport.isEmpty());
    QCOMPARE(inspectedReport.value(QStringLiteral("coverage_scope")).toObject().value(QStringLiteral("cli_page_scope")).toObject().value(QStringLiteral("pages")).toArray(), QJsonArray({ 1 }));
    QCOMPARE(inspectedReport.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("scope_restrictions")).toObject().value(QStringLiteral("pages")).toArray(), QJsonArray({ 1 }));

    QStringList bounded = base;
    bounded << QStringLiteral("--page-first") << QStringLiteral("1")
            << QStringLiteral("--page-last") << QStringLiteral("1");
    const ToolRun withinRange = runPdfTool(bounded);
    QVERIFY(withinRange.exitCode != 2);
    const QJsonObject boundedReport = withinRange.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QCOMPARE(boundedReport.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("scope_restrictions")).toObject().value(QStringLiteral("pages")).toArray(), QJsonArray({ 1 }));

    QStringList disjoint = base;
    disjoint << QStringLiteral("--page-first") << QStringLiteral("2")
             << QStringLiteral("--page-select") << QStringLiteral("1");
    const ToolRun excluded = runPdfTool(disjoint);
    QVERIFY(excluded.exitCode != 0);
    const QJsonObject excludedReport = excluded.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QVERIFY(!excludedReport.value(QStringLiteral("pass")).toBool(true));
    QCOMPARE(excludedReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(),
             QStringLiteral("incomplete"));
    QCOMPARE(excludedReport.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("not_applicable"));

    QStringList malformed = base;
    malformed << QStringLiteral("--page-select") << QStringLiteral("1-nope");
    const ToolRun invalid = runPdfTool(malformed);
    QVERIFY(invalid.exitCode != 0);
    const QJsonObject invalidReport = invalid.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QCOMPARE(invalidReport.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
             QStringLiteral("unsupported-scope"));
}

void PdfToolContractTest::preflightRestrictedAuditBindsEffectiveScope()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString fixture = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/image-dpi-low.pdf");
    const QString pdfPath = temporary.filePath(QStringLiteral("artwork.pdf"));
    QVERIFY(QFile::copy(fixture, pdfPath));

    const QString profilePath = temporary.filePath(QStringLiteral("profile.json"));
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Restricted audit parity") },
        { QStringLiteral("restrictions"), QJsonObject{
                                              { QStringLiteral("pages"), QStringLiteral("1") },
                                              { QStringLiteral("object_classes"), QJsonArray{ QStringLiteral("image") } } } },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("image-resolution") }, { QStringLiteral("min_dpi"), 300 } } } }
    };
    const QByteArray profileBytes = QJsonDocument(profile).toJson();
    QCOMPARE(profileFile.write(profileBytes), profileBytes.size());
    profileFile.close();

    const ToolRun run = runPdfTool({ QStringLiteral("preflight"),
                                     pdfPath,
                                     QStringLiteral("--profile"),
                                     profilePath,
                                     QStringLiteral("--console-format"),
                                     QStringLiteral("json") });
    QVERIFY2(run.exitCode >= 0 && run.exitCode != 2, run.stderrData.constData());
    const QJsonObject report = run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QVERIFY(!report.isEmpty());
    const QJsonObject scopeRestrictions =
        report.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("scope_restrictions")).toObject();
    QCOMPARE(scopeRestrictions.value(QStringLiteral("pages")).toArray(), QJsonArray({ 1 }));
    QCOMPARE(scopeRestrictions.value(QStringLiteral("object_classes")).toArray(), QJsonArray{ QStringLiteral("image") });

    const QString historyPath =
        QDir(QFileInfo(pdfPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    const auto finishedIt = std::find_if(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                                         { return event.kind == pdf::PDFOperationHistoryEventKind::PreflightRun &&
                                                  event.status == pdf::PDFOperationHistoryStatus::Accepted; });
    QVERIFY(finishedIt != events.cend());
    QCOMPARE(finishedIt->effectiveProfileDigest, report.value(QStringLiteral("effective_profile_digest")).toString());
    QCOMPARE(finishedIt->resultSummary.value(QStringLiteral("coverage_scope")).toObject().value(QStringLiteral("scope_restrictions")).toObject(),
             scopeRestrictions);
}

void PdfToolContractTest::preflightWritesTheCanonicalReport()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString fixture = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/image-dpi-low.pdf");
    const QString pdfPath = temporary.filePath(QStringLiteral("artwork.pdf"));
    QVERIFY(QFile::copy(fixture, pdfPath));

    const QString profilePath = temporary.filePath(QStringLiteral("profile.json"));
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    const QJsonObject profile{
        { QStringLiteral("id"), QStringLiteral("report-file-contract") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Report file contract") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 1 } } } }
    };
    const QByteArray profileBytes = QJsonDocument(profile).toJson();
    QCOMPARE(profileFile.write(profileBytes), profileBytes.size());
    profileFile.close();

    const QString reportPath = temporary.filePath(QStringLiteral("preflight-report.json"));
    const ToolRun run = runPdfTool({ QStringLiteral("preflight"),
                                     pdfPath,
                                     QStringLiteral("--profile"),
                                     profilePath,
                                     QStringLiteral("--report-file"),
                                     reportPath,
                                     QStringLiteral("--console-format"),
                                     QStringLiteral("json") });
    verifyEnvelope(run, 0, QStringLiteral("preflight"));
    QVERIFY2(QFile::exists(reportPath), qPrintable(reportPath));

    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    const QJsonObject written = QJsonDocument::fromJson(reportFile.readAll()).object();
    QCOMPARE(written.value(QStringLiteral("document_revision_digest")).toString(),
             QString::fromLatin1(fileDigest(pdfPath).toHex()));
    QVERIFY(!written.value(QStringLiteral("effective_profile_digest")).toString().isEmpty());
    QVERIFY(!written.value(QStringLiteral("coverage_scope")).toObject().isEmpty());
    const QJsonObject envelopeReport =
        run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QCOMPARE(written.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(),
             envelopeReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString());

    // The written file must be the payload the audit chain retains rather than a
    // second rendering of the run: a certificate hashes the retained report, and
    // the evidence bundle exporter binds a report to that same chain payload.
    const QString historyPath =
        QDir(QFileInfo(pdfPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    const auto acceptedIt = std::find_if(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                                         { return event.kind == pdf::PDFOperationHistoryEventKind::PreflightRun &&
                                                  event.status == pdf::PDFOperationHistoryStatus::Accepted; });
    QVERIFY(acceptedIt != events.cend());
    QCOMPARE(pdf::canonicalJson(written), pdf::canonicalJson(acceptedIt->resultSummary));
}

void PdfToolContractTest::preflightRejectsReportFileAliasingInput()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString fixture = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/image-dpi-low.pdf");
    const QString pdfPath = temporary.filePath(QStringLiteral("artwork.pdf"));
    QVERIFY(QFile::copy(fixture, pdfPath));

    const QString profilePath = temporary.filePath(QStringLiteral("profile.json"));
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    const QJsonObject profile{
        { QStringLiteral("id"), QStringLiteral("report-file-alias-contract") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Report file alias contract") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 1 } } } }
    };
    const QByteArray profileBytes = QJsonDocument(profile).toJson();
    QCOMPARE(profileFile.write(profileBytes), profileBytes.size());
    profileFile.close();

    const QByteArray inputDigest = fileDigest(pdfPath);
    QVERIFY(!inputDigest.isEmpty());

    const ToolRun run = runPdfTool({ QStringLiteral("preflight"),
                                     pdfPath,
                                     QStringLiteral("--profile"),
                                     profilePath,
                                     QStringLiteral("--report-file"),
                                     pdfPath,
                                     QStringLiteral("--console-format"),
                                     QStringLiteral("json") });
    verifyEnvelope(run, 2, QStringLiteral("preflight"));
    const QJsonObject diagnostic = findDiagnostic(run, QStringLiteral("output.duplicate-planned-path"));
    QVERIFY2(!diagnostic.isEmpty(), qPrintable(QString::fromUtf8(run.stdoutData)));
    QCOMPARE(diagnostic.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("path")).toString(), pdfPath);
    QVERIFY(run.json.value(QStringLiteral("outputs")).toArray().isEmpty());
    QCOMPARE(fileDigest(pdfPath), inputDigest);
}

void PdfToolContractTest::addBleedDoesNotAdvertiseReportFile()
{
    const ToolRun capabilities = runPdfTool({ QStringLiteral("capabilities"),
                                              QStringLiteral("--command"),
                                              QStringLiteral("add-bleed"),
                                              QStringLiteral("--console-format"),
                                              QStringLiteral("json") });
    verifyEnvelope(capabilities, 0, QStringLiteral("capabilities"));

    const QJsonArray commands = capabilities.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("commands")).toArray();
    QVERIFY2(!commands.isEmpty(), qPrintable(QString::fromUtf8(capabilities.stdoutData)));
    const QJsonArray options = commands.first().toObject().value(QStringLiteral("options")).toArray();
    for (const QJsonValue& value : options)
    {
        QVERIFY(value.toObject().value(QStringLiteral("id")).toString() != QStringLiteral("report-file"));
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    const QString profilePath =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
    const ToolRun run = runPdfTool({ QStringLiteral("add-bleed"),
                                     QStringLiteral("--console-format"),
                                     QStringLiteral("json"),
                                     fixture,
                                     QStringLiteral("--output"),
                                     directory.filePath(QStringLiteral("candidate.pdf")),
                                     QStringLiteral("--profile"),
                                     profilePath,
                                     QStringLiteral("--report-file"),
                                     directory.filePath(QStringLiteral("report.json")) });
    verifyEnvelope(run, 2, QStringLiteral("add-bleed"));
    QVERIFY(!findDiagnostic(run, QStringLiteral("cli.invalid-arguments")).isEmpty());
}

void PdfToolContractTest::schemaRejectsNonJsonOutput()
{
    const ToolRun run = runPdfTool({ QStringLiteral("schema"), QStringLiteral("--console-format"), QStringLiteral("text") });
    QCOMPARE(run.exitCode, 2);
    QVERIFY(run.json.isEmpty());
    QVERIFY2(!run.stderrData.isEmpty(), qPrintable(QStringLiteral("text-mode rejection did not write stderr")));
}

void PdfToolContractTest::schemaReportsTheMatrixForEveryKind()
{
    const ToolRun run = runPdfTool({ QStringLiteral("schema") });
    verifyEnvelope(run, 0, QStringLiteral("schema"));

    const QJsonObject kinds = run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("matrix")).toObject().value(QStringLiteral("kinds")).toObject();
    QVERIFY2(!kinds.isEmpty(), qPrintable(QString::fromUtf8(run.stdoutData)));
    for (const QString& expected : { QStringLiteral("preflight-report"), QStringLiteral("preflight-profile"),
                                     QStringLiteral("evidence-graph"), QStringLiteral("operation-plan"),
                                     QStringLiteral("operation-result"), QStringLiteral("provenance-event"),
                                     QStringLiteral("certificate"), QStringLiteral("capability-discovery"),
                                     QStringLiteral("package-manifest") })
    {
        QVERIFY2(kinds.contains(expected), qPrintable(expected));
    }
    QCOMPARE(kinds.value(QStringLiteral("preflight-report")).toObject().value(QStringLiteral("current")).toString(),
             QStringLiteral("4.0"));
}

void PdfToolContractTest::schemaReportsUnsupportedMajorIdenticallyToCore()
{
    const QString fixture = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/schemas/unsupported-major.json");
    const ToolRun run = runPdfTool({ QStringLiteral("schema"), QStringLiteral("--input"), fixture });
    verifyEnvelope(run, 1, QStringLiteral("schema"));

    const QJsonObject data = run.json.value(QStringLiteral("data")).toObject();
    QCOMPARE(data.value(QStringLiteral("schema_kind")).toString(), QStringLiteral("preflight-report"));
    QCOMPARE(data.value(QStringLiteral("compatibility")).toString(), QStringLiteral("unsupported-major"));
    // These two strings are pinned verbatim in UnitTestsSchemaEvolution too. The
    // duplication is deliberate: a shared constant would let the Core message
    // change without any test noticing the CLI drifted from it.
    QCOMPARE(data.value(QStringLiteral("code")).toString(), QStringLiteral("schema.unsupported-major"));
    QCOMPARE(data.value(QStringLiteral("message")).toString(),
             QStringLiteral("Unsupported schema major: kind 'preflight-report' version 99; "
                            "this build supports major(s) 1, 2, 3, 4."));
    QCOMPARE(data.value(QStringLiteral("migration")).toObject().value(QStringLiteral("document_ready")).toBool(),
             false);
}

void PdfToolContractTest::schemaReportsUnreadyForAnUnusableVersion()
{
    // An artifact whose version cannot be read was never prepared: nothing
    // validated it, so it must not be advertised as a ready document just
    // because `prepareSchemaDocument` leaves the original bytes in place when
    // it aborts. The exit code alone does not catch this - the doc is
    // incompatible and exits 1 either way.
    QTemporaryDir artifactDirectory;
    QVERIFY(artifactDirectory.isValid());

    const QVector<QPair<QString, QByteArray>> artifacts{
        { QStringLiteral("malformed-version.json"),
          QByteArrayLiteral("{\"schema_kind\":\"preflight-report\",\"schema_version\":\"abc\"}") },
        { QStringLiteral("missing-version.json"), QByteArrayLiteral("{\"schema_kind\":\"preflight-report\"}") },
    };

    for (const auto& artifact : artifacts)
    {
        const QString path = artifactDirectory.filePath(artifact.first);
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
        QCOMPARE(file.write(artifact.second), qint64(artifact.second.size()));
        file.close();

        const ToolRun run = runPdfTool({ QStringLiteral("schema"), QStringLiteral("--input"), path });
        verifyEnvelope(run, 1, QStringLiteral("schema"));

        const QJsonObject data = run.json.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("schema_kind")).toString(), QStringLiteral("preflight-report"));
        QCOMPARE(data.value(QStringLiteral("compatibility")).toString(), QStringLiteral("invalid"));
        QCOMPARE(data.value(QStringLiteral("code")).toString(), QStringLiteral("schema.invalid-version"));
        QCOMPARE(data.value(QStringLiteral("migration")).toObject().value(QStringLiteral("document_ready")).toBool(),
                 false);
    }
}

void PdfToolContractTest::schemaAcceptsCurrentAndPreviousGoldens()
{
    const QString current = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/schemas/preflight-report-v4.json");
    const ToolRun currentRun = runPdfTool({ QStringLiteral("schema"), QStringLiteral("--input"), current });
    verifyEnvelope(currentRun, 0, QStringLiteral("schema"));
    const QJsonObject currentData = currentRun.json.value(QStringLiteral("data")).toObject();
    QCOMPARE(currentData.value(QStringLiteral("compatibility")).toString(), QStringLiteral("compatible"));
    QCOMPARE(currentData.value(QStringLiteral("migration")).toObject().value(QStringLiteral("applied")).toBool(),
             false);

    const QString previous = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/schemas/preflight-report-v3.json");
    const ToolRun previousRun = runPdfTool({ QStringLiteral("schema"), QStringLiteral("--input"), previous });
    verifyEnvelope(previousRun, 0, QStringLiteral("schema"));
    const QJsonObject migration = previousRun.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("migration")).toObject();
    QCOMPARE(migration.value(QStringLiteral("required")).toBool(), true);
    QCOMPARE(migration.value(QStringLiteral("applied")).toBool(), true);
    QCOMPARE(migration.value(QStringLiteral("from")).toString(), QStringLiteral("3.0"));
    QCOMPARE(migration.value(QStringLiteral("to")).toString(), QStringLiteral("4.0"));
}

void PdfToolContractTest::capabilitiesReportMatrixVersions()
{
    const ToolRun capabilities = runPdfTool({ QStringLiteral("capabilities") });
    verifyEnvelope(capabilities, 0, QStringLiteral("capabilities"));
    const QJsonArray schemas = capabilities.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("schemas")).toArray();
    QCOMPARE(schemas.size(), 4);

    const ToolRun matrixRun = runPdfTool({ QStringLiteral("schema") });
    verifyEnvelope(matrixRun, 0, QStringLiteral("schema"));
    const QJsonObject kinds = matrixRun.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("matrix")).toObject().value(QStringLiteral("kinds")).toObject();

    const QHash<QString, QString> publishedToKind{
        { QStringLiteral("loop-preflight-profile"), QStringLiteral("preflight-profile") },
        { QStringLiteral("loop-preflight-report"), QStringLiteral("preflight-report") },
        { QStringLiteral("pdftool-discovery"), QStringLiteral("capability-discovery") },
        { QStringLiteral("pdftool-envelope"), QStringLiteral("pdftool-envelope") },
    };

    QCOMPARE(schemas.size(), publishedToKind.size());
    for (const QJsonValue& schema : schemas)
    {
        const QJsonObject entry = schema.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        QVERIFY2(publishedToKind.contains(id), qPrintable(id));
        const QJsonObject matrixEntry = kinds.value(publishedToKind.value(id)).toObject();
        QVERIFY2(!matrixEntry.isEmpty(), qPrintable(id));
        const QString current = matrixEntry.value(QStringLiteral("current")).toString();
        QCOMPARE(entry.value(QStringLiteral("version")).toInt(), current.section(QLatin1Char('.'), 0, 0).toInt());
    }
}

void PdfToolContractTest::redactRefusesToWriteOverItsOwnInput()
{
    // Redaction removes prior content, so the command must refuse to persist
    // its result over the trusted input the caller handed it.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/color-rgb.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));

    const ToolRun run = runPdfTool({ QStringLiteral("redact"),
                                     QStringLiteral("--console-format"), QStringLiteral("json"),
                                     inputPath, inputPath });

    verifyEnvelope(run, 4, QStringLiteral("redact"));
    const QJsonObject diagnostic = findDiagnostic(run, QStringLiteral("save-policy.refused"));
    QVERIFY2(!diagnostic.isEmpty(), qPrintable(QString::fromUtf8(run.stdoutData)));
    QCOMPARE(diagnostic.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QVERIFY(diagnostic.value(QStringLiteral("message")).toString().contains(QStringLiteral("trusted input artifact")));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("path")).toString(), inputPath);
    QVERIFY(run.json.value(QStringLiteral("outputs")).toArray().isEmpty());
    QVERIFY(QFile(inputPath).exists());

    // The refusal must be about writing over the input, not about redaction:
    // the same document and the same caller still produce the artifact when
    // the output is a different path.
    const ToolRun legitimate = runPdfTool({ QStringLiteral("redact"),
                                            QStringLiteral("--console-format"), QStringLiteral("json"),
                                            inputPath, directory.filePath(QStringLiteral("redacted.pdf")) });
    QCOMPARE(legitimate.exitCode, 0);
    QVERIFY(findDiagnostic(legitimate, QStringLiteral("save-policy.refused")).isEmpty());
}

void PdfToolContractTest::addBleedRefusesToWriteOverItsOwnInput()
{
    // add-bleed declares saveAsNewArtifact("bleed correction must preserve the
    // trusted source"), so a corrective run may not hand the input back as its
    // own output.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    const QString profilePath =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));
    const QByteArray inputDigest = fileDigest(inputPath);
    QVERIFY(!inputDigest.isEmpty());

    const ToolRun run = runPdfTool({ QStringLiteral("add-bleed"),
                                     QStringLiteral("--console-format"), QStringLiteral("json"),
                                     QStringLiteral("--overwrite"),
                                     inputPath,
                                     QStringLiteral("--output"), inputPath });

    verifyEnvelope(run, 4, QStringLiteral("add-bleed"));
    const QJsonObject diagnostic = findDiagnostic(run, QStringLiteral("save-policy.refused"));
    QVERIFY2(!diagnostic.isEmpty(), qPrintable(QString::fromUtf8(run.stdoutData)));
    QCOMPARE(diagnostic.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QVERIFY(diagnostic.value(QStringLiteral("message")).toString().contains(QStringLiteral("trusted input artifact")));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("path")).toString(), inputPath);
    QVERIFY(run.json.value(QStringLiteral("outputs")).toArray().isEmpty());
    QCOMPARE(fileDigest(inputPath), inputDigest);

    // The refusal has to be about the destination, not about add-bleed: the
    // same document and the same caller still produce a candidate at a distinct
    // path.
    const QString candidatePath = directory.filePath(QStringLiteral("candidate.pdf"));
    const ToolRun legitimate = runPdfTool({ QStringLiteral("add-bleed"),
                                            QStringLiteral("--console-format"), QStringLiteral("json"),
                                            inputPath,
                                            QStringLiteral("--output"), candidatePath,
                                            QStringLiteral("--profile"), profilePath,
                                            QStringLiteral("--force") });
    QCOMPARE(legitimate.exitCode, 0);
    QVERIFY(findDiagnostic(legitimate, QStringLiteral("save-policy.refused")).isEmpty());
    QVERIFY(QFile(candidatePath).exists());
}

void PdfToolContractTest::rgbToCmykRefusesToWriteOverItsOwnInput()
{
    // rgb-to-cmyk declares saveAsNewArtifact("color conversion creates a
    // production candidate"), so the candidate may not replace the input even
    // when the caller asks for --overwrite.
    const QString profilePath = QFINDTESTDATA("testdata/synthetic-cmyk.icc");
    QVERIFY2(!profilePath.isEmpty() && QFile::exists(profilePath), qPrintable(profilePath));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/output-intent-rgb.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));
    const QByteArray inputDigest = fileDigest(inputPath);
    QVERIFY(!inputDigest.isEmpty());

    const ToolRun run = runPdfTool({ QStringLiteral("rgb-to-cmyk"),
                                     QStringLiteral("--console-format"), QStringLiteral("json"),
                                     QStringLiteral("--overwrite"),
                                     inputPath,
                                     QStringLiteral("--output"), inputPath,
                                     QStringLiteral("--target-profile"), profilePath });

    verifyEnvelope(run, 4, QStringLiteral("rgb-to-cmyk"));
    const QJsonObject diagnostic = findDiagnostic(run, QStringLiteral("save-policy.refused"));
    QVERIFY2(!diagnostic.isEmpty(), qPrintable(QString::fromUtf8(run.stdoutData)));
    QCOMPARE(diagnostic.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QVERIFY(diagnostic.value(QStringLiteral("message")).toString().contains(QStringLiteral("trusted input artifact")));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("path")).toString(), inputPath);
    QVERIFY(run.json.value(QStringLiteral("outputs")).toArray().isEmpty());
    QCOMPARE(fileDigest(inputPath), inputDigest);

    // The refusal has to be about the destination, not about the conversion:
    // the same document and the same caller still convert at a distinct path.
    const QString candidatePath = directory.filePath(QStringLiteral("candidate.pdf"));
    const ToolRun legitimate = runPdfTool({ QStringLiteral("rgb-to-cmyk"),
                                            QStringLiteral("--console-format"), QStringLiteral("json"),
                                            inputPath,
                                            QStringLiteral("--output"), candidatePath,
                                            QStringLiteral("--target-profile"), profilePath });
    QCOMPARE(legitimate.exitCode, 0);
    QVERIFY(findDiagnostic(legitimate, QStringLiteral("save-policy.refused")).isEmpty());
    QVERIFY(QFile(candidatePath).exists());
}

void PdfToolContractTest::repairRefusesToWriteOverItsOwnInput()
{
    // Repair never appends in place, so even with --overwrite the trusted
    // input may not come back as the repair's own output.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString profilePath =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));
    const QByteArray inputDigest = fileDigest(inputPath);
    QVERIFY(!inputDigest.isEmpty());

    const ToolRun refused = runPdfTool({ QStringLiteral("repair"),
                                         inputPath,
                                         QStringLiteral("--operation"), QStringLiteral("add-bleed"),
                                         QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
                                         QStringLiteral("--param"), QStringLiteral("mode=mirror"),
                                         QStringLiteral("--param"), QStringLiteral("force=true"),
                                         QStringLiteral("--profile"), profilePath,
                                         QStringLiteral("--overwrite"),
                                         QStringLiteral("--output"), inputPath,
                                         QStringLiteral("--console-format"), QStringLiteral("json") });

    verifyEnvelope(refused, 4, QStringLiteral("repair"));
    const QJsonObject diagnostic = findDiagnostic(refused, QStringLiteral("save-policy.refused"));
    QVERIFY2(!diagnostic.isEmpty(), qPrintable(QString::fromUtf8(refused.stdoutData)));
    QCOMPARE(diagnostic.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QVERIFY(diagnostic.value(QStringLiteral("message")).toString().contains(QStringLiteral("trusted input artifact")));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("path")).toString(), inputPath);
    QVERIFY(refused.json.value(QStringLiteral("outputs")).toArray().isEmpty());
    QCOMPARE(fileDigest(inputPath), inputDigest);

    // The refusal has to be about the destination, not about repair: the same
    // document and the same caller still publish at a distinct path.
    const QString candidatePath = directory.filePath(QStringLiteral("candidate.pdf"));
    const ToolRun legitimate = runPdfTool({ QStringLiteral("repair"),
                                            inputPath,
                                            QStringLiteral("--operation"), QStringLiteral("add-bleed"),
                                            QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
                                            QStringLiteral("--param"), QStringLiteral("mode=mirror"),
                                            QStringLiteral("--param"), QStringLiteral("force=true"),
                                            QStringLiteral("--profile"), profilePath,
                                            QStringLiteral("--output"), candidatePath,
                                            QStringLiteral("--console-format"), QStringLiteral("json") });
    QCOMPARE(legitimate.exitCode, 0);
    QVERIFY(findDiagnostic(legitimate, QStringLiteral("save-policy.refused")).isEmpty());
    QVERIFY(QFile::exists(candidatePath));
    QCOMPARE(fileDigest(inputPath), inputDigest);
}

void PdfToolContractTest::repairRefusesStaleApprovalBeforeWrite()
{
    // The gateway is the one decision point: an approval file whose plan digest does
    // not name the analyzed plan is refused before any destination write.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString profilePath =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));
    const QByteArray inputDigest = fileDigest(inputPath);
    QVERIFY(!inputDigest.isEmpty());

    const QString approvalPath = directory.filePath(QStringLiteral("approval.json"));
    {
        QFile approval(approvalPath);
        QVERIFY(approval.open(QIODevice::WriteOnly));
        const QJsonObject approvalObject{
            { QStringLiteral("schema"), QStringLiteral("loop.governed-approval") },
            { QStringLiteral("schema_version"), 1 },
            { QStringLiteral("plan_digest"), QString(64, QLatin1Char('d')) },
            { QStringLiteral("source_sha256"), QString::fromLatin1(inputDigest.toHex()) },
            { QStringLiteral("candidate_sha256"), QString(64, QLatin1Char('e')) },
            { QStringLiteral("approval"), QJsonObject{
                                              { QStringLiteral("kind"), QStringLiteral("human") },
                                              { QStringLiteral("actorId"), QStringLiteral("operator") },
                                              { QStringLiteral("decision"), QStringLiteral("approve") },
                                              { QStringLiteral("decidedUtc"), QStringLiteral("2026-01-01T00:00:00Z") } } }
        };
        QVERIFY(approval.write(QJsonDocument(approvalObject).toJson()) > 0);
        approval.close();
    }

    const QString outputPath = directory.filePath(QStringLiteral("stale-output.pdf"));
    const ToolRun refused = runPdfTool({ QStringLiteral("repair"),
                                         inputPath,
                                         QStringLiteral("--operation"), QStringLiteral("add-bleed"),
                                         QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
                                         QStringLiteral("--param"), QStringLiteral("mode=mirror"),
                                         QStringLiteral("--param"), QStringLiteral("force=true"),
                                         QStringLiteral("--profile"), profilePath,
                                         QStringLiteral("--approval-file"), approvalPath,
                                         QStringLiteral("--output"), outputPath,
                                         QStringLiteral("--console-format"), QStringLiteral("json") });

    verifyEnvelope(refused, 2, QStringLiteral("repair"));
    const QJsonObject diagnostic = findDiagnostic(refused, QStringLiteral("repair.approval-invalid"));
    QVERIFY2(!diagnostic.isEmpty(), qPrintable(QString::fromUtf8(refused.stdoutData)));
    QCOMPARE(diagnostic.value(QStringLiteral("context")).toObject().value(QStringLiteral("reason_code")).toString(),
             QStringLiteral("approval-stale"));
    QVERIFY(refused.json.value(QStringLiteral("outputs")).toArray().isEmpty());
    QVERIFY(!QFile::exists(outputPath));
    QCOMPARE(fileDigest(inputPath), inputDigest);
}

void PdfToolContractTest::actionListBatchReportsRefusedOutputAsFailed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString recipePath = directory.filePath(QStringLiteral("recipe.json"));
    const QString profilePath =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));
    const QByteArray inputDigest = fileDigest(inputPath);
    const QJsonObject recipe{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/1") },
        { QStringLiteral("id"), QStringLiteral("batch-save-refusal") },
        { QStringLiteral("name"), QStringLiteral("Batch save refusal") },
        { QStringLiteral("steps"), QJsonArray{ QJsonObject{
                                       { QStringLiteral("id"), QStringLiteral("bleed") },
                                       { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                       { QStringLiteral("params"), QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } } } } } }
    };
    QFile recipeFile(recipePath);
    QVERIFY(recipeFile.open(QIODevice::WriteOnly));
    const QByteArray recipeBytes = QJsonDocument(recipe).toJson();
    QCOMPARE(recipeFile.write(recipeBytes), qint64(recipeBytes.size()));
    recipeFile.close();

    const ToolRun refused = runPdfTool({ QStringLiteral("action-list"), QStringLiteral("batch"), recipePath, inputPath,
                                         QStringLiteral("--output-dir"), directory.path(),
                                         QStringLiteral("--profile"), profilePath,
                                         QStringLiteral("--overwrite"),
                                         QStringLiteral("--console-format"), QStringLiteral("json") });
    verifyEnvelope(refused, 4, QStringLiteral("action-list"));
    QVERIFY(!findDiagnostic(refused, QStringLiteral("save-policy.refused")).isEmpty());
    const QJsonArray items = refused.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("items")).toArray();
    QCOMPARE(items.size(), 1);
    const QJsonObject item = items.first().toObject();
    QCOMPARE(item.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    QVERIFY(item.value(QStringLiteral("error")).toString().contains(QStringLiteral("save policy")));
    QCOMPARE(fileDigest(inputPath), inputDigest);
}

void PdfToolContractTest::repairRefusesRepeatedParameterAssignment()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));

    const ToolRun refused = runPdfTool({ QStringLiteral("repair"),
                                         QStringLiteral("--console-format"), QStringLiteral("json"),
                                         QStringLiteral("--operation"), QStringLiteral("add-bleed"),
                                         QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
                                         QStringLiteral("--param"), QStringLiteral("bleed_mm=4"),
                                         QStringLiteral("--dry-run"),
                                         inputPath });
    verifyEnvelope(refused, 2, QStringLiteral("repair"));
    const QJsonObject diagnostic = findDiagnostic(refused, QStringLiteral("cli.invalid-arguments"));
    QVERIFY2(!diagnostic.isEmpty(), qPrintable(QString::fromUtf8(refused.stdoutData)));
    QVERIFY(diagnostic.value(QStringLiteral("message")).toString().contains(QStringLiteral("assigned more than once")));

    // The refusal has to be about the repeated key, not about the invocation:
    // the same command with distinct parameter keys still plans.
    const ToolRun planned = runPdfTool({ QStringLiteral("repair"),
                                         QStringLiteral("--console-format"), QStringLiteral("json"),
                                         QStringLiteral("--operation"), QStringLiteral("add-bleed"),
                                         QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
                                         QStringLiteral("--param"), QStringLiteral("force=true"),
                                         QStringLiteral("--dry-run"),
                                         inputPath });
    QCOMPARE(planned.exitCode, 0);
    QVERIFY(findDiagnostic(planned, QStringLiteral("cli.invalid-arguments")).isEmpty());
}

void PdfToolContractTest::evidenceBundleExportVerifyPair()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString fixture = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/image-dpi-low.pdf");
    const QString pdfPath = temporary.filePath(QStringLiteral("artwork.pdf"));
    QVERIFY(QFile::copy(fixture, pdfPath));

    // A certifiable run needs a non-provisional profile identity, so the profile
    // declares its id and version.
    const QString profilePath = temporary.filePath(QStringLiteral("profile.json"));
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    const QJsonObject profile{
        { QStringLiteral("id"), QStringLiteral("bundle-contract") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Evidence bundle contract") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 1 } } } }
    };
    const QByteArray profileBytes = QJsonDocument(profile).toJson();
    QCOMPARE(profileFile.write(profileBytes), profileBytes.size());
    profileFile.close();

    const QString certificatePath = temporary.filePath(QStringLiteral("certificate.json"));
    const ToolRun certified = runPdfTool({ QStringLiteral("preflight"),
                                           pdfPath,
                                           QStringLiteral("--profile"),
                                           profilePath,
                                           QStringLiteral("--certify"),
                                           certificatePath,
                                           QStringLiteral("--console-format"),
                                           QStringLiteral("json") });
    QVERIFY2(certified.exitCode == 0, certified.stdoutData.constData());
    verifyEnvelope(certified, 0, QStringLiteral("preflight"));
    QVERIFY(QFile::exists(certificatePath));

    const QString reportPath = temporary.filePath(QStringLiteral("report.json"));
    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::WriteOnly));
    const QByteArray reportBytes =
        QJsonDocument(certified.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject())
            .toJson(QJsonDocument::Indented);
    QCOMPARE(reportFile.write(reportBytes), reportBytes.size());
    reportFile.close();

    const QString bundleDirectory = temporary.filePath(QStringLiteral("bundle"));
    const ToolRun exported = runPdfTool({ QStringLiteral("export-evidence-bundle"),
                                          pdfPath,
                                          QStringLiteral("--report"),
                                          reportPath,
                                          QStringLiteral("--certificate"),
                                          certificatePath,
                                          QStringLiteral("--output"),
                                          bundleDirectory,
                                          QStringLiteral("--console-format"),
                                          QStringLiteral("json") });
    verifyEnvelope(exported, 0, QStringLiteral("export-evidence-bundle"));

    const QStringList members{ QStringLiteral("manifest.json"),
                               QStringLiteral("report.json"),
                               QStringLiteral("certificate.json"),
                               QStringLiteral("history.json"),
                               QStringLiteral("rollback-references.json") };
    for (const QString& member : members)
    {
        QVERIFY2(QFile::exists(QDir(bundleDirectory).filePath(member)), qPrintable(member));
    }

    const auto readMember = [&](const QString& name)
    {
        QFile file(QDir(bundleDirectory).filePath(name));
        if (!file.open(QIODevice::ReadOnly))
        {
            return QJsonObject();
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    };

    const ToolRun verified = runPdfTool({ QStringLiteral("verify-evidence-bundle"),
                                          bundleDirectory,
                                          QStringLiteral("--console-format"),
                                          QStringLiteral("json") });
    verifyEnvelope(verified, 0, QStringLiteral("verify-evidence-bundle"));
    const QJsonObject verification =
        verified.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("verification")).toObject();
    QVERIFY2(verification.value(QStringLiteral("valid")).toBool(), qPrintable(QString::fromUtf8(verified.stdoutData)));
    QCOMPARE(verification.value(QStringLiteral("members_checked")).toInt(), members.size() - 1);

    // The manifest, not the report, is the entry point for a reader without
    // Loop: the effective profile, the coverage scope and the revision digest
    // must be inside it, and it must never claim canonical authority.
    const QJsonObject manifest = readMember(QStringLiteral("manifest.json"));
    QCOMPARE(manifest.value(QStringLiteral("schema")).toString(), QStringLiteral("loop.preflight-evidence-bundle"));
    QCOMPARE(manifest.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(manifest.value(QStringLiteral("authority")).toObject().value(QStringLiteral("canonical_state")).toString(),
             QStringLiteral("internal"));
    QCOMPARE(manifest.value(QStringLiteral("document")).toObject().value(QStringLiteral("revision_digest")).toString(),
             QString::fromLatin1(fileDigest(pdfPath).toHex()));
    QCOMPARE(manifest.value(QStringLiteral("document")).toObject().value(QStringLiteral("source_path_included")).toBool(true),
             false);
    QVERIFY(!manifest.value(QStringLiteral("effective_profile")).toObject().value(QStringLiteral("digest")).toString().isEmpty());
    QVERIFY(!manifest.value(QStringLiteral("coverage_scope")).toObject().isEmpty());
    QCOMPARE(manifest.value(QStringLiteral("members")).toArray().size(), members.size() - 1);
    QVERIFY(!manifest.value(QStringLiteral("certificate")).toObject().isEmpty());

    // Criterion: the bundle contains no raw file paths and no content outside
    // the declared set.
    const QStringList forbidden{ QFileInfo(pdfPath).absoluteFilePath(),
                                 QDir::toNativeSeparators(QFileInfo(pdfPath).absoluteFilePath()),
                                 QFileInfo(temporary.path()).absoluteFilePath(),
                                 QDir::toNativeSeparators(QFileInfo(temporary.path()).absoluteFilePath()),
                                 QStringLiteral("artwork.pdf") };
    for (const QString& member : members)
    {
        QFile file(QDir(bundleDirectory).filePath(member));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        for (const QString& needle : forbidden)
        {
            QVERIFY2(!content.contains(needle.toUtf8()),
                     qPrintable(QStringLiteral("bundle member '%1' carries '%2'").arg(member, needle)));
        }
    }

    // Tampering with any member is detected and attributed to that member. A
    // same-size edit is a digest mismatch; an added byte is a size mismatch.
    const QString reportMemberPath = QDir(bundleDirectory).filePath(QStringLiteral("report.json"));
    QFile reportMember(reportMemberPath);
    QVERIFY(reportMember.open(QIODevice::ReadOnly));
    QByteArray memberBytes = reportMember.readAll();
    reportMember.close();
    QVERIFY(memberBytes.size() > 8);
    memberBytes[memberBytes.size() / 2] = memberBytes.at(memberBytes.size() / 2) == 'x' ? 'y' : 'x';
    QVERIFY(reportMember.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(reportMember.write(memberBytes), memberBytes.size());
    reportMember.close();

    const ToolRun tampered = runPdfTool({ QStringLiteral("verify-evidence-bundle"),
                                          bundleDirectory,
                                          QStringLiteral("--console-format"),
                                          QStringLiteral("json") });
    verifyEnvelope(tampered, 1, QStringLiteral("verify-evidence-bundle"));
    const QJsonArray tamperedFindings =
        tampered.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("verification")).toObject().value(QStringLiteral("findings")).toArray();
    bool digestMismatchNamedReport = false;
    for (const QJsonValue& value : tamperedFindings)
    {
        const QJsonObject finding = value.toObject();
        if (finding.value(QStringLiteral("code")).toString() == QLatin1String("member.digest-mismatch") &&
            finding.value(QStringLiteral("member")).toString() == QLatin1String("report.json"))
        {
            digestMismatchNamedReport = true;
        }
    }
    QVERIFY2(digestMismatchNamedReport, qPrintable(QString::fromUtf8(tampered.stdoutData)));

    // A member the manifest does not declare is content outside the declared set.
    const QString undeclaredPath = QDir(bundleDirectory).filePath(QStringLiteral("extra.json"));
    QFile undeclared(undeclaredPath);
    QVERIFY(undeclared.open(QIODevice::WriteOnly));
    QCOMPARE(undeclared.write(QByteArrayLiteral("{}")), 2);
    undeclared.close();
    const ToolRun withUndeclared = runPdfTool({ QStringLiteral("verify-evidence-bundle"),
                                                bundleDirectory,
                                                QStringLiteral("--console-format"),
                                                QStringLiteral("json") });
    verifyEnvelope(withUndeclared, 1, QStringLiteral("verify-evidence-bundle"));
    bool undeclaredReported = false;
    for (const QJsonValue& value :
         withUndeclared.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("verification")).toObject().value(QStringLiteral("findings")).toArray())
    {
        const QJsonObject finding = value.toObject();
        if (finding.value(QStringLiteral("code")).toString() == QLatin1String("member.undeclared") &&
            finding.value(QStringLiteral("member")).toString() == QLatin1String("extra.json"))
        {
            undeclaredReported = true;
        }
    }
    QVERIFY2(undeclaredReported, qPrintable(QString::fromUtf8(withUndeclared.stdoutData)));
    QVERIFY(QFile::remove(undeclaredPath));

    // Offline means offline: a bundle exported before the inputs disappear still
    // verifies with no document, no sidecar, and no certificate present.
    const QString offlineBundle = temporary.filePath(QStringLiteral("offline-bundle"));
    const ToolRun reExported = runPdfTool({ QStringLiteral("export-evidence-bundle"),
                                            pdfPath,
                                            QStringLiteral("--report"),
                                            reportPath,
                                            QStringLiteral("--certificate"),
                                            certificatePath,
                                            QStringLiteral("--output"),
                                            offlineBundle,
                                            QStringLiteral("--console-format"),
                                            QStringLiteral("json") });
    verifyEnvelope(reExported, 0, QStringLiteral("export-evidence-bundle"));
    QVERIFY(QDir(temporary.filePath(QStringLiteral("artwork.pdf.loop-history"))).removeRecursively());
    QVERIFY(QFile::remove(pdfPath));
    QVERIFY(QFile::remove(certificatePath));
    QVERIFY(QFile::remove(reportPath));
    QVERIFY(!QFile::exists(pdfPath));
    QVERIFY(!QFile::exists(reportPath));

    const ToolRun offline = runPdfTool({ QStringLiteral("verify-evidence-bundle"),
                                         offlineBundle,
                                         QStringLiteral("--console-format"),
                                         QStringLiteral("json") });
    verifyEnvelope(offline, 0, QStringLiteral("verify-evidence-bundle"));
    QVERIFY2(offline.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("verification")).toObject().value(QStringLiteral("valid")).toBool(),
             qPrintable(QString::fromUtf8(offline.stdoutData)));
}

void PdfToolContractTest::evidenceBundleRejectsNonJsonOutput()
{
    const ToolRun exportRun = runPdfTool({ QStringLiteral("export-evidence-bundle"),
                                           QStringLiteral("--console-format"),
                                           QStringLiteral("text") });
    QCOMPARE(exportRun.exitCode, 2);
    QVERIFY(exportRun.json.isEmpty());
    QVERIFY2(!exportRun.stderrData.isEmpty(), qPrintable(QStringLiteral("text-mode rejection did not write stderr")));

    const ToolRun verifyRun = runPdfTool({ QStringLiteral("verify-evidence-bundle"),
                                           QStringLiteral("--console-format"),
                                           QStringLiteral("text") });
    QCOMPARE(verifyRun.exitCode, 2);
    QVERIFY(verifyRun.json.isEmpty());
    QVERIFY2(!verifyRun.stderrData.isEmpty(), qPrintable(QStringLiteral("text-mode rejection did not write stderr")));
}

}   // namespace

void PdfToolContractTest::repairPublicationBindsCompleteEventIdentities()
{
    // A repair through the one gateway appends a chain a reader can reconstruct:
    // every event binds the exact plan, approval, profile, revision, and output.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    const QString profilePath =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));

    const QString outputPath = directory.filePath(QStringLiteral("published.pdf"));
    const ToolRun run = runPdfTool({ QStringLiteral("repair"),
                                     inputPath,
                                     QStringLiteral("--operation"), QStringLiteral("add-bleed"),
                                     QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
                                     QStringLiteral("--param"), QStringLiteral("mode=mirror"),
                                     QStringLiteral("--param"), QStringLiteral("force=true"),
                                     QStringLiteral("--profile"), profilePath,
                                     QStringLiteral("--output"), outputPath,
                                     QStringLiteral("--console-format"), QStringLiteral("json") });
    QVERIFY2(run.exitCode == 0, qPrintable(QString::fromUtf8(run.stderrData)));
    QVERIFY(QFile::exists(outputPath));

    const QString historyPath =
        QDir(QFileInfo(outputPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    QVERIFY(history.verify().verified);
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));

    const pdf::PDFOperationHistoryEvent* running = nullptr;
    const pdf::PDFOperationHistoryEvent* accepted = nullptr;
    for (const pdf::PDFOperationHistoryEvent& event : events)
    {
        if (event.kind != pdf::PDFOperationHistoryEventKind::FixApplied)
        {
            continue;
        }
        if (event.status == pdf::PDFOperationHistoryStatus::Running)
        {
            running = &event;
        }
        else if (event.status == pdf::PDFOperationHistoryStatus::Accepted)
        {
            accepted = &event;
        }
    }
    QVERIFY(running != nullptr);
    QVERIFY(accepted != nullptr);
    // Identity completeness: no empty plan/approval/profile digests where the
    // acceptance requires them.
    QVERIFY(!running->documentRevisionDigest.isEmpty());
    QVERIFY(!running->effectiveProfileDigest.isEmpty());
    QVERIFY(running->approval.kind != pdf::PDFApprovalKind::None);
    QVERIFY(!running->operatorIdentity.isEmpty());
    QVERIFY(!accepted->effectiveProfileDigest.isEmpty());
    QVERIFY(!accepted->reportArtifactSha256.isEmpty());
    QVERIFY(accepted->output.has_value());

    const QJsonObject governedApproval = accepted->resultSummary.value(QStringLiteral("approval")).toObject();
    QVERIFY(pdf::isPDFSha256(governedApproval.value(QStringLiteral("plan_digest")).toString()));
    QVERIFY(!accepted->resultSummary.value(QStringLiteral("sign_off")).toObject().isEmpty());
    QVERIFY(!accepted->resultSummary.value(QStringLiteral("revalidation")).toObject().isEmpty());

    // The reader answers "who approved what output" for the published bytes.
    pdf::PDFGovernedPublicationAudit audit;
    const pdf::PDFOperationResult reconstructed =
        pdf::reconstructGovernedPublicationAudit(history, accepted->output->sha256, &audit);
    QVERIFY2(reconstructed, qPrintable(reconstructed.getErrorMessage()));
    QVERIFY(audit.reconstructed);
    QCOMPARE(audit.publishedSha256, accepted->output->sha256);
    QCOMPARE(audit.planDigest, governedApproval.value(QStringLiteral("plan_digest")).toString());
    QVERIFY(audit.approval.kind != pdf::PDFApprovalKind::None);
    QVERIFY(audit.signOff.has_value());
    QVERIFY(!audit.effectiveProfileDigest.isEmpty());
    QCOMPARE(audit.revalidationState, QStringLiteral("complete"));
}

void PdfToolContractTest::rollbackRejectsNonJsonOutput()
{
    const ToolRun run = runPdfTool({ QStringLiteral("rollback"), QStringLiteral("--console-format"), QStringLiteral("text") });
    QCOMPARE(run.exitCode, 2);
    QVERIFY(run.json.isEmpty());
    QVERIFY2(!run.stderrData.isEmpty(), qPrintable(QStringLiteral("text-mode rejection did not write stderr")));
}

void PdfToolContractTest::rollbackAdvertisesJsonOnlyGovernedSurface()
{
    const ToolRun run = runPdfTool({ QStringLiteral("capabilities"), QStringLiteral("--command"), QStringLiteral("rollback") });
    verifyEnvelope(run, 0, QStringLiteral("capabilities"));
    const QJsonArray commands = run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("commands")).toArray();
    QCOMPARE(commands.size(), 1);
    const QJsonObject command = commands.first().toObject();
    QCOMPARE(command.value(QStringLiteral("id")).toString(), QStringLiteral("rollback"));
    QCOMPARE(command.value(QStringLiteral("output_formats")).toArray(), QJsonArray{ QStringLiteral("json") });
    QVERIFY(command.value(QStringLiteral("capabilities")).toArray().contains(QStringLiteral("history.rollback")));
}

void PdfToolContractTest::rollbackRestoresRecordedRevisionWithGovernedReceipt()
{
    // The CLI rollback surface reaches the same governed publication as every other
    // surface: it restores a recorded revision as a new, revalidated revision.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString profilePath =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("profiles/loop-default.json"));
    const QString fixture =
        QDir(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR)).filePath(QStringLiteral("testdata/fixtures/bleed-missing.pdf"));
    const QString inputPath = directory.filePath(QStringLiteral("received.pdf"));
    QVERIFY2(QFile::copy(fixture, inputPath), qPrintable(fixture));

    const QString publishedPath = directory.filePath(QStringLiteral("published.pdf"));
    const ToolRun repair = runPdfTool({ QStringLiteral("repair"), inputPath,
                                        QStringLiteral("--operation"), QStringLiteral("add-bleed"),
                                        QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
                                        QStringLiteral("--param"), QStringLiteral("mode=mirror"),
                                        QStringLiteral("--param"), QStringLiteral("force=true"),
                                        QStringLiteral("--profile"), profilePath,
                                        QStringLiteral("--output"), publishedPath,
                                        QStringLiteral("--console-format"), QStringLiteral("json") });
    QVERIFY2(repair.exitCode == 0, qPrintable(QString::fromUtf8(repair.stderrData)));

    QFile published(publishedPath);
    QVERIFY(published.open(QIODevice::ReadOnly));
    const QByteArray publishedBytes = published.readAll();
    published.close();
    const QString publishedSha = QString::fromLatin1(QCryptographicHash::hash(publishedBytes, QCryptographicHash::Sha256).toHex());

    const QString restoredPath = directory.filePath(QStringLiteral("restored.pdf"));
    const QString reportPath = directory.filePath(QStringLiteral("rollback-report.json"));
    for (const QString& conflictingReport : { publishedPath, restoredPath, profilePath })
    {
        const ToolRun conflict = runPdfTool({ QStringLiteral("rollback"), publishedPath,
                                              QStringLiteral("--to"), publishedSha,
                                              QStringLiteral("--output"), restoredPath,
                                              QStringLiteral("--profile"), profilePath,
                                              QStringLiteral("--report-file"), conflictingReport });
        QVERIFY(conflict.exitCode != 0);
        QVERIFY(!QFile::exists(restoredPath));
        QFile unchanged(publishedPath);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), publishedBytes);
    }
    const ToolRun inPlace = runPdfTool({ QStringLiteral("rollback"), publishedPath,
                                         QStringLiteral("--to"), publishedSha,
                                         QStringLiteral("--output"), publishedPath,
                                         QStringLiteral("--profile"), profilePath });
    QVERIFY(inPlace.exitCode != 0);

    const ToolRun rollback = runPdfTool({ QStringLiteral("rollback"), publishedPath,
                                          QStringLiteral("--to"), publishedSha,
                                          QStringLiteral("--output"), restoredPath,
                                          QStringLiteral("--profile"), profilePath,
                                          QStringLiteral("--report-file"), reportPath,
                                          QStringLiteral("--reason"), QStringLiteral("contract rollback"),
                                          QStringLiteral("--console-format"), QStringLiteral("json") });
    QVERIFY2(rollback.exitCode == 0, qPrintable(QString::fromUtf8(rollback.stderrData)));
    verifyEnvelope(rollback, 0, QStringLiteral("rollback"));
    const QJsonObject data = rollback.json.value(QStringLiteral("data")).toObject();
    QCOMPARE(data.value(QStringLiteral("status")).toString(), QStringLiteral("rolled-back"));
    const QJsonObject signOff = data.value(QStringLiteral("sign_off")).toObject();
    QVERIFY(!signOff.isEmpty());
    QCOMPARE(signOff.value(QStringLiteral("published_sha256")).toString(), publishedSha);
    QVERIFY(!data.value(QStringLiteral("revalidation")).toObject().isEmpty());
    QVERIFY(QFile::exists(reportPath));

    QFile restored(restoredPath);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), publishedBytes);
    restored.close();
    const QString otherOutput = directory.filePath(QStringLiteral("restored-without-report.pdf"));
    const QString missingReport = directory.filePath(QStringLiteral("missing/report.json"));
    const ToolRun failedReport = runPdfTool({ QStringLiteral("rollback"), publishedPath,
                                              QStringLiteral("--to"), publishedSha,
                                              QStringLiteral("--output"), otherOutput,
                                              QStringLiteral("--profile"), profilePath,
                                              QStringLiteral("--report-file"), missingReport });
    QVERIFY(failedReport.exitCode != 0);
    QVERIFY(QFile::exists(otherOutput));
    QVERIFY(!QFile::exists(missingReport));
    const QJsonArray outputs = failedReport.json.value(QStringLiteral("outputs")).toArray();
    for (const QJsonValue& output : outputs)
    {
        QVERIFY(output.toObject().value(QStringLiteral("role")).toString() != QStringLiteral("report"));
    }
}

QTEST_MAIN(PdfToolContractTest)
#include "tst_pdftoolcontract.moc"
