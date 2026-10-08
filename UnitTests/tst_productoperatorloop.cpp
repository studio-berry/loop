// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "actionlistcontroller.h"
#include "editorhost.h"
#include "inspectormodel.h"
#include "loopcanvasitem.h"
#include "operatoracceptancehelpers.h"
#include "pdfactionlist.h"
#include "pdfartifactidentity.h"
#include "pdfdocumentmanipulator.h"
#include "pdfdocumentreader.h"
#include "pdfgovernedexecution.h"
#include "pdfpagemasterexport.h"
#include "pdfpreflightverdict.h"
#include "preflightcontroller.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfrepairoperation.h"
#include "preflightengine.h"
#include "preflightfindingsmodel.h"
#include "previewstatemodel.h"
#include "loopstatevisual.h"
#include "looptokens.h"
#include "preflightprofileresolver.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QRegularExpression>
#include <memory>

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

class CompareReviewProjection : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap compareReview MEMBER review NOTIFY reviewChanged)

public:
    QVariantMap review;

Q_SIGNALS:
    void reviewChanged();
};

using pdfinteraction::InspectorModel;
using pdfinteraction::PreflightController;
using pdfinteraction::PreviewStateModel;

namespace
{

QQuickItem* findWorkspaceItem(QQuickItem* parent, const QString& name)
{
    if (parent->objectName() == name)
    {
        return parent;
    }
    for (QQuickItem* child : parent->childItems())
    {
        if (QQuickItem* found = findWorkspaceItem(child, name))
        {
            return found;
        }
    }
    return nullptr;
}

std::unique_ptr<QQuickItem> createWorkspacePane(QQmlEngine& engine, QObject& host, QQuickWindow& window,
                                                const QString& name = QStringLiteral("ComparePane.qml"))
{
    engine.rootContext()->setContextProperty(QStringLiteral("editorHost"), &host);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(LOOP_UNITTEST_SOURCE_DIR) +
                                                         QStringLiteral("/../LoopEditor/qml/") + name));
    std::unique_ptr<QQuickItem> pane(qobject_cast<QQuickItem*>(component.create()));
    if (!pane)
    {
        qWarning() << component.errors();
        return pane;
    }
    pane->setParentItem(window.contentItem());
    pane->setSize(window.size());
    return pane;
}

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

QString governedParityDirectory()
{
    return QStringLiteral(LOOP_UNITTEST_SOURCE_DIR) + QStringLiteral("/testdata/governed-parity");
}

pdf::PDFDocument readGovernedFixture(const QString& path)
{
    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    return reader.readFromFile(path);
}

pdf::PDFArtifactIdentity identityForFile(const QString& path)
{
    pdf::PDFArtifactIdentity identity;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return identity;
    }
    const QByteArray bytes = file.readAll();
    identity.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    identity.size = bytes.size();
    identity.mediaType = QStringLiteral("application/pdf");
    identity.logicalName = QFileInfo(path).fileName();
    return identity;
}

/// The governed triple every surface publishes, normalized to the shape
/// scripts/ci/check_governed_parity.py discovers.
QJsonObject governedTriple(const QJsonObject& source)
{
    return QJsonObject{
        { QStringLiteral("approval"), source.value(QStringLiteral("approval")) },
        { QStringLiteral("revalidation"), source.value(QStringLiteral("revalidation")) },
        { QStringLiteral("sign_off"), source.value(QStringLiteral("sign_off")) }
    };
}

