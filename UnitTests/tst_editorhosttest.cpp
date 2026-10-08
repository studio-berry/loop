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

#include <QtTest>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <memory>

#include "processoutputcapture.h"

#include "documentviewsession.h"
#include "editorhost.h"
#include "loopstatevisual.h"
#include "looptokens.h"

#include "pdfapplicationidentity.h"
#include "pdfblockingthreadguard.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfoperationhistorystore.h"
#include "pdfsettings.h"
#include "pdfworkloadenvelope.h"
#include "preflightprofileresolver.h"

#include "hittestsource.h"
#include "inputintent.h"
#include "interactioncontroller.h"
#include "pagesurfacecoordinator.h"
#include "viewportcontroller.h"

namespace
{

/// Minimal scripted source for fencing proof, mirroring tst_interactioncontrollertest.
/// Returns targets the test wrote down so hover transitions are deterministic
/// without a corpus.
class ScriptedHitTestSource final : public pdfinteraction::IHitTestSource
{
public:
    QList<pdfinteraction::InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const override
    {
        QList<pdfinteraction::InteractionTarget> hits;
        for (const pdfinteraction::InteractionTarget& target : targets)
        {
            if (target.pageIndex == pageIndex && target.pageBounds.contains(pagePoint))
            {
                hits.push_back(target);
            }
        }
        return hits;
    }

    QList<pdfinteraction::InteractionTarget> targets;
};

pdfinteraction::InteractionTarget makeFindingTarget(const QString& id, const QRectF& bounds, int pageIndex = 0)
{
    pdfinteraction::InteractionTarget target;
    target.kind = pdfinteraction::InteractionTargetKind::Finding;
    target.pageIndex = pageIndex;
    target.id = id;
    target.pageBounds = bounds;
    return target;
}

pdfinteraction::PointerIntent makeMoveIntent(QPoint positionPx, quint64 sequence)
{
    pdfinteraction::PointerIntent intent;
    intent.stamp.sequence = sequence;
    intent.stamp.monotonicNs = qint64(sequence) * 1000000;
    intent.action = pdfinteraction::PointerAction::Move;
    intent.positionPx = positionPx;
    intent.button = Qt::NoButton;
    intent.buttons = Qt::NoButton;
    intent.modifiers = Qt::NoModifier;
    return intent;
}

// --- GUI-to-CLI preflight parity support (issue #195) -----------------------
//
// #195's anti-divergence test compares the two preflight surfaces, so the CLI
// oracle invocation and the report normalisation below are copies of
// UnitTestsPreflightCorpus (tst_preflightcorpus.cpp) rather than a second policy
// of this file's own: one PdfTool runner shape, one set of normalised fields.

QString preflightFixturesDir()
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures");
}

QString preflightSourceDir()
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR);
}

/// Runs the built `PdfTool preflight` and returns the report from the JSON
/// envelope's data.report. See PreflightCorpusTest::runPreflight().
void runPdfToolPreflight(const QString& pdfPath, const QString& profilePath, QJsonObject& report, int& exitCode)
{
    QProcess process;
    // PdfTool constructs a QGuiApplication; force the offscreen platform so the
    // child process can start on headless CI runners with no X display.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral(PDFTOOL_EXECUTABLE_PATH),
                  { QStringLiteral("preflight"),
                    pdfPath,
                    QStringLiteral("--profile"),
                    profilePath,
                    QStringLiteral("--console-format"),
                    QStringLiteral("json") });
    QByteArray stdOut;
    QByteArray stdErr;
    QVERIFY2(test_support::waitForFinishedAndCapture(process, 30000, stdOut, stdErr),
             qPrintable(QStringLiteral("PdfTool preflight timed out: %1\nstderr: %2").arg(process.errorString(), QString::fromUtf8(stdErr))));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdOut, &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("Invalid report JSON: %1\nstderr: %2").arg(parseError.errorString(), QString::fromUtf8(stdErr))));
    QVERIFY2(document.isObject(), "result JSON must be a top-level object");

    const QJsonObject envelope = document.object();
    QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(envelope.value(QStringLiteral("command")).toString(), QStringLiteral("preflight"));
    QCOMPARE(envelope.value(QStringLiteral("exit_code")).toInt(), process.exitCode());
    report = envelope.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
    QVERIFY2(!report.isEmpty(), "preflight result must contain data.report");
    exitCode = process.exitCode();
}

/// See PreflightCorpusTest::normalizeReport(): strip the fields that legitimately
/// vary between runs, checkouts and surfaces and are not part of the check
/// behavior both surfaces must agree on.
QJsonObject normalizePreflightReport(QJsonObject report)
{
    report.remove(QStringLiteral("engine_version"));
    report.remove(QStringLiteral("pdf"));
    report.remove(QStringLiteral("profile_resolution"));
    report.remove(QStringLiteral("document_revision_digest"));
    report.remove(QStringLiteral("effective_profile_digest"));
    report.remove(QStringLiteral("profile_identity"));
    report.remove(QStringLiteral("coverage_scope"));
    report.remove(QStringLiteral("variable_bindings"));
    report.remove(QStringLiteral("decisions"));
    for (const QString& section : { QStringLiteral("errors"), QStringLiteral("warnings") })
    {
        QJsonArray findings = report.value(section).toArray();
        for (int index = 0; index < findings.size(); ++index)
        {
            QJsonObject finding = findings.at(index).toObject();
            finding.remove(QStringLiteral("id"));
            finding.remove(QStringLiteral("evidence_ids"));
            findings.replace(index, finding);
        }
        report.insert(section, findings);
    }
    return report;
}

void parsePreflightReport(const QByteArray& bytes, const QString& label, QJsonObject& report)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError,
             qPrintable(QStringLiteral("%1 is not valid JSON: %2").arg(label, parseError.errorString())));
    QVERIFY2(document.isObject(), qPrintable(QStringLiteral("%1 must be a top-level JSON object").arg(label)));
    report = document.object();
}

/// Names the first few differing lines so a divergence is readable in the log.
/// The assertion stays whole-document equality; this only explains a failure.
QString describePreflightReportDifference(const QString& guiText, const QString& cliText)
{
    const QStringList guiLines = guiText.split(QLatin1Char('\n'));
    const QStringList cliLines = cliText.split(QLatin1Char('\n'));
    const int lineCount = qMax(guiLines.size(), cliLines.size());
    QStringList differences;
    for (int line = 0; line < lineCount && differences.size() < 12; ++line)
    {
        const QString guiLine = line < guiLines.size() ? guiLines.at(line) : QStringLiteral("<missing>");
        const QString cliLine = line < cliLines.size() ? cliLines.at(line) : QStringLiteral("<missing>");
        if (guiLine != cliLine)
        {
            differences << QStringLiteral("line %1: GUI %2\nline %1: CLI %3").arg(line + 1).arg(guiLine.trimmed(), cliLine.trimmed());
        }
    }
    return differences.join(QLatin1Char('\n'));
}

}   // namespace

class EditorHostTest : public QObject
{
    Q_OBJECT

private slots:
    void teardownClearsTheInteractiveThreadRegistration();
    void startsWithNoDocument();
    void exposesCatalogDescriptorsWithoutMutating();
    void navigationCommandsStayDisabledUntilOpen();
    void sessionTeardownDrainsWorkersBeforeAdapters();
    void preflightRunsOffInteractiveThread();
    void preflightStateVisualIsNotCheckedBeforeARun();
    void preflightFencesCompletionsThatLostTheirRequestIdentity();
    void actionListFencesCompletionsThatLostTheirRequestIdentity();
    void exportedPreflightReportMatchesPdfToolForTheSameInputs();
    void restrictedPreflightReportMatchesPdfTool();
    void importValidProfileAddsDigest();
    void importDigestMismatchProfileIsRejected();
    void saveProfileForkRecordsDerivedFrom();
    void openLargeDocument();

    // Workspace surfaces (#586).
    void fixWorkspacePresentsIdleLifecycleAndRefusesToArm();
    void fixReviewBindsToThePlannedDigestAndTheCurrentRevision();
    void moveSelectionProposesAPageBoxMoveInTheFixWorkspace();
    void completedDragOfARefusedKindIsReportedAndChangesNothing();
    void shippedRecipeIsListedButNeverSelectedByDefault();
    void shippedPageBoxRecipeRunsDefaultMoveThroughPlanApprovalAndRevalidation();
    void operatorRecipeWinsOverShippedRecipe();
    void invalidOperatorRecipeStopsMoveWithoutShippedFallback();
    void fixJourneyPublishesOnlyAnApprovedPlanBoundToTheDisplayedIdentity();
    void translatePageBoxRefusesAMoveThatIntroducesABlockingFinding();
    void executeApprovedActionListPlanRefusesAnUnreviewedPlan();
    void fixRollbackReturnsToARecordedRevision();
    void previewFidelityNamesTheOriginAndSwitchesExplicitly();
    void refusedRenderCannotBecomeAuthoritativeEvidence();
};

void EditorHostTest::teardownClearsTheInteractiveThreadRegistration()
{
    QVERIFY(!pdf::PDFBlockingThreadGuard::isInteractiveThreadRegistered());
    {
        EditorHost host;
        QVERIFY(pdf::PDFBlockingThreadGuard::isInteractiveThreadRegistered());
        QVERIFY(pdf::PDFBlockingThreadGuard::isCurrentThreadInteractive());
    }
    // The host registered its owning thread; it must take the registration with
    // it, or a host recreated in the same process leaves a stale one behind that
    // keeps refusing synchronous blocking work.
    QVERIFY(!pdf::PDFBlockingThreadGuard::isInteractiveThreadRegistered());
}

