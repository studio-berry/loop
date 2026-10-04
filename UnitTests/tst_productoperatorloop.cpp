// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "editorhost.h"
#include "inspectormodel.h"
#include "loopcanvasitem.h"
#include "operatoracceptancehelpers.h"
#include "pdfartifactidentity.h"
#include "pdfpreflightverdict.h"
#include "preflightcontroller.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "preflightfindingsmodel.h"
#include "previewstatemodel.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

using pdfinteraction::InspectorModel;
using pdfinteraction::PreflightController;
using pdfinteraction::PreviewStateModel;

namespace
{

pdf::PreflightFinding makeFinding(int page = 1)
{
    pdf::PreflightFinding finding;
    finding.checkId = QStringLiteral("bleed");
    finding.scope = QStringLiteral("page");
    finding.page = page;
    finding.severity = QStringLiteral("error");
    finding.type = QStringLiteral("bleed");
    finding.message = QStringLiteral("Bleed is insufficient");
    finding.bbox = QRectF(10, 20, 30, 40);
    finding.evidenceIds = { QStringLiteral("evidence-bleed-1") };
    return finding;
}

QString pdfToolPath()
{
    const QString executable =
#ifdef Q_OS_WIN
        QStringLiteral("PdfTool.exe");
#else
        QStringLiteral("PdfTool");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(executable);
}

bool readJsonObject(const QString& path, QJsonObject* object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return false;
    }

    if (object)
    {
        *object = document.object();
    }
    return true;
}

QStringList repairArguments(const QString& source,
                            const QString& output,
                            const QString& report,
                            bool dryRun,
                            const QString& profile)
{
    QStringList arguments{
        QStringLiteral("repair"), source,
        QStringLiteral("--operation"), QStringLiteral("add-bleed"),
        QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
        QStringLiteral("--param"), QStringLiteral("mode=mirror"),
        QStringLiteral("--param"), QStringLiteral("force=true"),
        QStringLiteral("--profile"), profile,
        QStringLiteral("--console-format"), QStringLiteral("json")
    };
    if (!output.isEmpty())
    {
        arguments << QStringLiteral("--output") << output;
    }
    if (!report.isEmpty())
    {
        arguments << QStringLiteral("--report-file") << report;
    }
    if (dryRun)
    {
        arguments << QStringLiteral("--dry-run");
    }
    return arguments;
}

QString firstBleedFindingId(const PreflightController& preflight)
{
    const pdfinteraction::PreflightFindingsModel* findings = preflight.findingsModel();
    for (int row = 0; row < findings->rowCount(); ++row)
    {
        const QModelIndex index = findings->index(row, 0);
        if (findings->data(index, pdfinteraction::PreflightFindingsModel::CheckIdRole).toString() ==
            QStringLiteral("bleed"))
        {
            return findings->data(index, pdfinteraction::PreflightFindingsModel::FindingIdRole).toString();
        }
    }
    return {};
}

bool reportHasFixup(const QByteArray& reportBytes, const QString& fixupId)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(reportBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return false;
    }

    for (const QJsonValue& value : document.object().value(QStringLiteral("fixups_available")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == fixupId)
        {
            return true;
        }
    }
    return false;
}

