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

#include "pdfpreflightverdict.h"
#include "pdfpreflightaudit.h"
#include "pdfoperationhistorystore.h"
#include "pdfpreflightcertificate.h"
#include "pdfactionlist.h"
#include "pdfdocumentbuilder.h"
#include "pdfrepairoperation.h"
#include "preflightcontroller.h"
#include "preflightengine.h"

#include <QPainter>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

class PreflightVerdictTest : public QObject
{
    Q_OBJECT

private slots:
    void emptyCompleteInspection_isPass();
    void blockingFinding_isFail();
    void budgetExceededWithoutFindings_isIncomplete();
    void activeWaiver_isPassAndRecorded();
    void engineError_isError();
    void unsupportedScopeErrorCode_isIncomplete();
    void unresolvedVariableErrorCode_isIncomplete();
    void cancelledErrorCode_isIncomplete();
    void budgetExceededErrorCode_isIncomplete();
    void reportPassIsDerivedFromVerdict();
    void incompleteInspectionWithoutFindings_isNotPass();
    void cancellationMarkedIncomplete_isNotPass();
    void requiredCheckMissingStatus_isIncomplete();
    void processExitCodes_matchPdfToolContract();
    void budgetExceeded_neverAllowsCertificate();
    void operatorSummary_distinguishesIncompleteFromPass();
    void pageMasterGateMessage_distinguishesIncomplete();
    void actionListStep_budgetExceededIsNotSucceeded();
    void surfacesShareBudgetGuard();
    void editorBudgetExceeded_isIncompleteNeverPass();
    void editorWaivedBlocking_isPass();
    void editorWarningsOnly_isPass();
    void editorEngineError_isError();
    void operatorSummaryIsTranslatable();
    void operatorSummaryIsCurrentWhenTheStateSignalFires();
    void editorWaivedBlockingIsPresentedAsWaived();
    void mandatoryPostflight_requiresProfilePath();
    void mandatoryPostflight_failsClosedOnBlockingFindings();
    void mandatoryPostflight_respectsCancellation();
    void mandatoryPostflight_acceptsResolvedProfileJson();
    void provisionalPass_doesNotAllowCertification();
    void certification_allowsWarningsAndWaivedErrors();
    void certification_rejectsSkippedOrBudgetLimitedChecks();
    void auditRun_appendsCanonicalEvents();
    void certificate_roundTripsAndDetectsTampering();
    void certificate_detectsStaleDecision();
};

namespace
{

pdf::PreflightFinding blockingFinding()
{
    pdf::PreflightFinding finding;
    finding.scope = QStringLiteral("page");
    finding.page = 1;
    finding.type = QStringLiteral("color-mode");
    finding.severity = QStringLiteral("error");
    finding.checkId = QStringLiteral("color-mode");
    finding.message = QStringLiteral("RGB content is not allowed.");
    return finding;
}

pdf::PreflightResult budgetExceededResult()
{
    pdf::PreflightResult result;
    result.pass = true;
    result.inspectionComplete = false;
    result.checkStatuses.append({ QStringLiteral("ink-coverage"),
                                  QStringLiteral("incomplete"),
                                  QStringLiteral("budget-exceeded"),
                                  QStringLiteral("raster-pixels"),
                                  QStringLiteral("raster-tile"),
                                  100,
                                  101,
                                  QStringLiteral("page 1") });
    return result;
}

/// A translator with no .qm file behind it: it answers one message in the
/// Core verdict context, which is exactly what a shipped catalogue would do.
class StubVerdictTranslator final : public QTranslator
{
public:
    bool isEmpty() const override { return false; }

    QString translate(const char* context,
                      const char* sourceText,
                      const char* disambiguation,
                      int n) const override
    {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (QLatin1String(context) == QLatin1String("pdf::PreflightVerdict") &&
            QLatin1String(sourceText) == QLatin1String("No problems found."))
        {
            return QStringLiteral("TRANSLATED-NO-PROBLEMS");
        }
        return {};
    }
};

}   // namespace

void PreflightVerdictTest::emptyCompleteInspection_isPass()
{
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(pdf::PreflightResult());
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Pass);
    QCOMPARE(verdict.reasonCode, QStringLiteral("no-blocking-findings"));
}

