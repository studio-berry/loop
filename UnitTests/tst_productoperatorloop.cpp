// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "editorhost.h"
#include "inspectormodel.h"
#include "loopcanvasitem.h"
#include "operatoracceptancehelpers.h"
#include "preflightcontroller.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfrepairoperation.h"
#include "preflightengine.h"
#include "preflightfindingsmodel.h"
#include "previewstatemodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

pdf::PreflightResult preflightResultFromJson(const QJsonObject& object)
{
    pdf::PreflightResult result;
    result.inspectionComplete = object.value(QStringLiteral("inspectionComplete")).toBool(true);
    for (const QJsonValue& value : object.value(QStringLiteral("findings")).toArray())
    {
        const QJsonObject entry = value.toObject();
        pdf::PreflightFinding finding;
        finding.checkId = entry.value(QStringLiteral("checkId")).toString();
        finding.scope = entry.value(QStringLiteral("scope")).toString(QStringLiteral("page"));
        finding.page = entry.value(QStringLiteral("page")).toInt(1);
        finding.type = entry.value(QStringLiteral("type")).toString();
        finding.severity = entry.value(QStringLiteral("severity")).toString();
        finding.message = entry.value(QStringLiteral("message")).toString();
        if (finding.severity == QStringLiteral("warning"))
        {
            result.warnings.append(finding);
        }
        else
        {
            result.errors.append(finding);
        }
    }
    for (const QJsonValue& value : object.value(QStringLiteral("checkStatuses")).toArray())
    {
        const QJsonObject entry = value.toObject();
        pdf::PreflightCheckStatus status;
        status.id = entry.value(QStringLiteral("id")).toString();
        status.status = entry.value(QStringLiteral("status")).toString();
        result.checkStatuses.append(status);
    }
    return result;
}

QStringList sortedLabels(const QStringList& findingIds, const QHash<QString, QString>& labelById)
{
    QStringList labels;
    labels.reserve(findingIds.size());
    for (const QString& findingId : findingIds)
    {
        labels.append(labelById.value(findingId, findingId));
    }
    labels.sort();
    return labels;
}

QStringList sortedStrings(const QJsonArray& array)
{
    QStringList values;
    for (const QJsonValue& value : array)
    {
        values.append(value.toString());
    }
    values.sort();
    return values;
}