QString processDetail(bool started, int exitCode, const QByteArray& stdOut, const QByteArray& stdErr)
{
    return QStringLiteral("started=%1 exit=%2 stdout=%3 stderr=%4")
        .arg(started ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(exitCode)
        .arg(QString::fromUtf8(stdOut).trimmed().left(1024))
        .arg(QString::fromUtf8(stdErr).trimmed().left(1024));
}

QJsonObject runPdfToolJson(const QStringList& arguments, int* exitCode, QByteArray* stdErr)
{
    QByteArray stdOut;
    int code = -1;
    operatoracceptance::runPdfTool(pdfToolPath(), arguments, &stdOut, stdErr, &code);
    if (exitCode)
    {
        *exitCode = code;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdOut, &parseError);
    return parseError.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}

/// The receipt the isolated worker presents for one file inspection.
QJsonObject workerReceipt(const QString& fixture, const QString& profile, int* exitCode, QByteArray* stdErr)
{
    const QJsonObject envelope = runPdfToolJson({ QStringLiteral("worker-preflight"), fixture,
                                                  QStringLiteral("--profile"), profile,
                                                  QStringLiteral("--console-format"), QStringLiteral("json") },
                                                exitCode, stdErr);
    return envelope.value(QStringLiteral("data")).toObject().value(QStringLiteral("receipt")).toObject();
}

QStringList jsonStringArray(const QJsonValue& value)
{
    QStringList values;
    for (const QJsonValue& item : value.toArray())
    {
        values.append(item.toString());
    }
    return values;
}

QStringList advertisedFixupIds(const QJsonObject& report)
{
    QStringList ids;
    for (const QJsonValue& value : report.value(QStringLiteral("fixups_available")).toArray())
    {
        ids.append(value.toObject().value(QStringLiteral("id")).toString());
    }
    ids.sort();
    return ids;
}

QMap<QString, QString> checkOutcomes(const QJsonArray& checks)
{
    QMap<QString, QString> outcomes;
    for (const QJsonValue& value : checks)
    {
        const QJsonObject check = value.toObject();
        outcomes.insert(check.value(QStringLiteral("id")).toString(),
                        check.value(QStringLiteral("status")).toString() + QLatin1Char('/') +
                            check.value(QStringLiteral("reason")).toString());
    }
    return outcomes;
}

/// Core derives the receipt identity from the input digest, the effective
/// profile digest and the evaluated coverage scope. Reproducing it from a
/// report's own provenance is what shows both adapters bound one receipt.
QString receiptIdentity(const QJsonObject& report)
{
    const QJsonObject identity{
        { QStringLiteral("kind"), QStringLiteral("loop.inspection-receipt-identity.v1") },
        { QStringLiteral("input_digest"), report.value(QStringLiteral("document_revision_digest")).toString() },
        { QStringLiteral("effective_profile_digest"), report.value(QStringLiteral("effective_profile_digest")).toString() },
        { QStringLiteral("coverage_scope"), report.value(QStringLiteral("coverage_scope")) }
    };
    return QString::fromLatin1(QCryptographicHash::hash(pdf::canonicalJson(identity), QCryptographicHash::Sha256).toHex());
}

}   // namespace

class ProductOperatorLoopTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void operatorLoop_replayableTraceCompletesSaveAsAndRevalidation();
    void operatorLoop_cancellationIsTerminalAndBounded();
    void operatorLoop_closeDuringPreflightLeavesNoDocument();
    void operatorLoop_missingPdfToolProducesActionableFailure();
    void operatorLoop_corruptInputFailsClosed();
    void operatorLoop_blockedOutputProducesActionableFailure();
    void openDetectPinpointInspectUnderstandState();
    void findingNavigationMovesCanvasToTheFindingPage();
    void workspaceTransitionsKeepTheOpenDocumentBound();
    void canvasBindingClearsWhenTheDocumentCloses();
    void cancellationLeavesNoAcceptedResult();
    void adapterParityOverOneInspectionReceipt();
};

void ProductOperatorLoopTest::initTestCase()
{
    // The product target intentionally stays free of application resources;
    // install the shipped profile into Qt's isolated test config so EditorHost
    // exercises its production profile discovery path without changing CMake.
    QStandardPaths::setTestModeEnabled(true);
    const QString profileDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                         .filePath(QStringLiteral("profiles"));
    QVERIFY(QDir().mkpath(profileDirectory));

    QFile source(operatoracceptance::defaultProfilePath());
    QVERIFY2(source.open(QIODevice::ReadOnly), "the shipped default profile must be readable");
    QSaveFile target(QDir(profileDirectory).filePath(QStringLiteral("loop-default.json")));
    QVERIFY(target.open(QIODevice::WriteOnly));
    QVERIFY(target.write(source.readAll()) > 0);
    QVERIFY(target.commit());
}