void EditorHostTest::startsWithNoDocument()
{
    EditorHost host;
    QCOMPARE(host.documentState(), QStringLiteral("empty"));
    QVERIFY(!host.hasDocument());
    QCOMPARE(host.pageCount(), 0);

    const QVariantList profiles = host.preflightProfiles();
    QVERIFY2(!profiles.isEmpty(), "the GUI must expose at least one bundled preflight profile");
    const QVariantMap defaultProfile = profiles.constFirst().toMap();
    QVERIFY(defaultProfile.value(QStringLiteral("valid")).toBool());
    QVERIFY(!defaultProfile.value(QStringLiteral("digest")).toString().isEmpty());
    QVERIFY(defaultProfile.value(QStringLiteral("diagnostic")).toString().isEmpty());
    QVERIFY(host.actionList());
    QCOMPARE(host.actionListStateName(), QStringLiteral("idle"));
    QVERIFY(!host.repairOperations().isEmpty());
    QVERIFY(!host.planActionList());
}

void EditorHostTest::exposesCatalogDescriptorsWithoutMutating()
{
    EditorHost host;
    const QVariantList descriptors = host.commandDescriptors();
    QVERIFY(descriptors.size() > 100);

    bool sawOpen = false;
    for (const QVariant& entryVariant : descriptors)
    {
        const QVariantMap entry = entryVariant.toMap();
        if (entry.value(QStringLiteral("id")).toString() == QStringLiteral("actionOpen"))
        {
            sawOpen = true;
            QVERIFY(entry.value(QStringLiteral("implemented")).toBool());
            QVERIFY(!host.shortcutForCommand(QStringLiteral("actionOpen")).isEmpty() || entry.contains(QStringLiteral("shortcut")));
        }
    }
    QVERIFY(sawOpen);
}

void EditorHostTest::navigationCommandsStayDisabledUntilOpen()
{
    EditorHost host;
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionGoToNextPage")));
    QCOMPARE(host.invokeCommand(QStringLiteral("actionGoToNextPage")), quint64(0));
}

void EditorHostTest::sessionTeardownDrainsWorkersBeforeAdapters()
{
    auto session = std::make_unique<DocumentViewSession>();
    DocumentViewSession* rawSession = session.get();
    std::atomic_bool started = false;
    std::atomic_bool adapterReached = false;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Background;
    rawSession->scheduler().submit(spec,
                                   [rawSession, &started, &adapterReached](pdf::PDFJobContext& context)
                                   {
                                       started.store(true, std::memory_order_release);
                                       while (!context.isCancellationRequested())
                                       {
                                           QThread::yieldCurrentThread();
                                       }

                                       // The session destructor must join this
                                       // work before destroying the renderer.
                                       rawSession->renderer().shedPrefetchAndQuality();
                                       adapterReached.store(true, std::memory_order_release);
                                   });

    QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 1000);
    session.reset();
    QVERIFY(adapterReached.load(std::memory_order_acquire));
}

void EditorHostTest::preflightRunsOffInteractiveThread()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preflight.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.runPreflight());
    QCOMPARE(host.preflightStateName(), QStringLiteral("running"));
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 30000);
    QVERIFY(host.preflightStateName() != QStringLiteral("error"));
    QCOMPARE(host.preflight()->property("progress").toInt(), 100);

    const QString historyPath =
        QDir(QFileInfo(path).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                        { return event.kind == pdf::PDFOperationHistoryEventKind::DocumentOpened; }));
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                        { return event.kind == pdf::PDFOperationHistoryEventKind::PreflightRun &&
                                 event.status == pdf::PDFOperationHistoryStatus::Accepted; }));
    QVERIFY(history.verify().verified);
}

void EditorHostTest::preflightStateVisualIsNotCheckedBeforeARun()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preflight-visual.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // Before any run the badge must say "not checked" and must not be a pass (#195 acceptance 1 and 7).
    const QVariantMap before = host.preflightStateVisual();
    QCOMPARE(before.value(QStringLiteral("kind")).toString(), QStringLiteral("NotChecked"));
    QVERIFY2(before.contains(QStringLiteral("kind")) && before.contains(QStringLiteral("colorRole")) && before.contains(QStringLiteral("icon")) && before.contains(QStringLiteral("accessibleName")),
             "the visual must carry the full canonical treatment for QML to render");
    QCOMPARE(before.value(QStringLiteral("accessibleName")).toString(), QStringLiteral("Not checked"));
    QCOMPARE(host.preflightCertificateStateName(), QStringLiteral("not-certified"));
    QCOMPARE(host.preflightCertificateStateVisual().value(QStringLiteral("kind")).toString(), QStringLiteral("NotChecked"));

    // The colour is resolved from the visual's own colour role, never chosen by the QML child. It must
    // be a real colour and it must not be the pass colour.
    const QColor stateColor = host.preflightStateColor();
    QVERIFY2(stateColor.isValid(), "the state colour must be a resolved QColor, not an unset one");

    const pdfquick::tokens::LoopTheme theme =
        host.highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    QCOMPARE(stateColor, pdfquick::tokens::color(pdfquick::tokens::ColorRole::StateNotChecked, theme));
    QVERIFY(stateColor != pdfquick::tokens::color(pdfquick::tokens::ColorRole::Success, theme));

    // A completed run keeps the same shape, but the values must agree with the state Core actually
    // reached. This half used to assert only key presence and a non-empty `kind`, which every
    // reachable state satisfies - a regression that produced the wrong post-run visual still passed.
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);

    const QString stateName = host.preflightStateName();
    const QVariantMap after = host.preflightStateVisual();
    QVERIFY2(after.contains(QStringLiteral("kind")) && after.contains(QStringLiteral("colorRole")) && after.contains(QStringLiteral("icon")) && after.contains(QStringLiteral("accessibleName")),
             "the visual must carry the full canonical treatment for QML to render");

    const pdfquick::tokens::LoopStateVisual expected = pdfquick::tokens::resolvePreflightStateVisual(stateName);

    // The visual is Core's own state rendered, never re-derived: only a real pass may look like one.
    const bool coreSaysPass = stateName == QStringLiteral("pass");
    QCOMPARE(after.value(QStringLiteral("kind")).toString() == QStringLiteral("Passed"), coreSaysPass);
    if (!coreSaysPass)
    {
        QVERIFY(after.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success"));
        QVERIFY(after.value(QStringLiteral("icon")).toString() != QStringLiteral("Checkmark"));
        QVERIFY(after.value(QStringLiteral("accessibleName")).toString() != QStringLiteral("Passed"));
    }

    // Every field is the one the same map chose for the state name we just read...
    QCOMPARE(after.value(QStringLiteral("kind")).toString(), pdfquick::tokens::stateKindName(expected.kind));
    QCOMPARE(after.value(QStringLiteral("colorRole")).toString(), pdfquick::tokens::colorRoleName(expected.colorRole));
    QCOMPARE(after.value(QStringLiteral("icon")).toString(), pdfquick::tokens::stateIconName(expected.icon));
    QCOMPARE(after.value(QStringLiteral("accessibleName")).toString(), pdfquick::tokens::stateAccessibleName(expected.kind));

    // ...and the resolved colour is color(role, theme) for that same role, exactly as the pre-run
    // half asserts above.
    QCOMPARE(host.preflightStateColor(), pdfquick::tokens::color(expected.colorRole, theme));
}

void EditorHostTest::exportedPreflightReportMatchesPdfToolForTheSameInputs()
{
    // #195 test strategy: "CLI/GUI parity ... assert the exported JSON is byte-identical to PdfTool
    // preflight output. This is the core anti-divergence test." UnitTestsPreflightCorpus pins the CLI
    // against the committed snapshots; this pins the GUI against the CLI, so the report the operator
    // exports cannot drift from what the tool reports for the same document and profile.
    //
    // color-rgb is the fixture of choice because it is one of the loop-default.json cases that fails
    // with findings, so the comparison covers populated errors and warnings instead of two empty
    // arrays, and loop-default.json is the one profile both surfaces can be given: the Editor ships it
    // as the bundled :/profiles/loop-default.json (LoopEditor/app.qrc aliases exactly the
    // loop-preflight/profiles/loop-default.json the CLI reads from disk).
    const QString fixtureId = QStringLiteral("color-rgb");
    const QString documentPath = QDir(preflightFixturesDir()).filePath(fixtureId + QStringLiteral(".pdf"));
    const QString profilePath = QDir(preflightSourceDir()).filePath(QStringLiteral("profiles/loop-default.json"));
    const QString bundledProfileId = QStringLiteral(":/profiles/loop-default.json");

    // The fixture is committed (see `git ls-files loop-preflight/testdata/fixtures`), so its absence
    // can only mean a broken or sparse checkout - exactly when an anti-divergence guard must fail
    // rather than skip. A QSKIP here let this row pass having compared nothing: QtTest's exit code
    // counts only failing rows, so the slot skipped, the process exited 0 and ctest reported the
    // suite as passed.
    QVERIFY2(QFile::exists(documentPath) && QFile::exists(profilePath),
             qPrintable(QStringLiteral("corpus fixture or profile missing; a deployed checkout must carry both, "
                                       "so this is a broken/sparse checkout rather than a reason to skip the parity guard. "
                                       "Regenerate with LoopGenerateFixtures and commit the output (see "
                                       "loop-preflight/README.md, 'Golden corpus & CI'). missing: document='%1' profile='%2'")
                            .arg(documentPath, profilePath)));

    // "The same profile" has to mean the same bytes, not the same name: the GUI can only select the
    // BUNDLED profile while the CLI is handed the file on disk.
    QFile bundledProfile(bundledProfileId);
    QVERIFY2(bundledProfile.open(QIODevice::ReadOnly), "the bundled loop-default profile is missing from the Editor resources");
    QFile diskProfile(profilePath);
    QVERIFY(diskProfile.open(QIODevice::ReadOnly));
    QCOMPARE(bundledProfile.readAll(), diskProfile.readAll());

    // 1. The CLI's report, from the built PdfTool.
    QJsonObject cliReport;
    int cliExitCode = -1;
    runPdfToolPreflight(documentPath, profilePath, cliReport, cliExitCode);
    QVERIFY(!cliReport.isEmpty());

    // 2. The GUI's report, through the export path the operator uses:
    //    EditorHost::exportPreflightReportFileUrl -> PreflightController::serializedReport.
    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);

    if (host.selectedPreflightProfileId() != bundledProfileId)
    {
        QVERIFY2(host.selectPreflightProfile(bundledProfileId),
                 qPrintable(QStringLiteral("the bundled loop-default profile must be selectable; selected '%1'")
                                .arg(host.selectedPreflightProfileId())));
    }
    QCOMPARE(host.selectedPreflightProfileId(), bundledProfileId);

    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY2(host.hasPreflightReport(),
             qPrintable(QStringLiteral("no report to export: state=%1 summary=%2").arg(host.preflightStateName(), host.preflightOperatorSummary())));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString guiReportPath = directory.filePath(QStringLiteral("gui-report.json"));
    QVERIFY2(host.exportPreflightReportFileUrl(QUrl::fromLocalFile(guiReportPath)),
             qPrintable(QStringLiteral("exporting the report failed: state=%1 summary=%2")
                            .arg(host.preflightStateName(), host.preflightOperatorSummary())));

    QFile guiFile(guiReportPath);
    QVERIFY(guiFile.open(QIODevice::ReadOnly));
    const QByteArray guiReport = guiFile.readAll();
    QVERIFY2(!guiReport.isEmpty(), "the exported report is empty");

    QJsonObject guiReportObject;
    parsePreflightReport(guiReport, QStringLiteral("the exported GUI report"), guiReportObject);

    // Both sides go through the corpus test's own normalisation and are then compared as whole
    // documents, so a difference anywhere - a check status, a finding field, a section the other
    // surface does not write - fails this row.
    const QByteArray guiNormalized = QJsonDocument(normalizePreflightReport(guiReportObject)).toJson(QJsonDocument::Indented);
    const QByteArray cliNormalized = QJsonDocument(normalizePreflightReport(cliReport)).toJson(QJsonDocument::Indented);

    // Normalise EOLs so Windows checkouts (eol=crlf) match QJsonDocument's LF output, as the corpus
    // test does for its snapshots.
    const auto normalizeNewlines = [](QByteArray data)
    {
        data.replace("\r\n", "\n");
        data.replace('\r', '\n');
        return data;
    };

    const QString guiText = QString::fromUtf8(normalizeNewlines(guiNormalized));
    const QString cliText = QString::fromUtf8(normalizeNewlines(cliNormalized));

    qInfo("preflight parity: fixture=%s profile=%s cli_exit_code=%d", qPrintable(fixtureId), qPrintable(profilePath), cliExitCode);
    if (guiText != cliText)
    {
        qWarning().noquote() << "GUI preflight report diverges from PdfTool preflight:\n"
                             << describePreflightReportDifference(guiText, cliText);
    }

    QCOMPARE(guiText, cliText);
}

