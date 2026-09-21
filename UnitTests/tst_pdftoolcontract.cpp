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
    void schemaRejectsNonJsonOutput();
    void schemaReportsTheMatrixForEveryKind();
    void schemaReportsUnsupportedMajorIdenticallyToCore();
    void schemaReportsUnreadyForAnUnusableVersion();
    void schemaAcceptsCurrentAndPreviousGoldens();
    void capabilitiesReportMatrixVersions();
    void redactRefusesToWriteOverItsOwnInput();
    void addBleedRefusesToWriteOverItsOwnInput();
    void rgbToCmykRefusesToWriteOverItsOwnInput();
};

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

}   // namespace

QTEST_MAIN(PdfToolContractTest)
#include "tst_pdftoolcontract.moc"