void PreflightVerdictTest::blockingFinding_isFail()
{
    pdf::PreflightResult result;
    result.errors.append(blockingFinding());

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Fail);
    QCOMPARE(verdict.blockingFindingIds.size(), 1);
    QVERIFY(verdict.waivedFindingIds.isEmpty());
}

void PreflightVerdictTest::budgetExceededWithoutFindings_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.checkStatuses.append({ QStringLiteral("ink-coverage"),
                                  QStringLiteral("incomplete"),
                                  QStringLiteral("budget-exceeded"),
                                  QStringLiteral("raster-pixels"),
                                  QStringLiteral("raster-tile"),
                                  100,
                                  101,
                                  QStringLiteral("page 1") });

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("budget-exceeded"));
}

void PreflightVerdictTest::activeWaiver_isPassAndRecorded()
{
    const QString documentDigest(64, QLatin1Char('a'));
    const QString profileDigest(64, QLatin1Char('b'));
    pdf::PreflightResult result;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = profileDigest;
    result.errors.append(blockingFinding());

    pdf::PreflightDecision decision;
    decision.findingId = result.errors.first().stableId();
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("Approved by the client.");
    decision.operatorIdentity = QStringLiteral("operator");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest;
    result.decisions.append(decision);

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Pass);
    QCOMPARE(verdict.reasonCode, QStringLiteral("blocking-findings-waived"));
    QCOMPARE(verdict.waivedFindingIds, QStringList{ decision.findingId });
}