void EditorHostTest::restrictedPreflightReportMatchesPdfTool()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    pdf::PDFSettings::setSettingsPath(temp.path());
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopEditor);
    const QString documentPath = QDir(preflightFixturesDir()).filePath(QStringLiteral("image-dpi-low.pdf"));
    QVERIFY(QFile::exists(documentPath));
    const QJsonObject profile = pdf::exportPreflightProfile(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("loop-test-restricted-parity") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Restricted parity") },
        { QStringLiteral("restrictions"), QJsonObject{
                                              { QStringLiteral("pages"), QStringLiteral("1") },
                                              { QStringLiteral("object_classes"), QJsonArray{ QStringLiteral("image") } } } },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("image-resolution") }, { QStringLiteral("min_dpi"), 300 } } } } });
    const QString profilePath = temp.filePath(QStringLiteral("restricted-parity.json"));
    QFile source(profilePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    const QByteArray bytes = QJsonDocument(profile).toJson();
    QCOMPARE(source.write(bytes), bytes.size());
    source.close();

    QJsonObject cliReport;
    int cliExit = -1;
    runPdfToolPreflight(documentPath, profilePath, cliReport, cliExit);
    QVERIFY(!cliReport.isEmpty());
    const QJsonObject cliScope = cliReport.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("scope_restrictions")).toObject();
    QCOMPARE(cliScope.value(QStringLiteral("pages")).toArray(), QJsonArray{ 1 });
    QCOMPARE(cliScope.value(QStringLiteral("object_classes")).toArray(), QJsonArray{ QStringLiteral("image") });

    EditorHost host;
    QVERIFY(host.importPreflightProfileFileUrl(QUrl::fromLocalFile(profilePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY(host.hasPreflightReport());

    const QString exportedPath = temp.filePath(QStringLiteral("restricted-gui-report.json"));
    QVERIFY(host.exportPreflightReportFileUrl(QUrl::fromLocalFile(exportedPath)));
    QFile exported(exportedPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QJsonObject guiReport;
    parsePreflightReport(exported.readAll(), QStringLiteral("restricted GUI report"), guiReport);
    QVERIFY(!guiReport.isEmpty());
    QCOMPARE(guiReport.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("scope_restrictions")).toObject(), cliScope);
    QCOMPARE(QJsonDocument(normalizePreflightReport(guiReport)).toJson(QJsonDocument::Compact),
             QJsonDocument(normalizePreflightReport(cliReport)).toJson(QJsonDocument::Compact));

    const QString historyPath =
        QDir(QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    const auto editorAuditIt = std::find_if(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                                            { return event.kind == pdf::PDFOperationHistoryEventKind::PreflightRun &&
                                                     event.status == pdf::PDFOperationHistoryStatus::Accepted &&
                                                     event.operatorIdentity == QStringLiteral("LoopEditor"); });
    QVERIFY(editorAuditIt != events.cend());
    QCOMPARE(editorAuditIt->effectiveProfileDigest, cliReport.value(QStringLiteral("effective_profile_digest")).toString());
    QCOMPARE(editorAuditIt->resultSummary.value(QStringLiteral("coverage_scope")).toObject().value(QStringLiteral("scope_restrictions")).toObject(),
             cliScope);
    QVERIFY(history.verify().verified);
}

void EditorHostTest::importValidProfileAddsDigest()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QStandardPaths::setTestModeEnabled(true);
    pdf::PDFSettings::setSettingsPath(temp.path());
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopEditor);

    const QString sourcePath = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/profiles/loop-default.json");
    QFile bundled(sourcePath);
    QVERIFY(bundled.open(QIODevice::ReadOnly));
    const QJsonObject profile = QJsonDocument::fromJson(bundled.readAll()).object();
    const QString expectedDigest = profile.value(QStringLiteral("digest")).toString();
    QVERIFY(!expectedDigest.isEmpty());

    EditorHost host;
    QVERIFY(host.importPreflightProfileFileUrl(QUrl::fromLocalFile(sourcePath)));

    const QString expectedId = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                   .filePath(QStringLiteral("profiles/loop-default.json"));
    bool found = false;
    for (const QVariant& entry : host.preflightProfiles())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("id")).toString() == expectedId)
        {
            found = true;
            QCOMPARE(item.value(QStringLiteral("digest")).toString(), expectedDigest);
            QVERIFY(item.value(QStringLiteral("valid")).toBool());
            break;
        }
    }
    QVERIFY(found);
}

void EditorHostTest::importDigestMismatchProfileIsRejected()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QJsonObject profile = pdf::exportPreflightProfile(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bad-profile") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Bad") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 300 } } } } });
    profile.insert(QStringLiteral("digest"), QString(64, QLatin1Char('a')));
    const QString sourcePath = temp.filePath(QStringLiteral("bad-profile.json"));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.write(QJsonDocument(profile).toJson(QJsonDocument::Compact));

    EditorHost host;
    QVERIFY(!host.importPreflightProfileFileUrl(QUrl::fromLocalFile(sourcePath)));
}

void EditorHostTest::saveProfileForkRecordsDerivedFrom()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    EditorHost host;
    QString parentDigest;
    for (const QVariant& entry : host.preflightProfiles())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("id")) == host.selectedPreflightProfileId())
        {
            parentDigest = item.value(QStringLiteral("digest")).toString();
            break;
        }
    }
    QVERIFY(!parentDigest.isEmpty());

    QVERIFY(host.beginPreflightProfileEdit());
    QVERIFY(host.setPreflightCheckField(QStringLiteral("image-resolution"), QStringLiteral("min_dpi"), 240));
    const QString destination = temp.filePath(QStringLiteral("forked-profile.json"));
    QVERIFY(host.savePreflightProfileEdit(host.preflightProfileDraftVersion(), QUrl::fromLocalFile(destination)));

    QFile forkedFile(destination);
    QVERIFY(forkedFile.open(QIODevice::ReadOnly));
    const QJsonObject forked = QJsonDocument::fromJson(forkedFile.readAll()).object();
    const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(forked, destination);
    QVERIFY2(imported.ok, qPrintable(imported.errorMessage));
    QCOMPARE(forked.value(QStringLiteral("derived_from")).toObject().value(QStringLiteral("digest")).toString(), parentDigest);
    QVERIFY(forked.value(QStringLiteral("digest")).toString() != parentDigest);

    const QByteArray roundTrip = pdf::serializePreflightProfileBytes(imported.profile);
    const pdf::PreflightProfileImportResult reimported = pdf::importPreflightProfile(QJsonDocument::fromJson(roundTrip).object());
    QVERIFY(reimported.ok);
    QCOMPARE(roundTrip, pdf::serializePreflightProfileBytes(reimported.profile));
}