void ProductOperatorLoopTest::operatorLoop_replayableTraceCompletesSaveAsAndRevalidation()
{
    operatoracceptance::OperatorLoopTrace trace(QStringLiteral("operator-loop-happy-path"));
    const QString source = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    const QString profile = operatoracceptance::defaultProfilePath();
    const QByteArray sourceDigest = operatoracceptance::fileSha256(source);
    QVERIFY2(!sourceDigest.isEmpty(), "source digest must be available before the trace starts");

    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());
    const QString previewReport = outputDirectory.filePath(QStringLiteral("repair-preview.json"));
    const QString output = outputDirectory.filePath(QStringLiteral("bleed-fixed.pdf"));
    const QString repairReport = outputDirectory.filePath(QStringLiteral("repair-report.json"));

    EditorHost host;
    trace.note(QStringLiteral("open"), source);
    host.openFileUrl(QUrl::fromLocalFile(source));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QCOMPARE(host.pageCount(), 1);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);
    QCOMPARE(host.currentPage(), 0);

    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    trace.note(QStringLiteral("preflight"));
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(preflight->state() != PreflightController::State::Running, 30000);
    trace.note(QStringLiteral("preflight-result"),
               QStringLiteral("%1: %2").arg(host.preflightStateName(), preflight->operatorSummary()));
    QCOMPARE(preflight->state(), PreflightController::State::Findings);

    const QString findingId = firstBleedFindingId(*preflight);
    QVERIFY2(!findingId.isEmpty(), "the GUI report must expose a stable bleed finding id");
    trace.note(QStringLiteral("finding-selection"), findingId);
    host.selectFinding(findingId);
    auto* inspector = qobject_cast<InspectorModel*>(host.inspector());
    QVERIFY(inspector);
    QCOMPARE(inspector->selectionKind(), InspectorModel::SelectionKind::Finding);
    QCOMPARE(inspector->selectionId(), findingId);

    PreflightController::EvidenceNavigationRequest navigation;
    trace.note(QStringLiteral("finding-navigation"));
    QVERIFY(preflight->navigationFor(findingId, &navigation));
    QCOMPARE(navigation.findingId, findingId);
    QCOMPARE(navigation.documentRevision, preflight->documentRevision());
    QCOMPARE(navigation.page, 1);
    QCOMPARE(host.currentPage(), navigation.page - 1);

    trace.note(QStringLiteral("corrective-intent"), QStringLiteral("add-bleed"));
    QVERIFY2(reportHasFixup(preflight->serializedReport(source), QStringLiteral("add-bleed")),
             "GUI preflight must advertise the registered corrective operation");

    QVERIFY(trace.replay(QStringLiteral("production-preview"), [&host]
                         {
                             host.setWorkspace(EditorHost::ProductionPreview);
                             return host.workspace() == EditorHost::ProductionPreview &&
                                   !host.previewSummary().isEmpty(); }));

    QByteArray previewStdOut;
    QByteArray previewStdErr;
    int previewExitCode = -1;
    trace.note(QStringLiteral("repair-preview"), previewReport);
    const bool previewStarted = operatoracceptance::runPdfTool(pdfToolPath(),
                                                               repairArguments(source, output, previewReport, true, profile),
                                                               &previewStdOut,
                                                               &previewStdErr,
                                                               &previewExitCode);
    trace.note(QStringLiteral("repair-preview-result"),
               processDetail(previewStarted, previewExitCode, previewStdOut, previewStdErr));
    QVERIFY(previewStarted);
    QCOMPARE(previewExitCode, 0);
    QVERIFY2(!QFile::exists(output), "preview must not commit a candidate output");
    QJsonObject preview;
    QVERIFY(readJsonObject(previewReport, &preview));
    QCOMPARE(preview.value(QStringLiteral("status")).toString(), QStringLiteral("planned"));

    QByteArray repairStdOut;
    QByteArray repairStdErr;
    int repairExitCode = -1;
    trace.note(QStringLiteral("save-as-new-output"), output);
    const bool repairStarted = operatoracceptance::runPdfTool(pdfToolPath(),
                                                              repairArguments(source, output, repairReport, false, profile),
                                                              &repairStdOut,
                                                              &repairStdErr,
                                                              &repairExitCode);
    trace.note(QStringLiteral("save-as-result"),
               processDetail(repairStarted, repairExitCode, repairStdOut, repairStdErr));
    QVERIFY(repairStarted);
    QCOMPARE(repairExitCode, 0);
    QVERIFY2(QFile::exists(output), qPrintable(QStringLiteral("repair stderr: %1").arg(QString::fromUtf8(repairStdErr))));
    QJsonObject repair;
    QVERIFY(readJsonObject(repairReport, &repair));
    QCOMPARE(repair.value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    QVERIFY(!repair.value(QStringLiteral("output")).toObject().value(QStringLiteral("sha256")).toString().isEmpty());
    QCOMPARE(operatoracceptance::fileSha256(source), sourceDigest);

    trace.note(QStringLiteral("reopen-output"), output);
    host.openFileUrl(QUrl::fromLocalFile(output));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(preflight->state() != PreflightController::State::Running, 30000);
    trace.note(QStringLiteral("revalidate"),
               preflight->state() == PreflightController::State::Pass ? QStringLiteral("pass")
                                                                      : preflight->operatorSummary());
    QCOMPARE(preflight->state(), PreflightController::State::Pass);
    QCOMPARE(operatoracceptance::fileSha256(source), sourceDigest);

    trace.complete();
}