void PreflightVerdictTest::engineError_isError()
{
    pdf::PreflightResult result;
    result.errorCode = QStringLiteral("profile-invalid");
    result.errorMessage = QStringLiteral("Profile is malformed.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Error);
    QCOMPARE(verdict.reasonCode, QStringLiteral("profile-invalid"));
    QCOMPARE(verdict.reason, result.errorMessage);
}

void PreflightVerdictTest::unsupportedScopeErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("unsupported-scope");
    result.errorMessage = QStringLiteral("Profile scope is empty or unsupported.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("unsupported-scope"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::unresolvedVariableErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("unresolved-variable");
    result.errorMessage = QStringLiteral("Profile variable 'stock' is unresolved.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("unresolved-variable"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::cancelledErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("cancelled");
    result.errorMessage = QStringLiteral("Preflight was cancelled.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("cancelled"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::budgetExceededErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("budget-exceeded");
    result.errorMessage = QStringLiteral("RasterTile");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("budget-exceeded"));
    QCOMPARE(verdict.reason, QStringLiteral("RasterTile"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::reportPassIsDerivedFromVerdict()
{
    pdf::PreflightResult result;
    result.pass = true;
    result.inspectionComplete = false;
    const QJsonObject report = result.toJson();

    QCOMPARE(report.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(),
             QStringLiteral("incomplete"));
    QVERIFY(!report.value(QStringLiteral("pass")).toBool());
}

void PreflightVerdictTest::incompleteInspectionWithoutFindings_isNotPass()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.pass = true;

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("inspection-incomplete"));
    QVERIFY(!verdict.isPass());
    QVERIFY(!result.toJson().value(QStringLiteral("pass")).toBool());
}

void PreflightVerdictTest::cancellationMarkedIncomplete_isNotPass()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.checkStatuses.append({ QStringLiteral("image-resolution"),
                                  QStringLiteral("incomplete"),
                                  QStringLiteral("cancelled"),
                                  QString(),
                                  QString(),
                                  0,
                                  0,
                                  QStringLiteral("operator cancel") });

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("cancelled"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::requiredCheckMissingStatus_isIncomplete()
{
    pdf::PreflightProfileData profile;
    pdf::PreflightCheckConfig check;
    check.id = QStringLiteral("image-resolution");
    check.required = true;
    check.enabled = true;
    profile.checks.append(check);

    pdf::PreflightResult result;
    result.inspectionComplete = true;

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result, &profile);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("required-check-not-run"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::processExitCodes_matchPdfToolContract()
{
    QCOMPARE(pdf::preflightVerdictProcessExitCode(pdf::PreflightVerdictState::Pass), 0);
    QCOMPARE(pdf::preflightVerdictProcessExitCode(pdf::PreflightVerdictState::Fail), 1);
    QCOMPARE(pdf::preflightVerdictProcessExitCode(pdf::PreflightVerdictState::Incomplete), 8);
    QCOMPARE(pdf::preflightVerdictProcessExitCode(pdf::PreflightVerdictState::Error), 9);
}

void PreflightVerdictTest::budgetExceeded_neverAllowsCertificate()
{
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(budgetExceededResult());
    QVERIFY(!verdict.allowsCertificateIssuance());
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
}

void PreflightVerdictTest::operatorSummary_distinguishesIncompleteFromPass()
{
    const pdf::PreflightVerdict pass = pdf::reducePreflightVerdict(pdf::PreflightResult());
    QCOMPARE(pdf::preflightVerdictOperatorSummary(pass), QStringLiteral("No problems found."));

    const pdf::PreflightVerdict incomplete = pdf::reducePreflightVerdict(budgetExceededResult());
    QVERIFY(pdf::preflightVerdictOperatorSummary(incomplete).startsWith(QStringLiteral("Could not finish inspecting.")));
}

void PreflightVerdictTest::pageMasterGateMessage_distinguishesIncomplete()
{
    const QString message = pdf::preflightGateFailureMessage(QStringLiteral("job.pdf"),
                                                             pdf::PreflightVerdictState::Incomplete,
                                                             false);
    QVERIFY(message.contains(QStringLiteral("could not finish inspecting")));
    QVERIFY(!message.contains(QStringLiteral("failed for")));
}

void PreflightVerdictTest::actionListStep_budgetExceededIsNotSucceeded()
{
    pdf::PDFActionListStepResult step;
    step.status = pdf::PDFActionListStepStatus::Succeeded;
    pdf::applyCanonicalPreflightVerdict(&step, budgetExceededResult());
    QCOMPARE(step.status, pdf::PDFActionListStepStatus::Failed);
    QCOMPARE(step.verdict.value(QStringLiteral("state")).toString(), QStringLiteral("incomplete"));
}

void PreflightVerdictTest::surfacesShareBudgetGuard()
{
    const pdf::PreflightResult result = budgetExceededResult();
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(pdf::preflightVerdictProcessExitCode(verdict.state), 8);
    QVERIFY(!verdict.allowsCertificateIssuance());
    QVERIFY(!verdict.isPass());

    pdf::PDFActionListStepResult step;
    step.status = pdf::PDFActionListStepStatus::Succeeded;
    pdf::applyCanonicalPreflightVerdict(&step, verdict);
    QCOMPARE(step.status, pdf::PDFActionListStepStatus::Failed);

    const QString gate = pdf::preflightGateFailureMessage(QStringLiteral("out.pdf"), verdict.state, false);
    QVERIFY(gate.contains(QStringLiteral("could not finish inspecting")));
}

void PreflightVerdictTest::editorBudgetExceeded_isIncompleteNeverPass()
{
    pdfinteraction::PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));
    QVERIFY(controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), budgetExceededResult()));
    QCOMPARE(controller.state(), pdfinteraction::PreflightController::State::Incomplete);
    QVERIFY(controller.operatorSummary().startsWith(QStringLiteral("Could not finish inspecting.")));
}

void PreflightVerdictTest::provisionalPass_doesNotAllowCertification()
{
    pdf::PreflightResult result;
    result.inspectionComplete = true;
    result.profileIdentity.insert(QStringLiteral("provisional"), true);
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QVERIFY(verdict.allowsCertificateIssuance());
    QVERIFY(!pdf::preflightAllowsCertification(result));
}

void PreflightVerdictTest::certification_allowsWarningsAndWaivedErrors()
{
    const QString documentDigest(64, QLatin1Char('a'));
    const QString profileDigest(64, QLatin1Char('b'));

    pdf::PreflightResult warningResult;
    warningResult.inspectionComplete = true;
    warningResult.documentRevisionDigest = documentDigest;
    warningResult.effectiveProfileDigest = profileDigest;
    warningResult.profileIdentity.insert(QStringLiteral("provisional"), false);
    warningResult.checkStatuses.append({ QStringLiteral("fonts"), QStringLiteral("warning") });
    QVERIFY(pdf::preflightAllowsCertification(warningResult));

    pdf::PreflightResult waivedResult;
    waivedResult.inspectionComplete = true;
    waivedResult.documentRevisionDigest = documentDigest;
    waivedResult.effectiveProfileDigest = profileDigest;
    waivedResult.profileIdentity.insert(QStringLiteral("provisional"), false);
    waivedResult.errors.append(blockingFinding());
    waivedResult.checkStatuses.append({ QStringLiteral("color-mode"), QStringLiteral("failed") });

    pdf::PreflightDecision decision;
    decision.findingId = waivedResult.errors.first().stableId();
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("Approved exception.");
    decision.operatorIdentity = QStringLiteral("operator");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest;
    waivedResult.decisions.append(decision);

    QVERIFY(pdf::preflightAllowsCertification(waivedResult));
}

void PreflightVerdictTest::certification_rejectsSkippedOrBudgetLimitedChecks()
{
    pdf::PreflightResult skipped;
    skipped.inspectionComplete = true;
    skipped.profileIdentity.insert(QStringLiteral("provisional"), false);
    skipped.checkStatuses.append({ QStringLiteral("ink-coverage"), QStringLiteral("skipped"), QStringLiteral("inspection incomplete") });
    QVERIFY(!pdf::preflightAllowsCertification(skipped));

    pdf::PreflightResult budgeted;
    budgeted.inspectionComplete = true;
    budgeted.profileIdentity.insert(QStringLiteral("provisional"), false);
    pdf::PreflightCheckStatus status;
    status.id = QStringLiteral("ink-coverage");
    status.status = QStringLiteral("ok");
    status.budgetKind = QStringLiteral("raster-pixels");
    status.budgetLimit = 10;
    status.budgetAttempted = 11;
    budgeted.checkStatuses.append(status);
    QVERIFY(!pdf::preflightAllowsCertification(budgeted));
}

void PreflightVerdictTest::auditRun_appendsCanonicalEvents()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString documentPath = directory.filePath(QStringLiteral("audit.pdf"));
    const QByteArray document("%PDF-1.4\n%%EOF\n");
    const QString documentDigest =
        QString::fromLatin1(QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex());

    pdf::PreflightResult result;
    result.inspectionComplete = true;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = QString(64, QLatin1Char('d'));
    result.profileIdentity.insert(QStringLiteral("provisional"), false);
    result.checkStatuses.append({ QStringLiteral("bleed"), QStringLiteral("ok") });

    const pdf::PDFOperationResult appended =
        pdf::appendPreflightAuditRun(documentPath,
                                     document,
                                     result,
                                     pdf::PDFOperationHistoryStatus::Accepted,
                                     QStringLiteral("test"));
    QVERIFY2(appended, qPrintable(appended.getErrorMessage()));

    pdf::PDFOperationHistoryStore history(
        QDir(QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3")));
    QString error;
    QVERIFY2(history.open(&error), qPrintable(error));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 3);
    QCOMPARE(events.at(0).kind, pdf::PDFOperationHistoryEventKind::DocumentOpened);
    QCOMPARE(events.at(1).kind, pdf::PDFOperationHistoryEventKind::PreflightRun);
    QCOMPARE(events.at(1).status, pdf::PDFOperationHistoryStatus::Running);
    QCOMPARE(events.at(2).kind, pdf::PDFOperationHistoryEventKind::PreflightRun);
    QCOMPARE(events.at(2).status, pdf::PDFOperationHistoryStatus::Accepted);
    QVERIFY(history.verify().verified);
}

void PreflightVerdictTest::certificate_roundTripsAndDetectsTampering()
{
    const QByteArray document("certified document revision");
    const QString documentDigest = QString::fromLatin1(QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex());
    const QString profileDigest(64, QLatin1Char('a'));

    pdf::PreflightResult result;
    result.inspectionComplete = true;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = profileDigest;
    result.profileIdentity.insert(QStringLiteral("provisional"), false);
    result.checkStatuses.append({ QStringLiteral("bleed"), QStringLiteral("ok") });

    pdf::PDFOperationHistoryEvent preflight;
    preflight.sequence = 1;
    preflight.entryId = QUuid::createUuid();
    preflight.executionId = QUuid::createUuid();
    preflight.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
    preflight.status = pdf::PDFOperationHistoryStatus::Accepted;
    preflight.documentRevisionDigest = documentDigest;
    preflight.effectiveProfileDigest = profileDigest;
    preflight.resultSummary = result.toJson(QStringLiteral("document.pdf"));
    preflight.createdUtc = QDateTime::currentDateTimeUtc();
    preflight.eventHash = pdf::computeOperationHistoryEventHash(preflight, {});

    pdf::PreflightCertificate certificate;
    QString error;
    QVERIFY(pdf::issuePreflightCertificate(result,
                                           result.toJson(QStringLiteral("document.pdf")),
                                           document,
                                           { preflight },
                                           QStringLiteral("operator"),
                                           certificate,
                                           error));
    QVERIFY(error.isEmpty());
    QCOMPARE(certificate.auditChainHeadEventId, preflight.entryId.toString(QUuid::WithoutBraces));

    pdf::PDFOperationHistoryEvent mismatchedReport = preflight;
    mismatchedReport.resultSummary.insert(QStringLiteral("tampered"), true);
    mismatchedReport.eventHash = pdf::computeOperationHistoryEventHash(mismatchedReport, {});
    pdf::PreflightCertificate refusedCertificate;
    QString refusedError;
    QVERIFY(!pdf::issuePreflightCertificate(result,
                                            result.toJson(QStringLiteral("document.pdf")),
                                            document,
                                            { mismatchedReport },
                                            QStringLiteral("operator"),
                                            refusedCertificate,
                                            refusedError));
    QVERIFY(!refusedError.isEmpty());

    pdf::PDFOperationHistoryEvent issuance;
    issuance.sequence = 2;
    issuance.entryId = QUuid::createUuid();
    issuance.executionId = preflight.executionId;
    issuance.kind = pdf::PDFOperationHistoryEventKind::CertificateIssued;
    issuance.status = pdf::PDFOperationHistoryStatus::Running;
    issuance.documentRevisionDigest = certificate.documentRevisionDigest;
    issuance.effectiveProfileDigest = certificate.effectiveProfileDigest;
    issuance.approval.decisionReference = certificate.certificateId;
    issuance.resultSummary = QJsonObject{
        { QStringLiteral("certificate_id"), certificate.certificateId },
        { QStringLiteral("report_digest"), certificate.reportDigest },
        { QStringLiteral("certificate"), certificate.toJson() }
    };
    issuance.previousEventHash = preflight.eventHash;
    issuance.createdUtc = QDateTime::currentDateTimeUtc();
    issuance.eventHash = pdf::computeOperationHistoryEventHash(issuance, issuance.previousEventHash);
    const QList<pdf::PDFOperationHistoryEvent> history{ preflight, issuance };

    pdf::PreflightCertificate parsed;
    QVERIFY(pdf::PreflightCertificate::fromJson(certificate.toJson(), parsed, error));
    QCOMPARE(pdf::verifyPreflightCertificate(parsed, document, history).state,
             pdf::PreflightCertificateState::Valid);
    QCOMPARE(pdf::verifyPreflightCertificate(parsed, QByteArray("changed"), history).state,
             pdf::PreflightCertificateState::InvalidDocumentChanged);

    issuance.resultSummary.insert(QStringLiteral("tampered"), true);
    QCOMPARE(pdf::verifyPreflightCertificate(parsed, document, { preflight, issuance }).state,
             pdf::PreflightCertificateState::InvalidAuditChainBroken);
}

void PreflightVerdictTest::certificate_detectsStaleDecision()
{
    const QByteArray document("waived document revision");
    const QString documentDigest = QString::fromLatin1(QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex());
    const QString profileDigest(64, QLatin1Char('c'));

    pdf::PreflightResult result;
    result.inspectionComplete = true;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = profileDigest;
    result.profileIdentity.insert(QStringLiteral("provisional"), false);
    result.errors.append(blockingFinding());
    result.checkStatuses.append({ QStringLiteral("color-mode"), QStringLiteral("failed") });

    pdf::PreflightDecision decision;
    decision.findingId = result.errors.first().stableId();
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("Approved exception.");
    decision.operatorIdentity = QStringLiteral("operator");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest;
    result.decisions.append(decision);
    const QString decisionId = pdf::preflightDecisionIdentity(decision);

    pdf::PDFOperationHistoryEvent preflight;
    preflight.sequence = 1;
    preflight.entryId = QUuid::createUuid();
    preflight.executionId = QUuid::createUuid();
    preflight.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
    preflight.status = pdf::PDFOperationHistoryStatus::Accepted;
    preflight.documentRevisionDigest = documentDigest;
    preflight.effectiveProfileDigest = profileDigest;
    preflight.resultSummary = result.toJson(QStringLiteral("document.pdf"));
    preflight.createdUtc = QDateTime::currentDateTimeUtc();
    preflight.eventHash = pdf::computeOperationHistoryEventHash(preflight, {});

    pdf::PDFOperationHistoryEvent recorded;
    recorded.sequence = 2;
    recorded.entryId = QUuid::createUuid();
    recorded.executionId = preflight.executionId;
    recorded.kind = pdf::PDFOperationHistoryEventKind::DecisionRecorded;
    recorded.status = pdf::PDFOperationHistoryStatus::Running;
    recorded.documentRevisionDigest = documentDigest;
    recorded.effectiveProfileDigest = profileDigest;
    recorded.resultSummary = QJsonObject{ { QStringLiteral("decision_id"), decisionId } };
    recorded.approval.decisionReference = decisionId;
    recorded.previousEventHash = preflight.eventHash;
    recorded.createdUtc = QDateTime::currentDateTimeUtc();
    recorded.eventHash = pdf::computeOperationHistoryEventHash(recorded, recorded.previousEventHash);

    pdf::PreflightCertificate certificate;
    QString error;
    QVERIFY(pdf::issuePreflightCertificate(result,
                                           result.toJson(QStringLiteral("document.pdf")),
                                           document,
                                           { preflight, recorded },
                                           QStringLiteral("operator"),
                                           certificate,
                                           error));
    QCOMPARE(certificate.waivedErrorCount, 1);
    QCOMPARE(certificate.coveringDecisionIds, QStringList{ decisionId });

    pdf::PDFOperationHistoryEvent issuance;
    issuance.sequence = 3;
    issuance.entryId = QUuid::createUuid();
    issuance.executionId = preflight.executionId;
    issuance.kind = pdf::PDFOperationHistoryEventKind::CertificateIssued;
    issuance.status = pdf::PDFOperationHistoryStatus::Running;
    issuance.documentRevisionDigest = certificate.documentRevisionDigest;
    issuance.effectiveProfileDigest = certificate.effectiveProfileDigest;
    issuance.approval.decisionReference = certificate.certificateId;
    issuance.resultSummary = QJsonObject{
        { QStringLiteral("certificate_id"), certificate.certificateId },
        { QStringLiteral("report_digest"), certificate.reportDigest },
        { QStringLiteral("certificate"), certificate.toJson() }
    };
    issuance.previousEventHash = recorded.eventHash;
    issuance.createdUtc = QDateTime::currentDateTimeUtc();
    issuance.eventHash = pdf::computeOperationHistoryEventHash(issuance, issuance.previousEventHash);

    QList<pdf::PDFOperationHistoryEvent> history{ preflight, recorded, issuance };
    QCOMPARE(pdf::verifyPreflightCertificate(certificate, document, history).state,
             pdf::PreflightCertificateState::Valid);

    pdf::PDFOperationHistoryEvent invalidated;
    invalidated.sequence = 4;
    invalidated.entryId = QUuid::createUuid();
    invalidated.executionId = preflight.executionId;
    invalidated.kind = pdf::PDFOperationHistoryEventKind::DecisionInvalidated;
    invalidated.status = pdf::PDFOperationHistoryStatus::Running;
    invalidated.approval.decisionReference = decisionId;
    invalidated.resultSummary = QJsonObject{ { QStringLiteral("decision_id"), decisionId } };
    invalidated.previousEventHash = issuance.eventHash;
    invalidated.createdUtc = QDateTime::currentDateTimeUtc();
    invalidated.eventHash = pdf::computeOperationHistoryEventHash(invalidated, invalidated.previousEventHash);
    history.append(invalidated);

    QCOMPARE(pdf::verifyPreflightCertificate(certificate, document, history).state,
             pdf::PreflightCertificateState::InvalidDecisionStale);
}

void PreflightVerdictTest::editorWaivedBlocking_isPass()
{
    const QString documentDigest(64, QLatin1Char('a'));
    const QString profileDigest(64, QLatin1Char('b'));
    pdf::PreflightResult result;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = profileDigest;
    result.errors.append(blockingFinding());
    pdf::PreflightDecision decision;
    decision.findingId = result.errors.first().stableId();
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("Approved by the client.");
    decision.operatorIdentity = QStringLiteral("operator");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest;
    result.decisions.append(decision);

    pdfinteraction::PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));
    QVERIFY(controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), result));
    QCOMPARE(controller.state(), pdfinteraction::PreflightController::State::Pass);
}