void EditorHostTest::openLargeDocument()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    constexpr int pageCount = 1000;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 612, 792));
    }

    const QString path = directory.filePath(QStringLiteral("large.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    QElapsedTimer openTimer;
    openTimer.start();

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 90000);
    const qint64 openToFirstViewMs = openTimer.elapsed();

    QCOMPARE(host.pageCount(), pageCount);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionGoToDocumentEnd")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionGoToDocumentEnd")) != 0);
    QTRY_COMPARE_WITH_TIMEOUT(host.currentPage(), pageCount - 1, 15000);

    // --- Interaction revision and surface fencing under load (gh-363) ---
    // EditorHost owns DocumentViewSession privately; sessionForTest() is the
    // minimal test-only accessor that lets the shell stress test observe
    // revision/viewport/surfaces/interaction without breaking QML encapsulation.
    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    QVERIFY(session->revisionSource() != nullptr);
    QVERIFY(session->surfaces() != nullptr);
    QVERIFY(session->interaction() != nullptr);

    // Ensure the viewport is in a deterministic state for hit-testing. The
    // host was already given 1024x768 above; re-apply to guarantee
    // requestGeneration is stable before we snapshot it.
    // Note: setViewportGeometry is idempotent when the values are unchanged,
    // so this does not bump generation if already set.
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);

    // The shell's Quick host without a LoopCanvasItem leaves
    // DocumentViewSession::hitTest() empty (EditorHost::bindCanvas() is the
    // only place that adds PageBoxHitTestSource). For a fencing proof we need
    // a source that can produce overlay-only hover transitions. Inject a
    // scripted finding that covers a small rect around the viewport center so
    // pointer sweeps can enter/leave it without advancing generation.
    pdfinteraction::ViewportController& viewport = session->viewport();
    QVERIFY(viewport.pageCount() == pageCount);
    QRect placed = viewport.placedPageRect(viewport.currentPage());
    // Fallback to page 0 if current page placement is empty (should not happen after navigation).
    if (placed.isEmpty())
    {
        placed = viewport.placedPageRect(0);
    }
    QVERIFY(!placed.isEmpty());

    const QPoint viewportCenter = placed.center();
    const std::optional<QPointF> optCenterPage = viewport.viewportToPagePoint(viewportCenter, viewport.currentPage());
    // Center of a 612x792 page is approx 306x396. Use that as fallback if the
    // inverse mapping fails (e.g., due to rounding).
    const QPointF centerPagePoint = optCenterPage.value_or(QPointF(306.0, 396.0));

    // Finding bounds: 30x30 square centered at the page center point, guaranteed
    // to be inside the page's media box and inside the viewport after mapping.
    const QRectF findingBounds(centerPagePoint.x() - 15.0, centerPagePoint.y() - 15.0, 30.0, 30.0);
    ScriptedHitTestSource scripted;
    scripted.targets.push_back(makeFindingTarget(QStringLiteral("stress-finding-1"), findingBounds, viewport.currentPage()));
    // Also expose the page-box source for completeness; edge hits are another
    // overlay-only path but not required for the assertion.
    session->hitTest()->addSource(&scripted);
    session->hitTest()->addSource(&session->pageBoxSource());

    const pdf::PDFRevisionIdentity revisionBefore = session->revisionSource()->currentRevision();
    const quint64 generationBefore = viewport.requestGeneration();
    const int requestedBefore = session->surfaces()->counters().requested;
    const qint64 admittedHighWaterBefore = session->surfaces()->counters().admittedBytesHighWater;

    QSignalSpy overlaySpy(session->interaction(), &pdfinteraction::InteractionController::overlayFrameChanged);
    QSignalSpy viewportSpy(session->interaction(), &pdfinteraction::InteractionController::viewportChanged);
    QSignalSpy hoverSpy(session->interaction(), &pdfinteraction::InteractionController::hoverChanged);
    QVERIFY(overlaySpy.isValid());
    QVERIFY(viewportSpy.isValid());

    // Helper: page point -> viewport pixel via the same matrix the surfaces use.
    // This keeps the test's arithmetic from diverging from the controller's.
    auto viewportPointFor = [&viewport](QPointF pagePoint, int pageIndex) -> QPoint
    {
        const QTransform matrix = viewport.pagePointToViewportMatrix(pageIndex);
        return matrix.map(pagePoint).toPoint();
    };

    const int currentPageIndex = viewport.currentPage();
    // Perform 80 pointer Move sweeps that alternately hit and miss the scripted
    // finding. Hover changes are overlay-only: they must produce overlay frames
    // but never advance requestGeneration or surface demand.
    constexpr int sweepSteps = 80;
    for (int step = 0; step < sweepSteps; ++step)
    {
        // Sweep horizontally across the finding: offset -40..+39 scaled by 2,
        // so the pointer travels ~160 page units centered on the finding.
        // Steps ~33..47 hit the 30-unit finding, others miss, yielding at least
        // 2 hover transitions (enter + leave) and thus >=2 overlay frames.
        const qreal offset = qreal(step) - 40.0;
        const QPointF pagePoint(centerPagePoint.x() + offset * 2.0, centerPagePoint.y());
        const QPoint viewportPoint = viewportPointFor(pagePoint, currentPageIndex);
        session->interaction()->handlePointer(makeMoveIntent(viewportPoint, quint64(step + 1)));
    }

    // Fencing holds: no document revision change, no viewport generation bump,
    // no new surface requests, no renderer invocation via surfaces. The sweep
    // did produce overlay work.
    QCOMPARE(session->revisionSource()->currentRevision(), revisionBefore);
    QCOMPARE(viewport.requestGeneration(), generationBefore);
    QCOMPARE(session->surfaces()->counters().requested, requestedBefore);
    // Admitted high water should not have grown due to overlay-only input.
    QCOMPARE(session->surfaces()->counters().admittedBytesHighWater, admittedHighWaterBefore);
    QCOMPARE(viewportSpy.size(), 0);
    // At least enter + leave of the finding, plus potentially Page fallback
    // transitions. The exact count depends on hit-test tie-break but is >=2.
    QVERIFY(overlaySpy.size() >= 2);
    // hoverChanged is emitted only when the hovered target actually changes;
    // sweeping across the finding should have produced at least 2 changes.
    QVERIFY(hoverSpy.size() >= 2);

    // Also verify that surfaces remain fenced at the coordinator's generation:
    // overlay-only hover must not have advanced the PageSurfaceCoordinator's
    // own generation (which tracks demand supersession). The coordinator's
    // generation is independent of the viewport's but is similarly only
    // advanced by requestSurfaces/invalidate, not by interaction.
    const quint64 coordinatorGenerationBefore = session->surfaces()->generation();
    // A second smaller sweep to ensure coordinator generation still stable.
    for (int step = 0; step < 20; ++step)
    {
        const QPointF pagePoint(centerPagePoint.x() + qreal(step), centerPagePoint.y() + 10.0);
        const QPoint viewportPoint = viewportPointFor(pagePoint, currentPageIndex);
        session->interaction()->handlePointer(makeMoveIntent(viewportPoint, quint64(sweepSteps + step + 1)));
    }
    QCOMPARE(session->surfaces()->generation(), coordinatorGenerationBefore);
    QCOMPARE(viewport.requestGeneration(), generationBefore);
    QCOMPARE(session->revisionSource()->currentRevision(), revisionBefore);

    // Optional envelope: when LOOP_STRESS_ENVELOPE is set, record
    // PDFWorkloadEnvelope identity and timings for observability. This is
    // opt-in so CI without the env var does not pay the cost of JSON
    // assertions, but when enabled it proves the 1k-page shell open is
    // measurable and the envelope contract holds.
    const QByteArray envelopeEnv = qgetenv("LOOP_STRESS_ENVELOPE");
    if (!envelopeEnv.isEmpty())
    {
        const bool envelopeEnabled = envelopeEnv == QByteArrayLiteral("1") || envelopeEnv.toLower() == QByteArrayLiteral("true");
        if (envelopeEnabled)
        {
            pdf::PDFWorkloadEnvelope envelope;
            envelope.identity = pdf::PDFRunIdentity::capture();
            envelope.family = QStringLiteral("shell-stress-1k");
            envelope.status = QStringLiteral("complete");
            envelope.pageCount = pageCount;
            envelope.openToFirstViewMs = openToFirstViewMs;
            envelope.rssHighWaterBytes = pdf::PDFWorkloadEnvelope::currentRssHighWaterBytes();
            envelope.cacheHighWaterBytes = session->surfaces()->counters().admittedBytesHighWater;
            envelope.pressureShedCount = session->surfaces()->counters().shed;
            envelope.elapsedMs = openTimer.elapsed();
            envelope.prefetchShed = session->surfaces()->counters().shed > 0;
            // Interaction slot was held throughout the sweep without blocking.
            envelope.interactionSlotHeld = true;

            const QJsonObject json = envelope.toJson();
            // Contract: family, page_count, open_to_first_view_ms, rss, cache,
            // pressure_shed_count, identity must be present.
            QCOMPARE(json.value(QStringLiteral("family")).toString(), QStringLiteral("shell-stress-1k"));
            QCOMPARE(json.value(QStringLiteral("page_count")).toInt(), pageCount);
            QVERIFY(json.contains(QStringLiteral("open_to_first_view_ms")));
            QVERIFY(!json.value(QStringLiteral("open_to_first_view_ms")).isNull());
            QVERIFY(json.value(QStringLiteral("open_to_first_view_ms")).toInt() >= 0);
            QVERIFY(json.contains(QStringLiteral("rss_high_water_bytes")));
            QVERIFY(json.contains(QStringLiteral("cache_high_water_bytes")));
            QVERIFY(json.contains(QStringLiteral("pressure_shed_count")));
            QVERIFY(json.contains(QStringLiteral("identity")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("commit")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("qt")));
            QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("os")));
            QVERIFY(json.value(QStringLiteral("status")).toString() == QStringLiteral("complete"));

            qDebug() << "PDFWorkloadEnvelope shell-stress-1k"
                     << QJsonDocument(json).toJson(QJsonDocument::Compact);
            qDebug() << "openToFirstViewMs" << openToFirstViewMs << "rssHighWater"
                     << envelope.rssHighWaterBytes << "cacheHighWater" << envelope.cacheHighWaterBytes
                     << "shed" << envelope.pressureShedCount;
        }
    }

    // Cleanup: remove scripted sources so teardown does not leave dangling
    // pointers (scripted is stack-allocated). DocumentViewSession will be
    // destroyed with EditorHost.
    session->hitTest()->clearSources();
}

// ---------------------------------------------------------------------------
// Workspace surfaces (#586)
// ---------------------------------------------------------------------------