/// The checker's rules (scripts/ci/check_governed_parity.py) applied in-process:
/// a self-consistent signed-off identity chain, verified bytes, and a passing
/// verdict. Returns false and fills `why` on the first violation.
bool governedRecordIsCheckerValid(const QJsonObject& governed, QString* why)
{
    const auto fail = [why](const QString& reason)
    {
        if (why)
        {
            *why = reason;
        }
        return false;
    };
    const QJsonObject approval = governed.value(QStringLiteral("approval")).toObject();
    const QJsonObject revalidation = governed.value(QStringLiteral("revalidation")).toObject();
    const QJsonObject signOff = governed.value(QStringLiteral("sign_off")).toObject();
    if (approval.isEmpty() || revalidation.isEmpty() || signOff.isEmpty())
    {
        return fail(QStringLiteral("governed triple is incomplete"));
    }
    for (const QString& field : { QStringLiteral("plan_digest"), QStringLiteral("source_sha256"), QStringLiteral("candidate_sha256") })
    {
        if (!pdf::isPDFSha256(approval.value(field).toString()))
        {
            return fail(QStringLiteral("approval.%1 is not a digest").arg(field));
        }
    }
    if (revalidation.value(QStringLiteral("bytes_verified")).toBool() != true)
    {
        return fail(QStringLiteral("revalidation.bytes_verified is not true"));
    }
    if (revalidation.value(QStringLiteral("sign_off_eligible")).toBool() != true)
    {
        return fail(QStringLiteral("revalidation.sign_off_eligible is not true"));
    }
    if (revalidation.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString() != QStringLiteral("pass"))
    {
        return fail(QStringLiteral("revalidation.verdict.state is not pass"));
    }
    for (const QString& field : { QStringLiteral("plan_digest"), QStringLiteral("source_sha256"), QStringLiteral("candidate_sha256"),
                                  QStringLiteral("published_sha256"), QStringLiteral("revalidation_report_sha256"),
                                  QStringLiteral("effective_profile_digest") })
    {
        if (!pdf::isPDFSha256(signOff.value(field).toString()))
        {
            return fail(QStringLiteral("sign_off.%1 is not a digest").arg(field));
        }
    }
    if (signOff.value(QStringLiteral("published_sha256")).toString() != revalidation.value(QStringLiteral("artifact_sha256")).toString())
    {
        return fail(QStringLiteral("sign_off.published_sha256 does not match revalidation.artifact_sha256"));
    }
    if (signOff.value(QStringLiteral("revalidation_report_sha256")).toString() != revalidation.value(QStringLiteral("report_sha256")).toString())
    {
        return fail(QStringLiteral("sign_off.revalidation_report_sha256 does not match revalidation.report_sha256"));
    }
    if (signOff.value(QStringLiteral("effective_profile_digest")).toString() != revalidation.value(QStringLiteral("effective_profile_digest")).toString())
    {
        return fail(QStringLiteral("sign_off.effective_profile_digest does not match revalidation"));
    }
    const QJsonObject signOffApproval = signOff.value(QStringLiteral("approval")).toObject();
    if (signOffApproval.value(QStringLiteral("decision")).toString() != QStringLiteral("approve") ||
        signOffApproval.value(QStringLiteral("actorId")).toString().isEmpty() ||
        signOffApproval.value(QStringLiteral("policyId")).toString().isEmpty())
    {
        return fail(QStringLiteral("sign_off.approval is not a complete approve certificate"));
    }
    return true;
}

/// Regenerates the committed cross-surface fixture from real surface output when
/// LOOP_GOVERNED_PARITY_OUT is set. Unset (CI, normal runs) is a no-op.
void captureGovernedRecords(const QMap<QString, QJsonObject>& records)
{
    const QString directory = qEnvironmentVariable("LOOP_GOVERNED_PARITY_OUT").trimmed();
    if (directory.isEmpty())
    {
        return;
    }
    QDir().mkpath(directory);
    for (auto it = records.cbegin(); it != records.cend(); ++it)
    {
        QFile file(QDir(directory).filePath(it.key() + QStringLiteral("-receipt.json")));
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(QJsonDocument(QJsonObject{ { QStringLiteral("governed"), it.value() } }).toJson(QJsonDocument::Indented));
        }
    }
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
    void partialInspectionStaysVisibleAndNeverLooksLikeAClearDocument();
    void workspaceTransitionsKeepTheOpenDocumentBound();
    void canvasBindingClearsWhenTheDocumentCloses();
    void cancellationLeavesNoAcceptedResult();
    void adapterParityOverOneInspectionReceipt();
    void governedPublicationParityAcrossSurfaces();
    void planApprovalJourneyBindsDisplayedIdentityAndBlocksBypasses();
    void compareReviewGoldenFixtureMatchesCoreFindingDelta();
    void compareWorkspaceBlocksAStaleComparison();
    void compareWorkspaceNavigatesMaterialDeltasAfterARun();
};

void ProductOperatorLoopTest::initTestCase()
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");
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