void PreflightVerdictTest::editorWarningsOnly_isPass()
{
    pdf::PreflightFinding warning;
    warning.scope = QStringLiteral("page");
    warning.page = 1;
    warning.type = QStringLiteral("fonts");
    warning.severity = QStringLiteral("warning");
    warning.checkId = QStringLiteral("fonts");
    warning.message = QStringLiteral("Embedded subset.");
    pdf::PreflightResult result;
    result.warnings.append(warning);

    pdfinteraction::PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));
    QVERIFY(controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), result));
    QCOMPARE(controller.state(), pdfinteraction::PreflightController::State::Pass);
    QCOMPARE(controller.operatorSummary(), QStringLiteral("No problems found."));
}

void PreflightVerdictTest::editorEngineError_isError()
{
    pdf::PreflightResult result;
    result.errorCode = QStringLiteral("profile-invalid");
    result.errorMessage = QStringLiteral("Profile is malformed.");

    pdfinteraction::PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));
    QVERIFY(controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), result));
    QCOMPARE(controller.state(), pdfinteraction::PreflightController::State::Error);
}

void PreflightVerdictTest::operatorSummaryIsTranslatable()
{
    StubVerdictTranslator translator;
    QCoreApplication::installTranslator(&translator);

    pdf::PreflightVerdict pass;
    pass.state = pdf::PreflightVerdictState::Pass;
    QCOMPARE(pdf::preflightVerdictOperatorSummary(pass), QStringLiteral("TRANSLATED-NO-PROBLEMS"));

    QCoreApplication::removeTranslator(&translator);
}