void EditorHostTest::fixWorkspacePresentsIdleLifecycleAndRefusesToArm()
{
    EditorHost host;

    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QVERIFY(!host.fixLifecycleSummary().trimmed().isEmpty());
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.fixRollbackAvailable());
    QVERIFY(!host.fixRollbackSummary().trimmed().isEmpty());
    QVERIFY(host.fixRollbackPoints().isEmpty());
    QVERIFY(!host.fixPreview().value(QStringLiteral("available")).toBool());
    QVERIFY(!host.fixPreview().value(QStringLiteral("incomplete")).toBool());

    // Nothing may be approved, rejected, executed, rolled back or inspected before a plan
    // exists for this revision.
    QVERIFY(!host.approveActionListPlan());
    QVERIFY(!host.rejectActionListPlan());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.inspectActionListStep(0));
    QVERIFY(!host.requestFixRollback(QStringLiteral("not-a-recorded-revision")));

    // The lifecycle is always renderable, carries words as well as a shape, and never claims
    // a pass.
    const QVariantMap visual = host.fixLifecycleVisual();
    QCOMPARE(visual.value(QStringLiteral("kind")).toString(), QStringLiteral("NotChecked"));
    QCOMPARE(visual.value(QStringLiteral("icon")).toString(), QStringLiteral("Outline"));
    QVERIFY(!visual.value(QStringLiteral("accessibleName")).toString().trimmed().isEmpty());

    // The identity every Fix affordance binds to exists even before a plan does, so a pane
    // never has to invent one.
    const QVariantMap identity = host.fixPlanIdentity();
    QCOMPARE(identity.value(QStringLiteral("lifecycleStateName")).toString(), QStringLiteral("idle"));
    QCOMPARE(identity.value(QStringLiteral("reviewDecision")).toString(), QStringLiteral("none"));
    QCOMPARE(identity.value(QStringLiteral("planIsCurrent")).toBool(), false);
    QCOMPARE(identity.value(QStringLiteral("executionArmed")).toBool(), false);
}

void EditorHostTest::fixReviewBindsToThePlannedDigestAndTheCurrentRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("gh-586-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("gh-586-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 3 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    // The catalog identifies a recipe by the file that defines it, and importing selects it.
    QVERIFY(!host.selectedActionListRecipeId().isEmpty());

    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // The Inspect workspace's corrective intent lands on a recipe that really runs the
    // operation, and plans nothing by itself.
    const QString recipeId = host.selectedActionListRecipeId();
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QCOMPARE(host.selectedActionListRecipeId(), recipeId);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QVERIFY(!host.selectActionListRecipeForOperation(QStringLiteral("no-such-operation")));
    QVERIFY(!host.selectActionListRecipeForOperation(QString()));

    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);

    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    // The plan is bound to the revision it was produced for, and the operator's approval is
    // bound to that exact plan digest.
    const QVariantMap planned = host.fixPlanIdentity();
    const QString planDigest = planned.value(QStringLiteral("planDigest")).toString();
    QVERIFY(!planDigest.isEmpty());
    QCOMPARE(planned.value(QStringLiteral("planIsCurrent")).toBool(), true);
    QCOMPARE(planned.value(QStringLiteral("reviewDecision")).toString(), QStringLiteral("none"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());

    QVERIFY(host.approveActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QVERIFY(host.fixExecutionArmed());
    const QVariantMap approved = host.fixPlanIdentity();
    QCOMPARE(approved.value(QStringLiteral("reviewDecision")).toString(), QStringLiteral("approved"));
    QCOMPARE(approved.value(QStringLiteral("reviewedPlanDigest")).toString(), planDigest);
    QCOMPARE(approved.value(QStringLiteral("executionArmed")).toBool(), true);

    // Rejecting is the operator's own decision and leaves the plan visible for inspection.
    QVERIFY(host.rejectActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("rejected"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());

    host.replanActionList();
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QVERIFY(host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString().isEmpty());

    // A revision change may not inherit the review: the plan goes stale, the approval stops
    // arming execution, and nothing may be approved against the superseded digest.
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.fixExecutionArmed());

    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("stale"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.approveActionListPlan());
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("planIsCurrent")).toBool(), false);
    QVERIFY(!host.fixLifecycleSummary().trimmed().isEmpty());
}

void EditorHostTest::moveSelectionProposesAPageBoxMoveInTheFixWorkspace()
{
    const QString commandId = QStringLiteral("actionMoveSelection");
    const auto moveParameters = [](const QString& kind, double dx, double dy)
    {
        return QVariantMap{ { QStringLiteral("targetKind"), kind },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), dx },
                            { QStringLiteral("dy"), dy } };
    };

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("move.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("issue-104-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("issue-104-test") },
                                       { QStringLiteral("name"), QStringLiteral("Move a page box") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("move") },
                                             { QStringLiteral("operation"), QStringLiteral("translate-page-box") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("box"), QStringLiteral("trim") },
                                                            { QStringLiteral("page_index"), 0 },
                                                            { QStringLiteral("dx"), 1 },
                                                            { QStringLiteral("dy"), 1 } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host;
    QVERIFY(!host.isCommandEnabled(commandId));
    QCOMPARE(host.invokeCommand(commandId, moveParameters(QStringLiteral("PageBox"), 4.0, 5.0)), quint64(0));

    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.isCommandEnabled(commandId));

    // Only a page box is a document edit, and a zero move is not one.
    host.setWorkspace(EditorHost::Document);
    QVERIFY(host.invokeCommand(commandId, moveParameters(QStringLiteral("Finding"), 4.0, 5.0)) != 0);
    QVERIFY(host.invokeCommand(commandId, moveParameters(QStringLiteral("PageBox"), 0.0, 0.0)) != 0);
    QCOMPARE(host.workspace(), EditorHost::Document);

    QVERIFY(host.invokeCommand(commandId, moveParameters(QStringLiteral("PageBox"), 4.0, 5.0)) != 0);
    QCOMPARE(host.workspace(), EditorHost::Fix);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
    QCOMPARE(host.actionListBindings().size(), 4);
    for (const QVariant& binding : host.actionListBindings())
    {
        const QVariantMap entry = binding.toMap();
        if (entry.value(QStringLiteral("name")).toString() == QStringLiteral("dx"))
        {
            QCOMPARE(entry.value(QStringLiteral("value")).toDouble(), 4.0);
        }
    }
}

QString shippedTranslatePageBoxRecipeId()
{
    return QStringLiteral(":/loop/builtin-recipe-translate-page-box.json");
}

// The operator recipe directory is shared by every test in the process, so each recipe test
// starts from an empty one; the shipped recipe is compiled in and is unaffected.
void clearOperatorActionListRecipes()
{
    QStandardPaths::setTestModeEnabled(true);
    QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath(QStringLiteral("recipes")))
        .removeRecursively();
}

QString recipesDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath(QStringLiteral("recipes"));
}

bool writeTranslatePageBoxRecipe(const QString& path, const QString& id, const QJsonValue& pageIndex)
{
    QFile recipe(path);
    if (!recipe.open(QIODevice::WriteOnly))
    {
        return false;
    }
    const QJsonDocument document(QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), id },
        { QStringLiteral("steps"),
          QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("move") },
                                   { QStringLiteral("operation"), QStringLiteral("translate-page-box") },
                                   { QStringLiteral("params"),
                                     QJsonObject{ { QStringLiteral("box"), QStringLiteral("trim") },
                                                  { QStringLiteral("page_index"), pageIndex },
                                                  { QStringLiteral("dx"), 2 },
                                                  { QStringLiteral("dy"), 3 } } } } } } });
    return recipe.write(document.toJson(QJsonDocument::Compact)) >= 0;
}

// A page whose trim box sits inside its media box. The default profile requires 9 pt of bleed
// around the trim, and with no BleedBox that bleed is the media box, so the 20 pt inset keeps
// 9 pt on every edge after the 2/3 point move.
bool writeTrimmedPageDocument(const QString& path)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    builder.setPageTrimBox(page, QRectF(20, 20, 160, 160));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    return static_cast<bool>(writer.write(path, &document, true));
}

void EditorHostTest::shippedRecipeIsListedButNeverSelectedByDefault()
{
    clearOperatorActionListRecipes();

    EditorHost host;
    bool listed = false;
    for (const QVariant& entry : host.actionListRecipes())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("id")).toString() == shippedTranslatePageBoxRecipeId())
        {
            listed = true;
            QVERIFY(item.value(QStringLiteral("valid")).toBool());
            QVERIFY(item.value(QStringLiteral("builtIn")).toBool());
        }
    }
    QVERIFY(listed);
    QVERIFY(host.selectedActionListRecipeId().isEmpty());
}

void EditorHostTest::shippedPageBoxRecipeRunsDefaultMoveThroughPlanApprovalAndRevalidation()
{
    clearOperatorActionListRecipes();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString documentPath = directory.filePath(QStringLiteral("default-move.pdf"));
    QVERIFY(writeTrimmedPageDocument(documentPath));
    const auto fileDigest = [](const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray sourceDigest = fileDigest(documentPath);
    QVERIFY(!sourceDigest.isEmpty());

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // A drag proposes the move, and the shipped recipe binds to it with no import. Nothing is
    // selected before the drag, so the shipped recipe is never a silent default.
    QVERIFY(host.selectedActionListRecipeId().isEmpty());
    const QVariantMap move{ { QStringLiteral("targetKind"), QStringLiteral("PageBox") },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), 2.0 },
                            { QStringLiteral("dy"), 3.0 } };
    QVERIFY(host.invokeCommand(QStringLiteral("actionMoveSelection"), move) != 0);
    QCOMPARE(host.workspace(), EditorHost::Fix);
    QCOMPARE(host.selectedActionListRecipeId(), shippedTranslatePageBoxRecipeId());
    QCOMPARE(host.actionListBindings().size(), 4);

    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    const QString approvedDigest = host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString();
    QVERIFY(!approvedDigest.isEmpty());

    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.fixExecutionArmed());
    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QStringLiteral("executing"), 120000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("succeeded"));

    // The published artifact is bound to the approved plan and is not the as-received input.
    const QVariantMap signOff = host.fixSignOff();
    QCOMPARE(signOff.value(QStringLiteral("planDigest")).toString(), approvedDigest);
    const QString publishedSha256 = signOff.value(QStringLiteral("publishedSha256")).toString();
    QCOMPARE(publishedSha256.size(), 64);
    QVERIFY(publishedSha256 != QString::fromLatin1(sourceDigest));
    QCOMPARE(fileDigest(documentPath), sourceDigest);
}