void ProductOperatorLoopTest::operatorLoop_cancellationIsTerminalAndBounded()
{
    operatoracceptance::OperatorLoopTrace trace(QStringLiteral("operator-loop-cancellation"));
    EditorHost host;
    const QString source = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    trace.note(QStringLiteral("open"), source);
    host.openFileUrl(QUrl::fromLocalFile(source));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    trace.note(QStringLiteral("preflight"));
    QVERIFY(host.runPreflight());
    trace.note(QStringLiteral("cancel-preflight"));
    QVERIFY2(host.cancelPreflight(), "a running preflight must accept cancellation");
    QVERIFY(preflight->state() != PreflightController::State::Running);
    QTRY_VERIFY_WITH_TIMEOUT(preflight->state() != PreflightController::State::Running, 10000);
    QVERIFY(!host.hasPreflightReport());
    trace.complete();
}

void ProductOperatorLoopTest::operatorLoop_closeDuringPreflightLeavesNoDocument()
{
    operatoracceptance::OperatorLoopTrace trace(QStringLiteral("operator-loop-close-during-operation"));
    EditorHost host;
    const QString source = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    trace.note(QStringLiteral("open"), source);
    host.openFileUrl(QUrl::fromLocalFile(source));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    trace.note(QStringLiteral("preflight"));
    QVERIFY(host.runPreflight());
    trace.note(QStringLiteral("close-during-preflight"));
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionClose")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionClose")) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(!host.hasDocument(), 10000);
    QCOMPARE(host.documentState(), QStringLiteral("empty"));
    QCOMPARE(host.preflightStateName(), QStringLiteral("not-checked"));
    trace.complete();
}