void PreflightVerdictTest::operatorSummaryIsCurrentWhenTheStateSignalFires()
{
    pdfinteraction::PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));

    // stateChanged is operatorSummary's notifier: whatever it reads must already
    // be the new value, or the pane shows the previous run's copy.
    QString staleSummaryAtSignal;
    const QMetaObject::Connection staleConnection = QObject::connect(
        &controller, &pdfinteraction::PreflightController::stateChanged, &controller,
        [&controller, &staleSummaryAtSignal](pdfinteraction::PreflightController::State state)
        {
            if (state == pdfinteraction::PreflightController::State::Stale)
            {
                staleSummaryAtSignal = controller.operatorSummary();
            }
        });
    controller.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-2"));
    QObject::disconnect(staleConnection);
    QCOMPARE(staleSummaryAtSignal, QStringLiteral("Preflight is stale for the current revision."));

    QString cancelledSummaryAtSignal;
    const QMetaObject::Connection cancelledConnection = QObject::connect(
        &controller, &pdfinteraction::PreflightController::stateChanged, &controller,
        [&controller, &cancelledSummaryAtSignal](pdfinteraction::PreflightController::State state)
        {
            if (state == pdfinteraction::PreflightController::State::Cancelled)
            {
                cancelledSummaryAtSignal = controller.operatorSummary();
            }
        });
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-3"), {}, QStringLiteral("job-2"));
    QVERIFY(controller.cancelRun(QStringLiteral("job-2")));
    QObject::disconnect(cancelledConnection);
    QCOMPARE(cancelledSummaryAtSignal, QStringLiteral("Preflight was cancelled."));
}