void EditorHostTest::operatorRecipeWinsOverShippedRecipe()
{
    clearOperatorActionListRecipes();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString documentPath = directory.filePath(QStringLiteral("operator-move.pdf"));
    QVERIFY(writeTrimmedPageDocument(documentPath));
    const QString recipePath = directory.filePath(QStringLiteral("operator-move.json"));
    QVERIFY(writeTranslatePageBoxRecipe(recipePath, QStringLiteral("operator-move"), 0));

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // A valid operator recipe that offers the operation is selected ahead of the shipped one, and
    // it is named on the surface so the operator can see which recipe the plan belongs to.
    const QVariantMap move{ { QStringLiteral("targetKind"), QStringLiteral("PageBox") },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), 2.0 },
                            { QStringLiteral("dy"), 3.0 } };
    QVERIFY(host.invokeCommand(QStringLiteral("actionMoveSelection"), move) != 0);
    QVERIFY(!host.selectedActionListRecipeId().isEmpty());
    QVERIFY(host.selectedActionListRecipeId() != shippedTranslatePageBoxRecipeId());
    QVERIFY(host.selectedActionListRecipeId().endsWith(QStringLiteral("operator-move.json")));
}

void EditorHostTest::invalidOperatorRecipeStopsMoveWithoutShippedFallback()
{
    clearOperatorActionListRecipes();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString documentPath = directory.filePath(QStringLiteral("broken-move.pdf"));
    QVERIFY(writeTrimmedPageDocument(documentPath));
    // The operator recipe offers the operation with a page index the schema refuses, so it is
    // invalid. It sits in the recipes directory before the host starts, as an imported copy would.
    QVERIFY(QDir().mkpath(recipesDirectory()));
    const QString brokenPath = QDir(recipesDirectory()).filePath(QStringLiteral("broken-move.json"));
    QVERIFY(writeTranslatePageBoxRecipe(brokenPath, QStringLiteral("broken-move"), QStringLiteral("first")));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    // The move is refused and nothing is selected: the shipped recipe must not run in place of
    // the operator's invalid one.
    const QVariantMap move{ { QStringLiteral("targetKind"), QStringLiteral("PageBox") },
                            { QStringLiteral("targetId"), QStringLiteral("trim") },
                            { QStringLiteral("page"), 0 },
                            { QStringLiteral("dx"), 2.0 },
                            { QStringLiteral("dy"), 3.0 } };
    host.invokeCommand(QStringLiteral("actionMoveSelection"), move);
    QVERIFY(host.selectedActionListRecipeId().isEmpty());
    QVERIFY(!host.selectActionListRecipeForOperation(QStringLiteral("translate-page-box")));
    QVERIFY(host.selectedActionListRecipeId().isEmpty());

    bool brokenIsInvalid = false;
    for (const QVariant& entry : host.actionListRecipes())
    {
        const QVariantMap item = entry.toMap();
        if (item.value(QStringLiteral("name")).toString() == QStringLiteral("broken-move"))
        {
            brokenIsInvalid = !item.value(QStringLiteral("valid")).toBool();
        }
    }
    QVERIFY(brokenIsInvalid);
}

void EditorHostTest::completedDragOfARefusedKindIsReportedAndChangesNothing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("refused-drag.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    DocumentViewSession* session = host.sessionForTest();
    QVERIFY(session != nullptr);
    QVERIFY(session->interaction() != nullptr);

    host.setWorkspace(EditorHost::Document);
    const auto revisionBefore = session->facade().currentRevision();
    QSignalSpy refusedSpy(&host, &EditorHost::dragRefused);

    const struct
    {
        pdfinteraction::InteractionTargetKind kind;
        const char* name;
    } refusedKinds[] = { { pdfinteraction::InteractionTargetKind::Finding, "finding" },
                         { pdfinteraction::InteractionTargetKind::Guide, "guide" },
                         { pdfinteraction::InteractionTargetKind::DragHandle, "drag-handle" } };
    for (const auto& refused : refusedKinds)
    {
        pdfinteraction::DragSession dragged;
        dragged.target.kind = refused.kind;
        dragged.target.pageIndex = 0;
        dragged.target.id = QStringLiteral("subject");
        dragged.pageDelta = QPointF(4.0, 5.0);
        dragged.exceededThreshold = true;

        const int before = refusedSpy.count();
        Q_EMIT session->interaction()->dragCompleted(dragged);

        QCOMPARE(refusedSpy.count(), before + 1);
        QCOMPARE(refusedSpy.last().at(0).toString(), QString::fromLatin1(refused.name));
        QCOMPARE(refusedSpy.last().at(1).toString(), QStringLiteral("subject"));
        QCOMPARE(host.workspace(), EditorHost::Document);
        QCOMPARE(session->facade().currentRevision(), revisionBefore);
    }

    // A page box is admitted, so it is never reported as refused.
    pdfinteraction::DragSession boxDrag;
    boxDrag.target.kind = pdfinteraction::InteractionTargetKind::PageBox;
    boxDrag.target.pageIndex = 0;
    boxDrag.target.id = QStringLiteral("trim");
    boxDrag.pageDelta = QPointF(4.0, 5.0);
    boxDrag.exceededThreshold = true;
    const int refusalsBeforeBox = refusedSpy.count();
    Q_EMIT session->interaction()->dragCompleted(boxDrag);
    QCOMPARE(refusedSpy.count(), refusalsBeforeBox);
}

void EditorHostTest::fixJourneyPublishesOnlyAnApprovedPlanBoundToTheDisplayedIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("plan-approval-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("plan-approval-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 3 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    const auto fileDigest = [](const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray sourceDigest = fileDigest(documentPath);
    QVERIFY(!sourceDigest.isEmpty());

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    // What the operator is shown is what an approval binds: one plan digest on both the
    // identity surface and the preview, against one document revision.
    const QVariantMap displayed = host.fixPlanIdentity();
    const QString displayedDigest = displayed.value(QStringLiteral("planDigest")).toString();
    QVERIFY(!displayedDigest.isEmpty());
    QCOMPARE(displayed.value(QStringLiteral("planIsCurrent")).toBool(), true);
    QVERIFY(!displayed.value(QStringLiteral("plannedRevision")).toString().isEmpty());
    const QVariantMap preview = host.fixPreview();
    QCOMPARE(preview.value(QStringLiteral("planDigest")).toString(), displayedDigest);
    QCOMPARE(preview.value(QStringLiteral("planIsCurrent")).toBool(), true);

    // Unapproved: a plan nobody has approved cannot start a run, and nothing is published.
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("preview-ready"));
    QVERIFY(!host.fixRecheck().value(QStringLiteral("available")).toBool());
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    // Denied: the operator's own refusal is honoured, and it also starts nothing.
    QVERIFY(host.rejectActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("rejected"));
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.fixRecheck().value(QStringLiteral("available")).toBool());
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    // Stale: reopening the document moves the revision, so the plan belongs to an earlier one.
    // The surface names the revision its evidence belongs to instead of looking clear, the plan
    // reports itself as no longer current, and no run may start against it.
    host.reopenDocument();
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("stale"));
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("planIsCurrent")).toBool(), false);
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.approveActionListPlan());
    QVERIFY(!host.executeApprovedActionListPlan());
    QVERIFY(!host.fixRecheck().value(QStringLiteral("available")).toBool());
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    // The operator now plans against the revision that is open and approves it: the approval is
    // armed and bound to the digest this plan actually has.
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    const QString approvedDigest = host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString();
    QVERIFY(!approvedDigest.isEmpty());
    QVERIFY(host.approveActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QVERIFY(host.fixExecutionArmed());
    QCOMPARE(host.fixPlanIdentity().value(QStringLiteral("reviewedPlanDigest")).toString(), approvedDigest);

    // An approval belongs to the plan it was given for: planning again against the same revision
    // clears the review, so the run is refused until the operator approves the plan that is
    // displayed now, and nothing is published in between. Mutation-probed: deleting the
    // clearFixReview() call that runs when a plan is accepted makes this step fail, so the refusal
    // is enforced by that line rather than by an accident of state. Two further clauses are
    // defensive against a state this journey offers no path to, and removing either leaves this
    // fixture green: the reviewed-digest equality inside fixLifecycleStateName() and the
    // fixPlanIsCurrent() clause in approveActionListPlan(), both already decided by the state
    // checks that run before them.
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QCOMPARE(fileDigest(documentPath), sourceDigest);
    QVERIFY(host.approveActionListPlan());
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("approved"));
    QVERIFY(host.fixExecutionArmed());

    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QStringLiteral("executing"), 120000);
    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("succeeded"));

    // Completion presents the revalidated result and the artifact Core signed, distinct
    // from the as-received input and bound to the plan the operator approved. The run
    // presents the published bytes; materializing them as a file is the export slice, so
    // this asserts the presentation rather than a filesystem write.
    const QVariantMap recheck = host.fixRecheck();
    QVERIFY(recheck.value(QStringLiteral("available")).toBool());
    const QString verdictState = recheck.value(QStringLiteral("verdictState")).toString();
    QVERIFY2(verdictState == QStringLiteral("pass") || verdictState == QStringLiteral("incomplete") ||
                 verdictState == QStringLiteral("fail"),
             qPrintable(verdictState));
    QVERIFY2(verdictState != QStringLiteral("fail"), qPrintable(verdictState));
    const QVariantMap signOff = host.fixSignOff();
    QCOMPARE(signOff.value(QStringLiteral("planDigest")).toString(), approvedDigest);
    const QString publishedSha256 = signOff.value(QStringLiteral("publishedSha256")).toString();
    QCOMPARE(publishedSha256.size(), 64);
    for (const QChar character : publishedSha256)
    {
        QVERIFY2(character.isDigit() || (character >= QLatin1Char('a') && character <= QLatin1Char('f')),
                 qPrintable(publishedSha256));
    }
    QVERIFY(publishedSha256 != QString::fromLatin1(sourceDigest));
    QVERIFY2(host.fixLifecycleSummary().contains(QStringLiteral("published")),
             qPrintable(host.fixLifecycleSummary()));

    // The as-received input is never the publication target.
    QCOMPARE(fileDigest(documentPath), sourceDigest);
}