void ProductOperatorLoopTest::operatorLoop_missingPdfToolProducesActionableFailure()
{
    operatoracceptance::OperatorLoopTrace trace(QStringLiteral("operator-loop-missing-pdftool"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missing = directory.filePath(QStringLiteral("PdfTool-that-is-not-installed"));
    QByteArray stdOut;
    QByteArray stdErr;
    int exitCode = -1;
    trace.note(QStringLiteral("missing-pdftool"), missing);
    const bool started = operatoracceptance::runPdfTool(missing,
                                                        { QStringLiteral("capabilities"), QStringLiteral("--console-format"), QStringLiteral("json") },
                                                        &stdOut,
                                                        &stdErr,
                                                        &exitCode);
    trace.note(QStringLiteral("missing-pdftool-result"), processDetail(started, exitCode, stdOut, stdErr));
    QVERIFY(!started);
    QVERIFY2(!stdErr.trimmed().isEmpty(), "missing PdfTool must retain the process-start diagnostic");
    trace.complete();
}

void ProductOperatorLoopTest::operatorLoop_corruptInputFailsClosed()
{
    operatoracceptance::OperatorLoopTrace trace(QStringLiteral("operator-loop-corrupt-input"));
    const QString corrupt = operatoracceptance::fixturePath(QStringLiteral("malformed-not-pdf.pdf"));
    QByteArray stdOut;
    QByteArray stdErr;
    int exitCode = -1;
    trace.note(QStringLiteral("open-corrupt-input"), corrupt);
    const bool started = operatoracceptance::runPdfTool(pdfToolPath(),
                                                        { QStringLiteral("preflight"), corrupt,
                                                          QStringLiteral("--profile"), operatoracceptance::defaultProfilePath(),
                                                          QStringLiteral("--console-format"), QStringLiteral("json") },
                                                        &stdOut,
                                                        &stdErr,
                                                        &exitCode);
    trace.note(QStringLiteral("corrupt-input-result"), processDetail(started, exitCode, stdOut, stdErr));
    QVERIFY(started);
    QVERIFY2(exitCode != 0, "corrupt input must not report a successful preflight");
    QVERIFY2(exitCode != 1, "corrupt input must not masquerade as a findings result");
    QVERIFY2(!stdErr.trimmed().isEmpty() || !stdOut.trimmed().isEmpty(),
             "corrupt input must produce actionable output");
    trace.complete();
}

void ProductOperatorLoopTest::operatorLoop_blockedOutputProducesActionableFailure()
{
    operatoracceptance::OperatorLoopTrace trace(QStringLiteral("operator-loop-permission-denied-output"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString blocker = directory.filePath(QStringLiteral("output-parent-is-a-file"));
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("blocked") > 0);
    file.close();
    const QString blockedOutput = QDir(blocker).filePath(QStringLiteral("bleed-fixed.pdf"));
    QByteArray stdOut;
    QByteArray stdErr;
    int exitCode = -1;
    trace.note(QStringLiteral("permission-denied-output"), blockedOutput);
    const bool started = operatoracceptance::runPdfTool(pdfToolPath(),
                                                        repairArguments(operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf")),
                                                                        blockedOutput,
                                                                        QString(),
                                                                        false,
                                                                        operatoracceptance::defaultProfilePath()),
                                                        &stdOut,
                                                        &stdErr,
                                                        &exitCode);
    trace.note(QStringLiteral("permission-denied-output-result"), processDetail(started, exitCode, stdOut, stdErr));
    QVERIFY(started);
    QVERIFY2(exitCode != 0, "a blocked output path must fail closed");
    QVERIFY2(!stdErr.trimmed().isEmpty() || !stdOut.trimmed().isEmpty(),
             "a blocked output path must retain actionable diagnostics");
    trace.complete();
}

void ProductOperatorLoopTest::openDetectPinpointInspectUnderstandState()
{
    const QString pdfPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY2(QFileInfo::exists(pdfPath), pdfPath.toUtf8().constData());

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(pdfPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QCOMPARE(host.pageCount(), 1);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);
    QCOMPARE(host.currentPage(), 0);

    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    const QString documentKey = preflight->documentKey();
    const QString documentRevision = preflight->documentRevision();
    QVERIFY(!documentKey.isEmpty());
    QVERIFY(!documentRevision.isEmpty());

    preflight->beginRun(documentKey, documentRevision, QStringLiteral("profile"), QStringLiteral("job-1"));
    const pdf::PreflightFinding finding = makeFinding();
    pdf::PreflightResult result;
    result.errors = { finding };
    QVERIFY(preflight->acceptResult(QStringLiteral("job-1"), documentRevision, result));
    QCOMPARE(preflight->state(), PreflightController::State::Findings);
    QCOMPARE(host.preflightStateName(), QStringLiteral("findings"));

    const QString findingId = finding.stableId();
    host.selectFinding(findingId);

    auto* inspector = qobject_cast<InspectorModel*>(host.inspector());
    QVERIFY(inspector);
    QCOMPARE(inspector->selectionKind(), InspectorModel::SelectionKind::Finding);
    QCOMPARE(inspector->selectionId(), findingId);
    QVERIFY(inspector->rowCount() > 0);

    auto* preview = qobject_cast<PreviewStateModel*>(host.preview());
    QVERIFY(preview);
    QCOMPARE(preview->authority(), PreviewStateModel::Authority::Approximate);
    QCOMPARE(PreviewStateModel::authorityName(preview->authority()), QStringLiteral("approximate"));
    QVERIFY(!host.previewSummary().isEmpty());
    QVERIFY(!host.inspectorTitle().isEmpty());
    QCOMPARE(host.currentPage(), 0);
}

void ProductOperatorLoopTest::findingNavigationMovesCanvasToTheFindingPage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString path = directory.filePath(QStringLiteral("navigation.pdf"));
    QVERIFY(writer.write(path, &document, true));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QCOMPARE(host.pageCount(), 2);
    // Without a viewport size no page can be revealed, so page navigation is a no-op.
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);

    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    auto* inspector = qobject_cast<InspectorModel*>(host.inspector());
    QVERIFY(preflight);
    QVERIFY(inspector);

    preflight->beginRun(preflight->documentKey(), preflight->documentRevision(), QStringLiteral("profile"), QStringLiteral("job-1"));
    pdf::PreflightFinding finding = makeFinding(2);
    const QString findingId = finding.stableId();
    pdf::PreflightResult result;
    result.errors = { finding };
    QVERIFY(preflight->acceptResult(QStringLiteral("job-1"), preflight->documentRevision(), result));

    host.selectFinding(findingId);
    QCOMPARE(inspector->selectionKind(), InspectorModel::SelectionKind::Finding);
    QCOMPARE(inspector->selectionId(), findingId);
    QCOMPARE(host.currentPage(), 1);
}

void ProductOperatorLoopTest::workspaceTransitionsKeepTheOpenDocumentBound()
{
    const QString pdfPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFileInfo::exists(pdfPath));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(pdfPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    const QString revision = preflight->documentRevision();

    for (const EditorHost::LoopWorkspace workspace : { EditorHost::Document,
                                                       EditorHost::Preflight,
                                                       EditorHost::ProductionPreview,
                                                       EditorHost::Pages,
                                                       EditorHost::Inspect,
                                                       EditorHost::Fix })
    {
        host.setWorkspace(workspace);
        QCOMPARE(host.workspace(), workspace);
        QVERIFY(host.hasDocument());
        QCOMPARE(preflight->documentRevision(), revision);
        QVERIFY(!host.documentShellStatus().isEmpty());
    }
}

void ProductOperatorLoopTest::canvasBindingClearsWhenTheDocumentCloses()
{
    const QString pdfPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFileInfo::exists(pdfPath));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(pdfPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 640, 480);

    QQuickWindow window;
    window.resize(640, 480);
    auto* canvas = new pdfquick::LoopCanvasItem(window.contentItem());
    canvas->setSize(QSizeF(640, 480));
    host.attachCanvas(canvas);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY_WITH_TIMEOUT(canvas->viewport() != nullptr, 5000);
    QVERIFY(canvas->accessibleDocumentSummary().contains(QStringLiteral("Page")));

    QVERIFY(host.isCommandEnabled(QStringLiteral("actionClose")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionClose")) != 0);
    QTRY_VERIFY_WITH_TIMEOUT(!host.hasDocument(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(canvas->viewport() == nullptr, 5000);
    QCOMPARE(canvas->accessibleDocumentSummary(), QStringLiteral("No document is currently open."));
}

void ProductOperatorLoopTest::cancellationLeavesNoAcceptedResult()
{
    const QString pdfPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFileInfo::exists(pdfPath));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(pdfPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    preflight->beginRun(preflight->documentKey(), preflight->documentRevision(), QStringLiteral("profile"), QStringLiteral("job-cancel"));
    QCOMPARE(preflight->state(), PreflightController::State::Running);
    QVERIFY(host.cancelPreflight());
    QCOMPARE(preflight->state(), PreflightController::State::Cancelled);
    QVERIFY(!preflight->hasResult());
    QVERIFY(!host.hasPreflightReport());
}

void ProductOperatorLoopTest::adapterParityOverOneInspectionReceipt()
{
    // Issue #26: one inspection over one fixture, read by both presentation
    // adapters. The Editor host renders the accepted inspection; the isolated
    // worker presents the receipt and the CLI presents the report for the same
    // bytes. Every comparison below is across surfaces, not within one.
    const QString fixture = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    const QString profile = operatoracceptance::defaultProfilePath();
    QVERIFY2(QFileInfo::exists(fixture), fixture.toUtf8().constData());
    QVERIFY2(QFileInfo::exists(profile), profile.toUtf8().constData());

    operatoracceptance::OperatorLoopTrace trace(QStringLiteral("adapter-parity-one-receipt"));

    EditorHost host;
    trace.note(QStringLiteral("editor-open"), fixture);
    host.openFileUrl(QUrl::fromLocalFile(fixture));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(preflight->state() != PreflightController::State::Running, 30000);
    QCOMPARE(preflight->state(), PreflightController::State::Findings);

    const QJsonObject editorReport = QJsonDocument::fromJson(preflight->serializedReport(fixture)).object();
    QVERIFY2(!editorReport.isEmpty(), "the Editor must present the accepted report");
    QStringList editorFindingIds;
    for (int row = 0; row < preflight->findingsModel()->rowCount(); ++row)
    {
        editorFindingIds.append(preflight->findingsModel()
                                    ->data(preflight->findingsModel()->index(row, 0),
                                           pdfinteraction::PreflightFindingsModel::FindingIdRole)
                                    .toString());
    }

    int receiptExit = -1;
    QByteArray receiptError;
    const QJsonObject receiptJson = workerReceipt(fixture, profile, &receiptExit, &receiptError);
    QVERIFY2(!receiptJson.isEmpty(), qPrintable(QString::fromUtf8(receiptError)));
    pdf::PreflightInspectionReceipt receipt;
    QString receiptParseError;
    QVERIFY2(pdf::preflightInspectionReceiptFromJson(receiptJson, receipt, receiptParseError),
             qPrintable(receiptParseError));

    int repeatExit = -1;
    QByteArray repeatError;
    const QJsonObject repeatReceipt = workerReceipt(fixture, profile, &repeatExit, &repeatError);
    QCOMPARE(repeatExit, receiptExit);
    QCOMPARE(repeatReceipt.value(QStringLiteral("identity")).toString(),
             receiptJson.value(QStringLiteral("identity")).toString());

    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());
    const QString headlessReportPath = outputDirectory.filePath(QStringLiteral("preflight-report.json"));
    int reportExit = -1;
    QByteArray reportError;
    runPdfToolJson({ QStringLiteral("preflight"), fixture,
                     QStringLiteral("--profile"), profile,
                     QStringLiteral("--report-file"), headlessReportPath,
                     QStringLiteral("--console-format"), QStringLiteral("json") },
                   &reportExit, &reportError);
    QJsonObject headlessReport;
    QVERIFY2(readJsonObject(headlessReportPath, &headlessReport), qPrintable(QString::fromUtf8(reportError)));

    // Finding identity: the receipt's blocking ids are exactly what the Editor
    // lists and what the CLI reports.
    const QJsonObject receiptVerdict = receiptJson.value(QStringLiteral("verdict")).toObject();
    const QStringList blockingIds = jsonStringArray(receiptVerdict.value(QStringLiteral("blocking_finding_ids")));
    QVERIFY2(!blockingIds.isEmpty(), "the fixture must establish a blocking finding");
    QCOMPARE(jsonStringArray(editorReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("blocking_finding_ids"))),
             blockingIds);
    QCOMPARE(jsonStringArray(headlessReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("blocking_finding_ids"))),
             blockingIds);
    for (const QString& findingId : blockingIds)
    {
        QVERIFY2(editorFindingIds.contains(findingId), qPrintable(findingId));
    }
    QCOMPARE(jsonStringArray(receiptVerdict.value(QStringLiteral("waived_finding_ids"))),
             jsonStringArray(editorReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("waived_finding_ids"))));

    // Verdict: one Core reducer decides the state, and the CLI's exit code is its
    // published mapping.
    const QString verdictState = receiptVerdict.value(QStringLiteral("state")).toString();
    QVERIFY(!verdictState.isEmpty());
    QCOMPARE(editorReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(), verdictState);
    QCOMPARE(headlessReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(), verdictState);
    QCOMPARE(headlessReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("reason_code")).toString(),
             editorReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("reason_code")).toString());
    QCOMPARE(reportExit, pdf::preflightVerdictProcessExitCode(receipt.verdict.state));
    QCOMPARE(receiptExit, pdf::preflightVerdictProcessExitCode(receipt.verdict.state));

    // Coverage: the evaluated scope is recorded once and reported unchanged.
    const QJsonObject coverageScope = receiptJson.value(QStringLiteral("coverage_scope")).toObject();
    QVERIFY2(!coverageScope.isEmpty(), "the receipt must record the evaluated coverage scope");
    QCOMPARE(editorReport.value(QStringLiteral("coverage_scope")).toObject(), coverageScope);
    QCOMPARE(headlessReport.value(QStringLiteral("coverage_scope")).toObject(), coverageScope);

    // Limitations: Core records which checks did not complete; the Editor renders
    // those reasons, and no surface narrows the recorded check outcome.
    QVERIFY2(!receipt.limitations.isEmpty(), "the receipt must record its limitations");
    const QMap<QString, QString> editorChecks = checkOutcomes(editorReport.value(QStringLiteral("checks")).toArray());
    const QMap<QString, QString> headlessChecks = checkOutcomes(headlessReport.value(QStringLiteral("checks")).toArray());
    int comparedChecks = 0;
    for (const QJsonValue& value : receiptJson.value(QStringLiteral("checks")).toArray())
    {
        const QJsonObject check = value.toObject();
        if (check.value(QStringLiteral("status")).toString().isEmpty())
        {
            continue;
        }
        const QString outcome = check.value(QStringLiteral("status")).toString() + QLatin1Char('/') +
                                check.value(QStringLiteral("reason")).toString();
        QCOMPARE(editorChecks.value(check.value(QStringLiteral("id")).toString()), outcome);
        QCOMPARE(headlessChecks.value(check.value(QStringLiteral("id")).toString()), outcome);
        ++comparedChecks;
        if (!check.value(QStringLiteral("complete")).toBool() && !check.value(QStringLiteral("reason")).toString().isEmpty())
        {
            QVERIFY2(preflight->limitationDescription().contains(check.value(QStringLiteral("reason")).toString()),
                     qPrintable(preflight->limitationDescription()));
        }
    }
    QVERIFY2(comparedChecks > 0, "the receipt must record a check outcome to compare");

    // Available intents: both surfaces advertise the same corrective operations,
    // and the Editor exposes the same terminal availability for them.
    const QStringList editorFixups = advertisedFixupIds(editorReport);
    QVERIFY2(!editorFixups.isEmpty(), "a blocking bleed finding must advertise its corrective operation");
    QCOMPARE(advertisedFixupIds(headlessReport), editorFixups);
    QVERIFY(preflight->exportUnavailableReason().isEmpty());
    QVERIFY(!preflight->cancelUnavailableReason().isEmpty());

    // The exact receipt identity, recomputed from the Editor's own provenance.
    const QString inputDigest = editorReport.value(QStringLiteral("document_revision_digest")).toString();
    const QString profileDigest = editorReport.value(QStringLiteral("effective_profile_digest")).toString();
    QCOMPARE(inputDigest, inputDigest.toLower());
    QCOMPARE(profileDigest, profileDigest.toLower());
    const QString identity = receiptIdentity(editorReport);
    QCOMPARE(identity, receiptJson.value(QStringLiteral("identity")).toString());
    trace.note(QStringLiteral("receipt-identity"), identity);
    trace.complete();
}

QTEST_MAIN(ProductOperatorLoopTest)

#include "tst_productoperatorloop.moc"