void PreflightVerdictTest::editorWaivedBlockingIsPresentedAsWaived()
{
    const QString documentDigest(64, QLatin1Char('a'));
    const QString profileDigest(64, QLatin1Char('b'));
    pdf::PreflightResult waived;
    waived.documentRevisionDigest = documentDigest;
    waived.effectiveProfileDigest = profileDigest;
    waived.errors.append(blockingFinding());
    pdf::PreflightDecision decision;
    decision.findingId = waived.errors.first().stableId();
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("Approved by the client.");
    decision.operatorIdentity = QStringLiteral("operator");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest;
    waived.decisions.append(decision);

    pdfinteraction::PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));
    QVERIFY(controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), waived));
    QCOMPARE(controller.state(), pdfinteraction::PreflightController::State::Pass);

    pdfinteraction::PreflightFindingsModel* model = controller.findingsModel();
    const pdfinteraction::PreflightFindingView* view = model->finding(waived.errors.first().stableId());
    QVERIFY(view);
    QVERIFY(view->waived);
    QCOMPARE(model->data(model->index(0), pdfinteraction::PreflightFindingsModel::WaivedRole).toBool(), true);
    QCOMPARE(model->severityMap().value(view->id), pdfinteraction::OverlaySeverity::Info);

    // A finding with no active disposition keeps its blocking presentation, so
    // this cannot pass by marking everything waived.
    pdf::PreflightResult blocking;
    blocking.errors.append(blockingFinding());
    pdfinteraction::PreflightController blockingController;
    blockingController.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-2"));
    QVERIFY(blockingController.acceptResult(QStringLiteral("job-2"), QStringLiteral("rev-1"), blocking));
    const pdfinteraction::PreflightFindingView* blockingView =
        blockingController.findingsModel()->finding(blocking.errors.first().stableId());
    QVERIFY(blockingView);
    QVERIFY(!blockingView->waived);
    QCOMPARE(blockingController.findingsModel()->severityMap().value(blockingView->id),
             pdfinteraction::OverlaySeverity::Error);
}