void EditorHostTest::translatePageBoxRefusesAMoveThatIntroducesABlockingFinding()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    // A 10 pt trim inset leaves 8 pt on the right and top edges after the +2/+3 pt move. The default
    // profile requires 9 pt of bleed, and with no BleedBox that bleed is the media box.
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    builder.setPageTrimBox(page, QRectF(10, 10, 180, 180));
    const QString documentPath = directory.filePath(QStringLiteral("tight-margin.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("tight-margin-move.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("tight-margin-move") },
                                       { QStringLiteral("name"), QStringLiteral("Tight margin move") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("move") },
                                             { QStringLiteral("operation"), QStringLiteral("translate-page-box") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("box"), QStringLiteral("trim") },
                                                            { QStringLiteral("page_index"), 0 },
                                                            { QStringLiteral("dx"), 2 },
                                                            { QStringLiteral("dy"), 3 } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    QVERIFY(!host.selectedActionListRecipeId().isEmpty());
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QStringLiteral("executing"), 120000);

    QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("failed"));
    QVERIFY(host.actionList()->property("operatorSummary").toString().contains(QStringLiteral("blocking findings")));
}

void EditorHostTest::fixRollbackReturnsToARecordedRevision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString sourcePath = directory.filePath(QStringLiteral("job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(sourcePath, &document, true));
    }

    // The recorded history a rollback returns to is written by the governed repair path; the
    // shell only reads it and appends the rolled-back event through Core.
    const QString repairedPath = directory.filePath(QStringLiteral("job-bleed.pdf"));
    {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        process.setProcessEnvironment(environment);
        process.start(QStringLiteral(PDFTOOL_EXECUTABLE_PATH),
                      { QStringLiteral("repair"),
                        sourcePath,
                        QStringLiteral("--operation"),
                        QStringLiteral("add-bleed"),
                        QStringLiteral("--param"),
                        QStringLiteral("bleed_mm=3"),
                        QStringLiteral("--param"),
                        QStringLiteral("mode=mirror"),
                        QStringLiteral("--param"),
                        QStringLiteral("force=true"),
                        QStringLiteral("--profile"),
                        preflightSourceDir() + QStringLiteral("/profiles/loop-default.json"),
                        QStringLiteral("--output"),
                        repairedPath,
                        QStringLiteral("--console-format"),
                        QStringLiteral("json") });
        QByteArray stdOut;
        QByteArray stdErr;
        QVERIFY2(test_support::waitForFinishedAndCapture(process, 60000, stdOut, stdErr),
                 qPrintable(QString::fromUtf8(stdErr)));
        QCOMPARE(process.exitCode(), 0);
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(repairedPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    const QVariantList points = host.fixRollbackPoints();
    QVERIFY(!points.isEmpty());
    QVERIFY(host.fixRollbackAvailable());
    QVERIFY(!host.fixRollbackSummary().trimmed().isEmpty());

    // Only revisions Core can actually return to are offered: the as-received input stays a
    // protected retention point and is never presented as a selectable revision.
    for (const QVariant& point : points)
    {
        QCOMPARE(point.toMap().value(QStringLiteral("isOriginalInput")).toBool(), false);
        QVERIFY(!point.toMap().value(QStringLiteral("rollbackId")).toString().isEmpty());
        QVERIFY(!point.toMap().value(QStringLiteral("documentRevisionDigest")).toString().isEmpty());
    }
    const QString rollbackId = points.first().toMap().value(QStringLiteral("rollbackId")).toString();

    // An unknown revision is refused before anything is written, and the refusal writes no
    // revision. (A rollback without a validated profile is refused the same way; Core's
    // rollbackRefusesWithoutProfileOrOnCompromisedChain pins that fail-closed path.)
    QVERIFY(!host.requestFixRollback(QStringLiteral("revision-that-was-never-recorded")));
    QVERIFY2(QDir(directory.path()).entryList(QStringList{ QStringLiteral("*-rollback-*.pdf") }, QDir::Files).isEmpty(),
             "a refused rollback must not write a revision");

    // The bundled loop-default profile is the one profile both surfaces can be given, so pin
    // it deterministically: the request supplies the effective profile the restored revision
    // is revalidated under.
    const QString bundledProfileId = QStringLiteral(":/profiles/loop-default.json");
    if (host.selectedPreflightProfileId() != bundledProfileId)
    {
        QVERIFY2(host.selectPreflightProfile(bundledProfileId),
                 qPrintable(QStringLiteral("the bundled loop-default profile must be selectable; selected '%1'")
                                .arg(host.selectedPreflightProfileId())));
    }
    QCOMPARE(host.selectedPreflightProfileId(), bundledProfileId);

    // Core records the rollback as a new event; the scheduled job's completion is the signal
    // to read it. Reading the history while the worker holds its SQLite connection would
    // contend for the database, so wait on the scheduler, not on a probe of the store.
    pdf::PDFJobScheduler& scheduler = host.sessionForTest()->scheduler();
    bool rollbackFinished = false;
    QObject::connect(&scheduler, &pdf::PDFJobScheduler::jobFinished, &host,
                     [&rollbackFinished](const pdf::PDFJobSnapshot& snapshot)
                     {
                         if (snapshot.operationId.startsWith(QStringLiteral("rollback.")))
                         {
                             rollbackFinished = true;
                         }
                     });

    // The request now SCHEDULES the governed restore: it returns true immediately while the
    // revalidation (which refuses to run on this interactive thread) runs on a worker.
    QVERIFY(host.requestFixRollback(rollbackId));
    QTRY_VERIFY_WITH_TIMEOUT(rollbackFinished, 60000);

    // The rolled-back revision is a new sibling file; the open document is never overwritten.
    const QStringList siblings =
        QDir(directory.path()).entryList(QStringList{ QStringLiteral("*-rollback-*.pdf") }, QDir::Files);
    QVERIFY2(!siblings.isEmpty(), "the rollback must write a new revision beside the document");
    QVERIFY(QFile::exists(sourcePath));

    // Core recorded the rollback as a new event and left the existing history intact.
    const QString historyPath =
        QDir(QFileInfo(repairedPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    QVERIFY2(history.open(&historyError), qPrintable(historyError));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    QVERIFY2(historyError.isEmpty(), qPrintable(historyError));
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const pdf::PDFOperationHistoryEvent& event)
                        { return event.status == pdf::PDFOperationHistoryStatus::RolledBack; }));
    host.openFileUrl(QUrl::fromLocalFile(repairedPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    rollbackFinished = false;
    QVERIFY(host.requestFixRollback(rollbackId));
    host.openFileUrl(QUrl::fromLocalFile(sourcePath));
    QTRY_VERIFY_WITH_TIMEOUT(rollbackFinished, 60000);
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.sessionForTest()->facade().source().path, sourcePath);
}

// ---------------------------------------------------------------------------
// #17: scheduled results are fenced by request identity
// ---------------------------------------------------------------------------
//
// The fence lives in EditorHost::finishPreflightJob and finishActionListJob, which run
// on the scheduler's jobFinished signal. Each test first lets one run complete for real,
// captures that completion, and then hands the shell the same snapshot for a live request
// with one identity field corrupted. The capture is a plain lambda connection because
// PDFJobSnapshot is not a registered metatype.

void EditorHostTest::preflightFencesCompletionsThatLostTheirRequestIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preflight-fence.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    auto* controller = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    QVERIFY2(controller, "the preflight property must expose the controller that holds the request identity");
    pdf::PDFJobScheduler& scheduler = host.sessionForTest()->scheduler();

    pdf::PDFJobSnapshot realCompletion;
    QString watchedJobId;
    QObject::connect(&scheduler, &pdf::PDFJobScheduler::jobFinished, &host,
                     [&realCompletion, &watchedJobId](const pdf::PDFJobSnapshot& snapshot)
                     {
                         if (!watchedJobId.isEmpty() && snapshot.jobId == watchedJobId)
                         {
                             realCompletion = snapshot;
                         }
                     });

    // The live run must be admitted. Its completion is also the exact snapshot the fault
    // cases below corrupt, so the only difference between a published verdict and a
    // fenced one is the identity field each case breaks.
    QVERIFY(host.runPreflight());
    watchedJobId = controller->jobId();
    QVERIFY(!watchedJobId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY(host.preflightStateName() != QStringLiteral("error"));
    QVERIFY(host.hasPreflightReport());
    QVERIFY(!realCompletion.jobId.isEmpty());
    QCOMPARE(realCompletion.jobId, watchedJobId);
    QCOMPARE(realCompletion.status, pdf::PDFJobStatus::Succeeded);

    for (int fault = 0; fault < 4; ++fault)
    {
        QVERIFY(host.runPreflight());
        watchedJobId = controller->jobId();
        QCOMPARE(controller->state(), pdfinteraction::PreflightController::State::Running);
        QVERIFY(!watchedJobId.isEmpty());

        pdf::PDFJobSnapshot corrupted = realCompletion;
        corrupted.jobId = watchedJobId;
        corrupted.documentKey = controller->documentKey();
        corrupted.documentRevision = controller->documentRevision();
        switch (fault)
        {
            case 0:
                // A completion for a revision the document has already left.
                corrupted.documentRevision = controller->documentRevision() + QStringLiteral("-superseded");
                break;
            case 1:
                // A completion for another document that shares this shell.
                corrupted.documentKey = controller->documentKey() + QStringLiteral("-other-document");
                break;
            case 2:
                // A completion for a different operation on the same revision.
                corrupted.operationId = QStringLiteral("preflight.a-profile-that-was-never-selected");
                break;
            default:
                // A completion that is not the scheduled preflight at all.
                corrupted.kind = pdf::PDFJobKind::Other;
                break;
        }

        scheduler.jobFinished(corrupted);

        // The fenced completion publishes nothing; the earlier verdict stays on screen,
        // marked stale, rather than being replaced by the unowned result.
        QCOMPARE(controller->state(), pdfinteraction::PreflightController::State::Stale);
        QVERIFY(controller->hasResult());
        QVERIFY(host.preflightStateName() != QStringLiteral("pass"));
        QVERIFY(!controller->operatorSummary().trimmed().isEmpty());
    }

    // The fence rejects mismatched completions only: a later live run still completes.
    QVERIFY(host.runPreflight());
    QTRY_VERIFY_WITH_TIMEOUT(host.preflightStateName() != QStringLiteral("running"), 60000);
    QVERIFY(host.preflightStateName() != QStringLiteral("error"));
    QVERIFY(host.hasPreflightReport());
}

void EditorHostTest::actionListFencesCompletionsThatLostTheirRequestIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("fence-job.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("fence-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("gh-586-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 3 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);

    auto* controller = qobject_cast<pdfinteraction::ActionListController*>(host.actionList());
    QVERIFY2(controller, "the actionList property must expose the controller that holds the request identity");
    pdf::PDFJobScheduler& scheduler = host.sessionForTest()->scheduler();

    pdf::PDFJobSnapshot realCompletion;
    QString watchedJobId;
    QObject::connect(&scheduler, &pdf::PDFJobScheduler::jobFinished, &host,
                     [&realCompletion, &watchedJobId](const pdf::PDFJobSnapshot& snapshot)
                     {
                         if (!watchedJobId.isEmpty() && snapshot.jobId == watchedJobId)
                         {
                             realCompletion = snapshot;
                         }
                     });

    // The real plan completion is admitted, and it is the snapshot the faults corrupt.
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 60000);
    QVERIFY(host.planActionList());
    watchedJobId = controller->jobId();
    QVERIFY(!watchedJobId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(!realCompletion.jobId.isEmpty());
    QCOMPARE(realCompletion.jobId, watchedJobId);
    QCOMPARE(realCompletion.status, pdf::PDFJobStatus::Succeeded);
    QVERIFY(!host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString().isEmpty());

    for (int fault = 0; fault < 2; ++fault)
    {
        // A stale result clears the validated binding, so every fault needs a fresh
        // validate and plan: the run under test has to be a live request.
        QVERIFY(host.validateActionListRecipe());
        QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 60000);
        QVERIFY(host.planActionList());
        watchedJobId = controller->jobId();
        QCOMPARE(controller->state(), pdfinteraction::ActionListController::State::Planning);

        pdf::PDFJobSnapshot corrupted = realCompletion;
        corrupted.jobId = watchedJobId;
        corrupted.documentKey = controller->documentKey();
        corrupted.documentRevision = controller->documentRevision();
        if (fault == 0)
        {
            // A completion that is not the scheduled Action List work at all.
            corrupted.kind = pdf::PDFJobKind::Preflight;
        }
        else
        {
            // A completion for a different recipe on the same revision.
            corrupted.checkId = QStringLiteral("a-recipe-that-was-never-selected");
        }

        scheduler.jobFinished(corrupted);

        // The controller discards the plan. The shell still names the discarded digest so
        // the surface can show what went stale, so the fence is the lifecycle state.
        QCOMPARE(controller->state(), pdfinteraction::ActionListController::State::Idle);
        QVERIFY(controller->planDigest().isEmpty());
        QCOMPARE(host.fixLifecycleStateName(), QStringLiteral("idle"));
        QVERIFY(!host.fixExecutionArmed());
        QVERIFY(!controller->operatorSummary().trimmed().isEmpty());
    }

    // The fence rejects mismatched completions only: a later live plan still completes.
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 60000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);
    QVERIFY(!host.fixPlanIdentity().value(QStringLiteral("planDigest")).toString().isEmpty());
}

void EditorHostTest::previewFidelityNamesTheOriginAndSwitchesExplicitly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const QString path = directory.filePath(QStringLiteral("preview-fidelity.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("none"));
    QVERIFY(host.previewFidelitySummary().contains(QStringLiteral("No rendered evidence")));
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);

    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("exact"), 30000);
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("fast-canvas"));
    QVERIFY(!host.previewRequiresAuthoritative());
    QVERIFY(!host.ensureAuthoritativePreview());
    QVERIFY(!host.previewFidelitySummary().trimmed().isEmpty());

    host.toggleCurrentPageFidelity();
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("none"));
    QTRY_COMPARE_WITH_TIMEOUT(host.previewFidelityStateName(), QStringLiteral("authoritative"), 30000);
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("output-preview"));
    QVERIFY(!host.previewRequiresAuthoritative());

    const QString overprintPath =
        preflightFixturesDir() + QStringLiteral("/overprint-cmyk-mode1-on.pdf");
    QVERIFY2(QFileInfo::exists(overprintPath), qPrintable(overprintPath));

    EditorHost overprintHost;
    overprintHost.openFileUrl(QUrl::fromLocalFile(overprintPath));
    QTRY_VERIFY_WITH_TIMEOUT(overprintHost.hasDocument(), 30000);
    overprintHost.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_VERIFY_WITH_TIMEOUT(overprintHost.previewFidelityStateName() == QStringLiteral("approximate"), 30000);
    QCOMPARE(overprintHost.previewFidelityOriginName(), QStringLiteral("fast-canvas"));
    QVERIFY(overprintHost.previewRequiresAuthoritative());
    QVERIFY(overprintHost.previewFidelitySummary().contains(QStringLiteral("print-safe output")));

    const QVariantMap approximate = overprintHost.previewFidelityVisual();
    QVERIFY(approximate.value(QStringLiteral("kind")).toString() != QStringLiteral("Passed"));
    QVERIFY(approximate.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success"));

    QVERIFY(overprintHost.ensureAuthoritativePreview());
    QCOMPARE(overprintHost.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(overprintHost.previewFidelityOriginName(), QStringLiteral("none"));
    QVERIFY(!overprintHost.ensureAuthoritativePreview());
    QTRY_COMPARE_WITH_TIMEOUT(overprintHost.previewFidelityStateName(), QStringLiteral("authoritative"), 30000);
    QCOMPARE(overprintHost.previewFidelityOriginName(), QStringLiteral("output-preview"));
    QVERIFY(!overprintHost.previewRequiresAuthoritative());
    QVERIFY(!overprintHost.ensureAuthoritativePreview());
}

void EditorHostTest::refusedRenderCannotBecomeAuthoritativeEvidence()
{
    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(preflightFixturesDir() + QStringLiteral("/overprint-cmyk-mode1-on.pdf")));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    auto* surfaces = host.sessionForTest()->surfaces();
    pdf::PDFResourceBudgetConfig budget;
    budget.setLimit(pdf::PDFResourcePool::RasterTileCache, 1024);
    surfaces->setResourceBudget(std::make_shared<pdf::PDFResourceBudget>(budget));
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QTRY_VERIFY_WITH_TIMEOUT(surfaces->counters().budgetExhausted > 0 || surfaces->counters().rejectedOversize > 0, 30000);
    QVERIFY(!surfaces->diagnosticsForPage(0).has_value());
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));

    host.toggleCurrentPageFidelity();
    QVERIFY(host.pageFidelityIsAuthoritative());
    QVERIFY(!surfaces->diagnosticsForPage(0).has_value());
    QCOMPARE(host.previewFidelityStateName(), QStringLiteral("unavailable"));
    QCOMPARE(host.previewFidelityOriginName(), QStringLiteral("none"));
    QVERIFY(host.previewFidelitySummary().contains(QStringLiteral("No rendered evidence")));
    QVERIFY(!host.ensureAuthoritativePreview());
}