void ProductOperatorLoopTest::partialInspectionStaysVisibleAndNeverLooksLikeAClearDocument()
{
    // #27 failure case. The representative document (bleed-missing.pdf, one page) is inspected
    // under the corpus `restriction-pages` restriction: the profile scopes its only enabled check
    // to page 2 of a one-page document, so Core cannot collect that check's evidence and reduces
    // the report to an `incomplete` verdict (reason_code `unsupported-scope`) instead of a clean
    // pass. The operator shell must present exactly that partial inspection - progress while it
    // runs, an incomplete verdict after it - and must never render a clear document.
    const QString source = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY2(QFileInfo::exists(source), source.toUtf8().constData());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject profile = pdf::exportPreflightProfile(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("loop-test-partial-inspection") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Partial inspection fixture") },
        { QStringLiteral("restrictions"), QJsonObject{ { QStringLiteral("pages"), QStringLiteral("2") } } },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("bleed") }, { QStringLiteral("amount_pt"), 9 } } } } });
    const QString profilePath = directory.filePath(QStringLiteral("partial-inspection.json"));
    {
        QFile output(profilePath);
        QVERIFY(output.open(QIODevice::WriteOnly));
        const QByteArray bytes = QJsonDocument(profile).toJson();
        QCOMPARE(output.write(bytes), bytes.size());
    }

    EditorHost host;
    QVERIFY2(host.importPreflightProfileFileUrl(QUrl::fromLocalFile(profilePath)),
             "the restricted inspection profile must import and become the selected profile");
    QVERIFY(host.selectedPreflightProfileId().endsWith(QStringLiteral("partial-inspection.json")));

    host.openFileUrl(QUrl::fromLocalFile(source));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QCOMPARE(host.pageCount(), 1);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);

    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    QSignalSpy progressSpy(preflight, &PreflightController::progressChanged);

    // The operator sees progress while the inspection is live...
    QVERIFY(host.runPreflight());
    QCOMPARE(host.preflightStateName(), QStringLiteral("running"));
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY2(!progressSpy.isEmpty(), "the operator must observe preflight progress while it runs");
    QCOMPARE(preflight->property("progress").toInt(), 100);

    // ...and a partial inspection stays visible as incomplete, never as a clear document.
    QCOMPARE(host.preflightStateName(), QStringLiteral("incomplete"));
    QVERIFY2(host.preflightStateName() != QStringLiteral("pass"),
             "a partial inspection must never present as a clean pass");
    QVERIFY(host.hasPreflightReport());
    QVERIFY2(host.preflightOperatorSummary().trimmed().contains(QStringLiteral("finish inspecting")),
             qPrintable(host.preflightOperatorSummary()));
    QVERIFY2(preflight->limitationDescription().trimmed().contains(QStringLiteral("bleed")),
             qPrintable(preflight->limitationDescription()));

    // The rendered treatment is Core's incomplete state itself, not a re-derived or pass-like one.
    const pdfquick::tokens::LoopStateVisual expected =
        pdfquick::tokens::resolvePreflightStateVisual(QStringLiteral("incomplete"));
    QCOMPARE(expected.kind, pdfquick::tokens::StateKind::Incomplete);
    const QVariantMap visual = host.preflightStateVisual();
    QCOMPARE(visual.value(QStringLiteral("kind")).toString(), pdfquick::tokens::stateKindName(expected.kind));
    QCOMPARE(visual.value(QStringLiteral("colorRole")).toString(), pdfquick::tokens::colorRoleName(expected.colorRole));
    QCOMPARE(visual.value(QStringLiteral("icon")).toString(), pdfquick::tokens::stateIconName(expected.icon));
    QCOMPARE(visual.value(QStringLiteral("accessibleName")).toString(), pdfquick::tokens::stateAccessibleName(expected.kind));
    QVERIFY(visual.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success"));
    QVERIFY(visual.value(QStringLiteral("icon")).toString() != QStringLiteral("Checkmark"));
    QVERIFY(visual.value(QStringLiteral("accessibleName")).toString() != QStringLiteral("Passed"));
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

void ProductOperatorLoopTest::governedPublicationParityAcrossSurfaces()
{
    // Issue #40 / D5: one fixture and one equivalent plan reach every governed
    // surface. Every surface publishes a receipt the parity checker accepts, and
    // the source/profile identity is shared. Plan-digest equality is asserted
    // within the action-list envelope family (CLI action-list and Editor), which
    // is the family the surfaces actually share; repair and PageMaster publish
    // different plan envelopes by design (ADR-011).
    const QString fixture = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    const QString profile = operatoracceptance::defaultProfilePath();
    const QString recipePath = governedParityDirectory() + QStringLiteral("/action-list-recipe.json");
    QVERIFY2(QFileInfo::exists(fixture), fixture.toUtf8().constData());
    QVERIFY2(QFileInfo::exists(profile), profile.toUtf8().constData());
    QVERIFY2(QFileInfo::exists(recipePath), recipePath.toUtf8().constData());

    const QString sourceSha = pdf::PDFRunIdentity::digestFile(fixture);
    QVERIFY(pdf::isPDFSha256(sourceSha));

    QJsonObject recipeObject;
    QVERIFY(readJsonObject(recipePath, &recipeObject));
    pdf::PDFActionList actionList;
    const pdf::PDFOperationResult parsed = pdf::PDFActionList::fromJson(recipeObject, &actionList);
    QVERIFY2(parsed, qPrintable(parsed.getErrorMessage()));

    QTemporaryDir outputDirectory;
    QVERIFY(outputDirectory.isValid());

    QMap<QString, QJsonObject> records;

    // 1. CLI repair: the operation-plan envelope.
    {
        const QString output = outputDirectory.filePath(QStringLiteral("repair.pdf"));
        const QString report = outputDirectory.filePath(QStringLiteral("repair-report.json"));
        QByteArray out;
        QByteArray err;
        int code = -1;
        QVERIFY(operatoracceptance::runPdfTool(pdfToolPath(), repairArguments(fixture, output, report, false, profile), &out, &err, &code));
        QVERIFY2(code == 0, qPrintable(QString::fromUtf8(err)));
        QJsonObject reportJson;
        QVERIFY(readJsonObject(report, &reportJson));
        records.insert(QStringLiteral("repair"), governedTriple(reportJson));
    }

    // 2. CLI action-list run: the action-list envelope.
    QJsonObject actionListGoverned;
    {
        const QString output = outputDirectory.filePath(QStringLiteral("action-list.pdf"));
        QByteArray out;
        QByteArray err;
        int code = -1;
        QVERIFY(operatoracceptance::runPdfTool(pdfToolPath(),
                                               { QStringLiteral("action-list"), QStringLiteral("run"), recipePath, fixture,
                                                 QStringLiteral("--output"), output,
                                                 QStringLiteral("--profile"), profile,
                                                 QStringLiteral("--console-format"), QStringLiteral("json") },
                                               &out, &err, &code));
        QVERIFY2(code == 0, qPrintable(QString::fromUtf8(err)));
        const QJsonObject envelope = QJsonDocument::fromJson(out).object();
        actionListGoverned = envelope.value(QStringLiteral("data")).toObject().value(QStringLiteral("governed")).toObject();
        records.insert(QStringLiteral("action-list"), governedTriple(actionListGoverned));
    }

    // 3. Editor Action List worker: the same action-list envelope, published by
    //    the one gateway the Editor route calls.
    QJsonObject editorGoverned;
    {
        EditorHost host;
        QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
        host.openFileUrl(QUrl::fromLocalFile(fixture));
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
        QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
        QVERIFY(host.validateActionListRecipe());
        QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
        QVERIFY(host.planActionList());
        QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
        QVERIFY(host.approveActionListPlan());
        QVERIFY(host.executeApprovedActionListPlan());
        QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("succeeded"), 120000);
        const QVariantMap signOff = host.fixSignOff();
        editorGoverned = QJsonObject::fromVariantMap(signOff.value(QStringLiteral("governed")).toMap());
        records.insert(QStringLiteral("editor"), governedTriple(editorGoverned));
    }

    // 4. PageMaster export with the same action list and preflight gate.
    QJsonObject pageMasterManifest;
    {
        const QString output = outputDirectory.filePath(QStringLiteral("pagemaster.pdf"));
        const QString manifestPath = outputDirectory.filePath(QStringLiteral("pagemaster-manifest.json"));
        pdf::PDFDocument source = readGovernedFixture(fixture);
        QVERIFY(source.getCatalog() != nullptr);
        const pdf::PDFPage* page = source.getCatalog()->getPage(0);
        QVERIFY(page != nullptr);
        const QRectF mediaBox = page->getMediaBox();
        const QSizeF sizeMM(mediaBox.width() * pdf::PDF_POINT_TO_MM, mediaBox.height() * pdf::PDF_POINT_TO_MM);

        pdf::PDFPageMasterExportJob job;
        job.assembledDocuments.push_back({ pdf::PDFDocumentManipulator::createDocumentPage(0, 0, sizeMM, pdf::PageRotation::None) });
        job.documents.emplace(0, std::move(source));
        job.documentSourceIdentities.emplace(0, identityForFile(fixture));
        job.outputFileNames.push_back(output);
        job.overwriteFiles = true;
        job.hasPreflightGate = true;
        job.preflightProfilePath = profile;
        job.forcePreflight = true;
        job.hasActionList = true;
        job.actionList = actionList;
        job.manifestPath = manifestPath;

        const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        pageMasterManifest = result.manifest;
        const QJsonObject outputEntry = result.manifest.value(QStringLiteral("outputs")).toArray().first().toObject();
        records.insert(QStringLiteral("pagemaster"), governedTriple(outputEntry.value(QStringLiteral("governed")).toObject()));
        // PageMaster publishes the action-list result alongside its governed block.
        QVERIFY(!outputEntry.value(QStringLiteral("action_list_result")).toObject().isEmpty());
    }

    // Every surface must pass the checker's rules, and the committed fixture is
    // regenerated from real surface output when a capture path is set.
    for (auto it = records.cbegin(); it != records.cend(); ++it)
    {
        QString why;
        QVERIFY2(governedRecordIsCheckerValid(it.value(), &why),
                 qPrintable(QStringLiteral("%1: %2").arg(it.key(), why)));
    }
    captureGovernedRecords(records);

    // Source identity: repair, action-list, and Editor bind the exact fixture
    // digest. PageMaster publishes a derived source-identity-list digest, so the
    // same source is asserted through its manifest's document identity.
    for (const QString& surface : { QStringLiteral("repair"), QStringLiteral("action-list"), QStringLiteral("editor") })
    {
        QCOMPARE(records.value(surface).value(QStringLiteral("approval")).toObject().value(QStringLiteral("source_sha256")).toString(), sourceSha);
    }
    const QJsonObject documentIdentities =
        pageMasterManifest.value(QStringLiteral("source_identities")).toObject().value(QStringLiteral("documents")).toArray().first().toObject();
    QCOMPARE(documentIdentities.value(QStringLiteral("identity")).toObject().value(QStringLiteral("sha256")).toString(), sourceSha);

    // Effective profile identity: the surfaces that revalidate the raw profile
    // agree. PageMaster resolves a contextual profile, so its effective digest is
    // its own resolved-profile digest (kept self-consistent by the checker pass).
    const QString profileDigest = records.value(QStringLiteral("repair"))
                                      .value(QStringLiteral("revalidation"))
                                      .toObject()
                                      .value(QStringLiteral("effective_profile_digest"))
                                      .toString();
    QVERIFY(pdf::isPDFSha256(profileDigest));
    for (const QString& surface : { QStringLiteral("action-list"), QStringLiteral("editor") })
    {
        QCOMPARE(records.value(surface).value(QStringLiteral("revalidation")).toObject().value(QStringLiteral("effective_profile_digest")).toString(), profileDigest);
    }
    QVERIFY(pdf::isPDFSha256(records.value(QStringLiteral("pagemaster")).value(QStringLiteral("revalidation")).toObject().value(QStringLiteral("effective_profile_digest")).toString()));

    // The action-list plan digest is shared across the action-list envelope family
    // (CLI action-list and Editor), which is the family the surfaces share.
    const QString actionListPlan = actionListGoverned.value(QStringLiteral("approval")).toObject().value(QStringLiteral("plan_digest")).toString();
    const QString editorPlan = editorGoverned.value(QStringLiteral("approval")).toObject().value(QStringLiteral("plan_digest")).toString();
    QVERIFY(pdf::isPDFSha256(actionListPlan));
    QCOMPARE(editorPlan, actionListPlan);
}