void PreflightVerdictTest::mandatoryPostflight_requiresProfilePath()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();
    pdf::PreflightVerdict verdict;
    const pdf::PDFOperationResult result = pdf::runMandatoryPostflight(&document, {}, &verdict);
    QVERIFY(!result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Error);
}

void PreflightVerdictTest::mandatoryPostflight_failsClosedOnBlockingFindings()
{
    pdf::PDFDocumentBuilder builder;
    const QRectF mediaBox(0, 0, 180, 180);
    const pdf::PDFObjectReference page = builder.appendPage(mediaBox);
    builder.setPageTrimBox(page, mediaBox.adjusted(10, 10, -10, -10));
    pdf::PDFPageContentStreamBuilder pageContentStreamBuilder(&builder,
                                                              pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = pageContentStreamBuilder.begin(page))
    {
        painter->fillRect(mediaBox.adjusted(10, 10, -10, -10), Qt::black);
        pageContentStreamBuilder.end(painter);
    }
    pdf::PDFDocument document = builder.build();
    pdf::PreflightVerdict verdict;
    const QString profilePath = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/profiles/loop-default.json");
    const pdf::PDFOperationResult result = pdf::runMandatoryPostflight(&document, profilePath, &verdict);
    QVERIFY(!result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Fail);
}