void EditorHostTest::executeApprovedActionListPlanRefusesAnUnreviewedPlan()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const QString documentPath = directory.filePath(QStringLiteral("unreviewed.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(documentPath, &document, true));
    }

    const QString recipePath = directory.filePath(QStringLiteral("unreviewed-recipe.json"));
    {
        QFile recipe(recipePath);
        QVERIFY(recipe.open(QIODevice::WriteOnly));
        recipe.write(QJsonDocument(QJsonObject{
                                       { QStringLiteral("schema"), QStringLiteral("loop-action-list/2") },
                                       { QStringLiteral("id"), QStringLiteral("unreviewed-test") },
                                       { QStringLiteral("name"), QStringLiteral("Bleed correction") },
                                       { QStringLiteral("steps"),
                                         QJsonArray{ QJsonObject{
                                             { QStringLiteral("id"), QStringLiteral("bleed") },
                                             { QStringLiteral("operation"), QStringLiteral("add-bleed") },
                                             { QStringLiteral("params"),
                                               QJsonObject{ { QStringLiteral("bleed_mm"), 3 },
                                                            { QStringLiteral("mode"), QStringLiteral("mirror") } } } } } } })
                         .toJson(QJsonDocument::Compact));
        recipe.close();
    }

    const auto fileDigest = [](const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    };
    const QByteArray sourceDigest = fileDigest(documentPath);
    QVERIFY(!sourceDigest.isEmpty());

    EditorHost host;
    QVERIFY(host.importActionListRecipe(QUrl::fromLocalFile(recipePath)));
    host.openFileUrl(QUrl::fromLocalFile(documentPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 15000);
    QVERIFY(host.selectActionListRecipeForOperation(QStringLiteral("add-bleed")));
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_COMPARE_WITH_TIMEOUT(host.fixLifecycleStateName(), QStringLiteral("preview-ready"), 60000);

    QVERIFY(!host.fixExecutionArmed());
    QVERIFY(!host.executeApprovedActionListPlan());
    QCOMPARE(host.actionListStateName(), QStringLiteral("planned"));
    QCOMPARE(fileDigest(documentPath), sourceDigest);

    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.fixExecutionArmed());
    QVERIFY(host.executeApprovedActionListPlan());
}

QTEST_GUILESS_MAIN(EditorHostTest)

#include "tst_editorhosttest.moc"