QString writeBleedRecipe(const QString& directory)
{
    const QString recipePath = QDir(directory).filePath(QStringLiteral("compare-review-recipe.json"));
    QFile recipe(recipePath);
    if (!recipe.open(QIODevice::WriteOnly))
    {
        return QString();
    }
    recipe.write(QJsonDocument(QJsonObject{
                                   { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                   { QStringLiteral("id"), QStringLiteral("compare-review") },
                                   { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                   { QStringLiteral("steps"),
                                     QJsonArray{ QJsonObject{
                                         { QStringLiteral("id"), QStringLiteral("bleed") },
                                         { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                         { QStringLiteral("params"),
                                           QJsonObject{ { QStringLiteral("bleed_mm"), 3 },
                                                        { QStringLiteral("mode"), QStringLiteral("mirror") },
                                                        { QStringLiteral("force"), true } } } } } } })
                     .toJson(QJsonDocument::Compact));
    recipe.close();
    return recipePath;
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
    void compareReviewGoldenFixtureMatchesCoreFindingDelta();
    void compareWorkspaceBlocksAStaleComparison();
    void compareWorkspaceNavigatesMaterialDeltasAfterARun();
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

void ProductOperatorLoopTest::compareReviewGoldenFixtureMatchesCoreFindingDelta()
{
    const QString path = QStringLiteral(LOOP_UNITTEST_SOURCE_DIR) +
                         QStringLiteral("/testdata/compare-review/golden-comparison.json");
    QJsonObject golden;
    QVERIFY2(readJsonObject(path, &golden), qPrintable(path));

    const pdf::PreflightResult before =
        preflightResultFromJson(golden.value(QStringLiteral("before")).toObject());
    const pdf::PreflightResult after =
        preflightResultFromJson(golden.value(QStringLiteral("after")).toObject());

    QHash<QString, QString> labelById;
    for (const pdf::PreflightResult* result : { &before, &after })
    {
        for (const pdf::PreflightFinding& finding : result->errors)
        {
            labelById.insert(finding.stableId(), finding.type);
        }
        for (const pdf::PreflightFinding& finding : result->warnings)
        {
            labelById.insert(finding.stableId(), finding.type);
        }
    }

    const pdf::PDFRepairFindingDelta delta = pdf::computeFindingDelta(before, after);
    const QJsonObject expected = golden.value(QStringLiteral("expected")).toObject();
    QCOMPARE(delta.compared, expected.value(QStringLiteral("compared")).toBool());
    QCOMPARE(sortedLabels(delta.resolvedFindingIds, labelById),
             sortedStrings(expected.value(QStringLiteral("resolved")).toArray()));
    QCOMPARE(sortedLabels(delta.unchangedFindingIds, labelById),
             sortedStrings(expected.value(QStringLiteral("unchanged")).toArray()));
    QCOMPARE(sortedLabels(delta.introducedFindingIds, labelById),
             sortedStrings(expected.value(QStringLiteral("introduced")).toArray()));
    QCOMPARE(sortedLabels(delta.incompleteFindingIds, labelById),
             sortedStrings(expected.value(QStringLiteral("incomplete")).toArray()));

    // The same pair classifies the same way on a re-run; the Compare workspace never shows a
    // delta Core would derive differently the second time.
    const pdf::PDFRepairFindingDelta again = pdf::computeFindingDelta(before, after);
    QCOMPARE(again.resolvedFindingIds, delta.resolvedFindingIds);
    QCOMPARE(again.unchangedFindingIds, delta.unchangedFindingIds);
    QCOMPARE(again.introducedFindingIds, delta.introducedFindingIds);
}

void ProductOperatorLoopTest::compareWorkspaceBlocksAStaleComparison()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString recipePath = writeBleedRecipe(directory.path());
    QVERIFY(!recipePath.isEmpty());

    const QString documentPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFileInfo::exists(documentPath));

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    const QVariantMap review = host.compareReview();
    QVERIFY(review.value(QStringLiteral("available")).toBool());
    QVERIFY(!review.value(QStringLiteral("blocked")).toBool());
    QCOMPARE(review.value(QStringLiteral("lifecycleStateName")).toString(), QStringLiteral("preview-ready"));

    // The comparison is bound to the exact input the plan was produced from.
    const QVariantMap before = review.value(QStringLiteral("before")).toMap();
    QVERIFY(!before.value(QStringLiteral("sourceSha256")).toString().isEmpty());
    QCOMPARE(before.value(QStringLiteral("sourceSha256")).toString(),
             host.fixPlanIdentity().value(QStringLiteral("sourceSha256")).toString());
    QCOMPARE(review.value(QStringLiteral("plan")).toMap().value(QStringLiteral("planDigest")).toString(),
             host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString());

    // A revision change makes the plan and preview stale. The comparison blocks instead of
    // silently refreshing, and its material deltas stay unnavigable.
    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    const QVariantMap stale = host.compareReview();
    QVERIFY(stale.value(QStringLiteral("available")).toBool());
    QVERIFY2(stale.value(QStringLiteral("blocked")).toBool(),
             qPrintable(stale.value(QStringLiteral("blockedReason")).toString()));
    QCOMPARE(stale.value(QStringLiteral("lifecycleStateName")).toString(), QStringLiteral("stale"));
    QVERIFY(!stale.value(QStringLiteral("blockedReason")).toString().trimmed().isEmpty());
    QVERIFY(!host.navigateCompareDelta(0));
}

void ProductOperatorLoopTest::compareWorkspaceNavigatesMaterialDeltasAfterARun()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString recipePath = writeBleedRecipe(directory.path());
    QVERIFY(!recipePath.isEmpty());

    const QString documentPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFileInfo::exists(documentPath));

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("succeeded"), 120000);

    const QVariantMap review = host.compareReview();
    QVERIFY(review.value(QStringLiteral("available")).toBool());
    QVERIFY(!review.value(QStringLiteral("blocked")).toBool());
    QCOMPARE(review.value(QStringLiteral("findingDelta")).toMap().value(QStringLiteral("compared")).toBool(),
             true);
    QVERIFY(review.value(QStringLiteral("hasMaterialDeltas")).toBool());

    // Navigating a material delta routes to the step that produced it; it never mutates a
    // document or reruns the comparison.
    QVERIFY(host.navigateCompareDelta(0));
    QCOMPARE(host.workspace(), EditorHost::Inspect);
}

QTEST_MAIN(ProductOperatorLoopTest)

#include "tst_productoperatorloop.moc"