class AlwaysCancelledPostflightControl final : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override
    {
        return true;
    }
};

void PreflightVerdictTest::mandatoryPostflight_respectsCancellation()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();
    pdf::PreflightVerdict verdict;
    AlwaysCancelledPostflightControl control;
    pdf::MandatoryPostflightOptions options;
    options.operationControl = &control;
    const QString profilePath = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/profiles/loop-default.json");
    const pdf::PDFOperationResult result = pdf::runMandatoryPostflight(&document, profilePath, &verdict, nullptr, options);
    QVERIFY(!result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("cancelled"));
}

void PreflightVerdictTest::mandatoryPostflight_acceptsResolvedProfileJson()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();
    pdf::PreflightVerdict verdict;
    const QString profilePath = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/profiles/loop-default.json");
    QJsonObject profile;
    QString profileError;
    QVERIFY(pdf::PreflightEngine::loadProfile(profilePath, profile, profileError));
    pdf::MandatoryPostflightOptions options;
    options.profileJson = profile;
    const pdf::PDFOperationResult result = pdf::runMandatoryPostflight(&document, {}, &verdict, nullptr, options);
    QVERIFY(!result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Fail);
}

QTEST_GUILESS_MAIN(PreflightVerdictTest)

#include "tst_preflightverdicttest.moc"