void ProductOperatorLoopTest::planApprovalJourneyBindsDisplayedIdentityAndBlocksBypasses()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(R"(.*ActionListPane\.qml.*)")));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString recipePath = writeBleedRecipe(directory.path());
    QVERIFY(!recipePath.isEmpty());
    const QString source = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    const QByteArray sourceDigest = operatoracceptance::fileSha256(source);
    QVERIFY(!sourceDigest.isEmpty());
    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(source));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    host.setWorkspace(EditorHost::Fix);
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(1000, 1600);
    auto pane = createWorkspacePane(engine, host, window, QStringLiteral("ActionListPane.qml"));
    QVERIFY(pane);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto activate = [&pane, &window](const char* name)
    {
        QQuickItem* button = findWorkspaceItem(pane.get(), QLatin1String(name));
        if (!button || !button->isEnabled())
        {
            return false;
        }
        button->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Space);
        return true;
    };
    const auto text = [&pane](const char* name)
    {
        QQuickItem* item = findWorkspaceItem(pane.get(), QLatin1String(name));
        return item ? item->property("text").toString() : QString();
    };
    QVERIFY(activate("validateActionListButton"));
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(activate("planActionListButton"));
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    const QVariantMap displayed = host.fixPlanIdentity();
    const QString planDigest = displayed.value(QStringLiteral("planDigest")).toString();
    const QString inputIdentity = displayed.value(QStringLiteral("sourceSha256")).toString();
    QCOMPARE(inputIdentity.size(), 64);
    QCOMPARE(inputIdentity, QString::fromLatin1(sourceDigest.toHex()));
    const QString revision = displayed.value(QStringLiteral("plannedRevision")).toString();
    QVERIFY(text("fixPlanIdentityLabel").contains(planDigest));
    QVERIFY(text("fixPlanIdentityLabel").contains(inputIdentity));
    QTRY_VERIFY(text("fixPlannedStep_0").contains(QStringLiteral("add-bleed")));
    const QVariantMap plannedStep = host.fixPreview().value(QStringLiteral("plannedSteps")).toList().front().toMap();
    QVERIFY(text("fixPlannedStep_0").contains(QString::fromUtf8(QJsonDocument::fromVariant(plannedStep.value(QStringLiteral("parameters"))).toJson(QJsonDocument::Compact))));
    QVERIFY(text("fixPlannedStep_0").contains(QString::fromUtf8(QJsonDocument::fromVariant(plannedStep.value(QStringLiteral("impact"))).toJson(QJsonDocument::Compact))));
    QVERIFY(text("fixPlannedStep_0").contains(QString::fromUtf8(QJsonDocument::fromVariant(plannedStep.value(QStringLiteral("scope"))).toJson(QJsonDocument::Compact))));

    QJSValue qmlHost = engine.newQObject(&host);
    QQmlEngine::setObjectOwnership(&host, QQmlEngine::CppOwnership);
    QVERIFY(!qmlHost.property(QStringLiteral("runActionList")).call().toBool());
    QVERIFY(qmlHost.property(QStringLiteral("approveActionListPlan")).call().isError());
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!findWorkspaceItem(pane.get(), QLatin1String("confirmActionListButton")));
    QVERIFY(qmlHost.property(QStringLiteral("confirmActionListPlan")).isUndefined());
    QVERIFY(!activate("fixExecutePlanButton"));
    QVERIFY(activate("fixRejectPlanButton"));
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("rejected"));
    QVERIFY(!qmlHost.property(QStringLiteral("runActionList")).call().toBool());
    QCOMPARE(operatoracceptance::fileSha256(source), sourceDigest);

    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("stale"));
    QVERIFY(!activate("fixApprovePlanButton"));
    QVERIFY(!qmlHost.property(QStringLiteral("approveActionListPlan")).call({ QJSValue(planDigest), QJSValue(inputIdentity), QJSValue(revision) }).toBool());
    QVERIFY(!qmlHost.property(QStringLiteral("runActionList")).call().toBool());

    QVERIFY(host.setActionListStepParameter(0, QStringLiteral("bleed_mm"), 4));
    QVERIFY(activate("saveActionListRecipeButton"));
    QVERIFY(activate("validateActionListButton"));
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(activate("planActionListButton"));
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(!qmlHost.property(QStringLiteral("approveActionListPlan")).call({ QJSValue(planDigest), QJSValue(inputIdentity), QJSValue(revision) }).toBool());
    const QVariantMap current = host.fixPlanIdentity();
    QVERIFY(current.value(QStringLiteral("planDigest")).toString() != planDigest);
    QVERIFY(!qmlHost.property(QStringLiteral("approveActionListPlan")).call({ QJSValue(planDigest), QJSValue(inputIdentity), QJSValue(current.value(QStringLiteral("plannedRevision")).toString()) }).toBool());
    QVERIFY(!qmlHost.property(QStringLiteral("approveActionListPlan")).call({ QJSValue(current.value(QStringLiteral("planDigest")).toString()), QJSValue(QString(64, QLatin1Char('0'))), QJSValue(current.value(QStringLiteral("plannedRevision")).toString()) }).toBool());
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(activate("fixApprovePlanButton"));
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("reviewedPlanDigest")), current.value(QStringLiteral("planDigest")));
    QVERIFY(activate("fixExecutePlanButton"));
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("succeeded"), 120000);
    const QVariantMap signOff = host.fixSignOff();
    const QString publishedDigest = signOff.value(QStringLiteral("publishedSha256")).toString();
    QCOMPARE(publishedDigest.size(), 64);
    QVERIFY(text("fixSignOffSummary").contains(publishedDigest));
    QVERIFY(text("fixSignOffSummary").contains(current.value(QStringLiteral("planDigest")).toString()));
    const QVariantMap recheck = host.fixRecheck();
    QVERIFY(recheck.value(QStringLiteral("available")).toBool());
    QVERIFY(text("fixRecheckSummary").contains(recheck.value(QStringLiteral("verdictState")).toString()));
    QVERIFY(!activate("fixExecutePlanButton"));
    QCOMPARE(operatoracceptance::fileSha256(source), sourceDigest);
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

    const pdf::PDFRepairFindingDelta again = pdf::computeFindingDelta(before, after);
    QCOMPARE(again.resolvedFindingIds, delta.resolvedFindingIds);
    QCOMPARE(again.unchangedFindingIds, delta.unchangedFindingIds);
    QCOMPARE(again.introducedFindingIds, delta.introducedFindingIds);

    const QString sourceDigest = QString(63, QLatin1Char('a')) + QLatin1Char('1');
    const QString candidateDigest = QString(63, QLatin1Char('a')) + QLatin1Char('2');
    const QString publishedDigest = QString(63, QLatin1Char('a')) + QLatin1Char('3');
    const QString planDigest = QString(64, QLatin1Char('b'));
    const QString rationale = QStringLiteral("Retain the exact source bytes");
    const QString riskMessage = QStringLiteral("Font embedding remains unresolved");
    CompareReviewProjection projection;
    projection.review = {
        { QStringLiteral("available"), true },
        { QStringLiteral("blocked"), false },
        { QStringLiteral("blockedReason"), QString() },
        { QStringLiteral("before"), QVariantMap{
                                        { QStringLiteral("sourceSha256"), sourceDigest },
                                        { QStringLiteral("plannedRevision"), planDigest } } },
        { QStringLiteral("after"), QVariantMap{ { QStringLiteral("candidateSha256"), candidateDigest }, { QStringLiteral("publishedSha256"), publishedDigest }, { QStringLiteral("reviewDecision"), QStringLiteral("approved") } } },
        { QStringLiteral("plan"), QVariantMap{ { QStringLiteral("planDigest"), planDigest }, { QStringLiteral("reviewedPlanDigest"), planDigest }, { QStringLiteral("publishedPlanDigest"), planDigest }, { QStringLiteral("recipeId"), QStringLiteral("compare-review") }, { QStringLiteral("planIsCurrent"), true } } },
        { QStringLiteral("findingDelta"), QVariantMap{ { QStringLiteral("compared"), delta.compared }, { QStringLiteral("resolved"), delta.resolvedFindingIds }, { QStringLiteral("unchanged"), delta.unchangedFindingIds }, { QStringLiteral("introduced"), delta.introducedFindingIds }, { QStringLiteral("incomplete"), delta.incompleteFindingIds } } },
        { QStringLiteral("preserved"), QVariantMap{ { QStringLiteral("declaredChangeAttributes"), QStringList{ QStringLiteral("bleed") } }, { QStringLiteral("unchangedFindings"), delta.unchangedFindingIds }, { QStringLiteral("carriedForwardFindings"), delta.unchangedFindingIds }, { QStringLiteral("savePolicyMode"), QStringLiteral("save-as") }, { QStringLiteral("savePolicyRationale"), rationale } } },
        { QStringLiteral("unresolvedRisk"), QVariantMap{ { QStringLiteral("level"), QStringLiteral("high") }, { QStringLiteral("introducedFindings"), delta.introducedFindingIds }, { QStringLiteral("incompleteFindings"), delta.incompleteFindingIds }, { QStringLiteral("messages"), QStringList{ riskMessage } } } },
        { QStringLiteral("materialDeltas"), QVariantList{ QVariantMap{ { QStringLiteral("kind"), QStringLiteral("resolved") }, { QStringLiteral("findingId"), delta.resolvedFindingIds.front() }, { QStringLiteral("stepIndex"), 0 } } } }
    };
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(360, 480);
    auto pane = createWorkspacePane(engine, projection, window);
    QVERIFY(pane);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto text = [&pane](const char* name)
    {
        QQuickItem* item = findWorkspaceItem(pane.get(), QLatin1String(name));
        return item ? item->property("text").toString() : QString();
    };
    QVERIFY(text("compareArtifacts").contains(sourceDigest));
    QVERIFY(text("compareArtifacts").contains(candidateDigest));
    QVERIFY(text("compareArtifacts").contains(publishedDigest));
    auto* artifacts = pane->findChild<QQuickItem*>(QStringLiteral("compareArtifacts"));
    QVERIFY(artifacts);
    QTRY_VERIFY(artifacts->property("contentWidth").toReal() <= artifacts->width());
    QVERIFY(text("comparePlanIdentity").contains(planDigest));
    QCOMPARE(text("compareReviewIdentity").count(planDigest), 2);
    QVERIFY(text("compareDeltaSummary").contains(QStringLiteral("cleared 1")));
    QVERIFY(text("compareDeltaSummary").contains(QStringLiteral("remaining 1")));
    QVERIFY(text("compareDeltaSummary").contains(QStringLiteral("introduced 1")));
    QVERIFY(text("compareDeltaIdentity_0").contains(delta.resolvedFindingIds.front()));
    QVERIFY(text("comparePreservedDetails").contains(delta.unchangedFindingIds.front()));
    QVERIFY(text("comparePreservedDetails").contains(rationale));
    QVERIFY(text("compareRiskDetails").contains(delta.introducedFindingIds.front()));
    QVERIFY(text("compareRiskDetails").contains(riskMessage));
    auto* scroll = pane->findChild<QQuickItem*>(QStringLiteral("compareDetailsScroll"));
    QVERIFY(scroll);
    QTRY_VERIFY(scroll->property("contentHeight").toReal() > scroll->height());
    auto* navigate = findWorkspaceItem(pane.get(), QStringLiteral("compareNavigateDelta_0"));
    QVERIFY(navigate);
    QVERIFY(navigate->isEnabled());
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

    const QVariantMap before = review.value(QStringLiteral("before")).toMap();
    QVERIFY(!before.value(QStringLiteral("sourceSha256")).toString().isEmpty());
    QCOMPARE(before.value(QStringLiteral("sourceSha256")).toString(),
             host.fixPlanIdentity().value(QStringLiteral("sourceSha256")).toString());
    QCOMPARE(review.value(QStringLiteral("plan")).toMap().value(QStringLiteral("planDigest")).toString(),
             host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString());

    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    auto pane = createWorkspacePane(engine, host, window);
    QVERIFY(pane);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* guard = pane->findChild<QQuickItem*>(QStringLiteral("compareStaleGuard"));
    auto* reason = pane->findChild<QQuickItem*>(QStringLiteral("compareBlockedReason"));
    auto* artifacts = pane->findChild<QQuickItem*>(QStringLiteral("compareArtifacts"));
    QVERIFY(guard);
    QVERIFY(reason);
    QVERIFY(artifacts);
    QVERIFY(!guard->isVisible());
    const QString displayedArtifacts = artifacts->property("text").toString();
    QVERIFY(displayedArtifacts.contains(before.value(QStringLiteral("sourceSha256")).toString()));

    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    const QVariantMap stale = host.compareReview();
    QVERIFY(stale.value(QStringLiteral("available")).toBool());
    QVERIFY2(stale.value(QStringLiteral("blocked")).toBool(),
             qPrintable(stale.value(QStringLiteral("blockedReason")).toString()));
    QCOMPARE(stale.value(QStringLiteral("lifecycleStateName")).toString(), QStringLiteral("stale"));
    QVERIFY(!stale.value(QStringLiteral("blockedReason")).toString().trimmed().isEmpty());
    QVERIFY(!host.navigateCompareDelta(0));
    QTRY_VERIFY(guard->isVisible());
    QCOMPARE(reason->property("text").toString(), stale.value(QStringLiteral("blockedReason")).toString());
    QAccessibleInterface* accessibleReason = QAccessible::queryAccessibleInterface(reason);
    QVERIFY(accessibleReason);
    QCOMPARE(accessibleReason->text(QAccessible::Description), reason->property("text").toString());
    const QString staleSource = stale.value(QStringLiteral("before")).toMap().value(QStringLiteral("sourceSha256")).toString();
    QVERIFY(artifacts->property("text").toString().contains(staleSource.isEmpty() ? QStringLiteral("not available") : staleSource));
    QCOMPARE(stale.value(QStringLiteral("plan")).toMap().value(QStringLiteral("planDigest")),
             review.value(QStringLiteral("plan")).toMap().value(QStringLiteral("planDigest")));
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

    host.setWorkspace(EditorHost::Compare);
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(960, 900);
    auto pane = createWorkspacePane(engine, host, window);
    QVERIFY(pane);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* navigate = findWorkspaceItem(pane.get(), QStringLiteral("compareNavigateDelta_0"));
    QVERIFY(navigate);
    QVERIFY(navigate->isEnabled());
    navigate->forceActiveFocus();
    QTRY_VERIFY(navigate->hasActiveFocus());
    QTest::keyClick(&window, Qt::Key_Space);
    QTRY_COMPARE(host.workspace(), EditorHost::Inspect);

    auto* controller = qobject_cast<pdfinteraction::ActionListController*>(host.actionList());
    QVERIFY(controller);
    pdf::PDFActionListExecutionResult replacement = controller->result();
    replacement.status = QStringLiteral("planned");
    replacement.planDigest = QString(64, QLatin1Char('c'));
    const QString revision = controller->documentRevision();
    const QString replacementJob = QStringLiteral("compare-replacement-plan");
    controller->beginRun(pdfinteraction::ActionListController::State::Planning,
                         controller->documentKey(), revision, controller->recipeId(),
                         controller->recipeHash(), QString(), replacementJob);
    QVERIFY(controller->acceptPlan(replacementJob, revision, replacement));
    const QVariantMap mismatched = host.compareReview();
    QVERIFY(mismatched.value(QStringLiteral("blocked")).toBool());
    QVERIFY(!host.approveActionListPlan(replacement.planDigest, replacement.sourceSha256, revision));
    QVERIFY(!host.approveActionListPlan());
    QVERIFY(!host.runActionList());
    const QVariantMap mismatchedPlan = mismatched.value(QStringLiteral("plan")).toMap();
    QCOMPARE(mismatchedPlan.value(QStringLiteral("planDigest")).toString(), replacement.planDigest);
    QCOMPARE(mismatchedPlan.value(QStringLiteral("reviewedPlanDigest")),
             review.value(QStringLiteral("plan")).toMap().value(QStringLiteral("reviewedPlanDigest")));
    auto* reason = pane->findChild<QQuickItem*>(QStringLiteral("compareBlockedReason"));
    QVERIFY(reason);
    QTRY_COMPARE(reason->property("text").toString(), mismatched.value(QStringLiteral("blockedReason")).toString());
    QTRY_VERIFY(findWorkspaceItem(pane.get(), QStringLiteral("compareNavigateDelta_0")));
    navigate = findWorkspaceItem(pane.get(), QStringLiteral("compareNavigateDelta_0"));
    QVERIFY(!navigate->isEnabled());
    QVERIFY(!host.navigateCompareDelta(0));
    host.setWorkspace(EditorHost::Compare);
    navigate->forceActiveFocus();
    QTest::keyClick(&window, Qt::Key_Space);
    QCOMPARE(host.workspace(), EditorHost::Compare);

    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QTRY_VERIFY(host.compareReview().value(QStringLiteral("blocked")).toBool());
    navigate = findWorkspaceItem(pane.get(), QStringLiteral("compareNavigateDelta_0"));
    QVERIFY(!navigate || !navigate->isEnabled());
    QVERIFY(!host.navigateCompareDelta(0));
    host.setWorkspace(EditorHost::Compare);
    QTest::keyClick(&window, Qt::Key_Space);
    QCOMPARE(host.workspace(), EditorHost::Compare);
}

QTEST_MAIN(ProductOperatorLoopTest)

#include "tst_productoperatorloop.moc"
