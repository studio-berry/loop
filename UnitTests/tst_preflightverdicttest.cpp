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
#include "pdfpreflightevidencebundle.h"
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
#include <QJsonArray>
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
    void receiptIdentity_matchesGoldenVector();
    void receiptTerminalStates_data();
    void receiptTerminalStates();
    void receiptRejectsMismatchedProvenance();
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
    void auditRun_preservesEffectiveRestrictions();
    void certificate_roundTripsAndDetectsTampering();
    void certificate_detectsStaleDecision();
    void certificate_bindsRestrictionDigest();
    void evidenceBundle_bindsIdentitiesAndVerifiesOffline();
    void evidenceBundle_bindsGovernedSignOff();
    void evidenceBundle_detectsTamperedMembers();
    void evidenceBundle_carriesNoRawPaths();
};

namespace
{

pdf::PreflightCheckStatus makeCheckStatus(const QString& id, const QString& status, const QString& reason = QString())
{
    pdf::PreflightCheckStatus entry;
    entry.id = id;
    entry.status = status;
    entry.reason = reason;
    return entry;
}

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
    result.checkStatuses.append(pdf::PreflightCheckStatus{ QStringLiteral("ink-coverage"),
                                                           QStringLiteral("incomplete"),
                                                           QStringLiteral("budget-exceeded"),
                                                           QJsonObject{},
                                                           QStringList{},
                                                           QStringLiteral("raster-pixels"),
                                                           QStringLiteral("raster-tile"),
                                                           100,
                                                           101,
                                                           QStringLiteral("page 1") });
    return result;
}

struct ReceiptFixture
{
    pdf::PreflightResult result;
    pdf::PreflightProfileData profile;
    pdf::PDFRevisionIdentity revision;
    pdf::PDFEvidenceGraph evidence;

    ReceiptFixture()
    {
        result.documentRevisionDigest = QString(64, QLatin1Char('a'));
        result.effectiveProfileDigest = QString(64, QLatin1Char('b'));
        result.coverageScope = QJsonObject{
            { QStringLiteral("claim"), QStringLiteral("Limited to enabled checks.") },
            { QStringLiteral("enabled_checks"), QJsonArray{ QStringLiteral("bleed") } }
        };
        result.checkStatuses.append(makeCheckStatus(QStringLiteral("bleed"), QStringLiteral("ok")));
        profile.effectiveDigest = result.effectiveProfileDigest;
        pdf::PreflightCheckConfig check;
        check.id = QStringLiteral("bleed");
        check.enabled = true;
        check.required = true;
        profile.checks.append(check);
        revision.document.documentId = QStringLiteral("receipt-fixture");
        revision.documentRevision = 1;
    }
};

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

namespace
{

QString sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

/// One certified revision with a path-bearing report, a warning whose message
/// names a path, a retained rollback point, and a verified three-event chain.
struct BundleFixture
{
    QByteArray document;
    QJsonObject report;
    pdf::PreflightCertificate certificate;
    pdf::PreflightEvidenceBundleRequest request;

    pdf::PreflightEvidenceBundle bundle;
};

bool buildBundleFixture(const QString& documentPath, BundleFixture& fixture, QString& error)
{
    fixture = {};
    fixture.document = QByteArrayLiteral("certified bundle revision");
    const QString documentDigest = sha256Hex(fixture.document);
    const QString profileDigest(64, QLatin1Char('b'));
    const QString artifactDigest(64, QLatin1Char('d'));

    pdf::PreflightResult result;
    result.inspectionComplete = true;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = profileDigest;
    result.profileName = QStringLiteral("Bundle fixture");
    result.profileIdentity = QJsonObject{ { QStringLiteral("id"), QStringLiteral("bundle-fixture") },
                                          { QStringLiteral("version"), QStringLiteral("1.0.0") },
                                          { QStringLiteral("provisional"), false },
                                          { QStringLiteral("digest"), profileDigest },
                                          { QStringLiteral("source_path"), documentPath } };
    result.coverageScope = QJsonObject{
        { QStringLiteral("claim"), QStringLiteral("Loop does not claim formal GWG conformance.") },
        { QStringLiteral("enabled_checks"), QJsonArray{ QStringLiteral("bleed") } }
    };
    result.checkStatuses.append(makeCheckStatus(QStringLiteral("bleed"), QStringLiteral("ok")));

    pdf::PreflightFinding warning;
    warning.scope = QStringLiteral("document");
    warning.type = QStringLiteral("image-resolution");
    warning.severity = QStringLiteral("warning");
    warning.checkId = QStringLiteral("image-resolution");
    warning.message = QStringLiteral("Low resolution image retained; see %1").arg(documentPath);
    result.warnings.append(warning);

    {
        pdf::PreflightDecision decision;
        decision.findingId = QStringLiteral("image-resolution");
        decision.kind = pdf::PreflightDecisionKind::Accept;
        decision.justification = QStringLiteral("Accepted for this run: job ticket at %1").arg(documentPath);
        decision.operatorIdentity = QStringLiteral("operator");
        decision.timestampUtc = QDateTime::currentDateTimeUtc();
        decision.documentRevisionDigest = documentDigest;
        decision.effectiveProfileDigest = profileDigest;
        result.decisions.append(decision);
    }

    fixture.report = result.toJson(documentPath);

    pdf::PDFOperationHistoryEvent opened;
    opened.sequence = 1;
    opened.entryId = QUuid::createUuid();
    opened.executionId = QUuid::createUuid();
    opened.kind = pdf::PDFOperationHistoryEventKind::DocumentOpened;
    opened.status = pdf::PDFOperationHistoryStatus::Running;
    opened.operatorIdentity = QStringLiteral("operator");
    opened.documentRevisionDigest = documentDigest;
    opened.effectiveProfileDigest = profileDigest;
    opened.createdUtc = QDateTime::currentDateTimeUtc();
    opened.eventHash = pdf::computeOperationHistoryEventHash(opened, {});

    pdf::PDFOperationHistoryEvent preflight;
    preflight.sequence = 2;
    preflight.entryId = QUuid::createUuid();
    preflight.executionId = opened.executionId;
    preflight.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
    preflight.status = pdf::PDFOperationHistoryStatus::Accepted;
    preflight.operatorIdentity = QStringLiteral("operator");
    preflight.documentRevisionDigest = documentDigest;
    preflight.effectiveProfileDigest = profileDigest;
    preflight.resultSummary = pdf::redactSensitiveJson(fixture.report).toObject();
    preflight.previousEventHash = opened.eventHash;
    preflight.createdUtc = QDateTime::currentDateTimeUtc();
    preflight.eventHash = pdf::computeOperationHistoryEventHash(preflight, preflight.previousEventHash);

    const QList<pdf::PDFOperationHistoryEvent> preflightChain{ opened, preflight };

    if (!pdf::issuePreflightCertificate(result,
                                        fixture.report,
                                        fixture.document,
                                        preflightChain,
                                        QStringLiteral("operator"),
                                        fixture.certificate,
                                        error))
    {
        return false;
    }

    pdf::PDFOperationHistoryEvent issuance;
    issuance.sequence = 3;
    issuance.entryId = QUuid::createUuid();
    issuance.executionId = opened.executionId;
    issuance.kind = pdf::PDFOperationHistoryEventKind::CertificateIssued;
    issuance.status = pdf::PDFOperationHistoryStatus::Running;
    issuance.operatorIdentity = QStringLiteral("PdfTool");
    issuance.documentRevisionDigest = documentDigest;
    issuance.effectiveProfileDigest = profileDigest;
    issuance.approval.decisionReference = fixture.certificate.certificateId;
    issuance.resultSummary = QJsonObject{
        { QStringLiteral("certificate_id"), fixture.certificate.certificateId },
        { QStringLiteral("report_digest"), fixture.certificate.reportDigest }
    };
    issuance.previousEventHash = preflight.eventHash;
    issuance.createdUtc = QDateTime::currentDateTimeUtc();
    issuance.eventHash = pdf::computeOperationHistoryEventHash(issuance, issuance.previousEventHash);

    pdf::PDFRollbackPoint rollback;
    rollback.rollbackId = QStringLiteral("rollback-1");
    rollback.auditEventId = preflight.entryId;
    rollback.documentRevisionDigest = documentDigest;
    rollback.createdAtUtc = QDateTime::currentDateTimeUtc();
    rollback.artifactPath = QStringLiteral("C:/jobs/acme/.loop-history/artifacts/dd/%1").arg(artifactDigest);
    rollback.artifactBytes = fixture.document.size();
    rollback.operationId = QStringLiteral("preflight");
    rollback.planSummary = QStringLiteral("Retained revision of %1").arg(documentPath);
    rollback.isOriginalInput = true;

    fixture.request.documentBytes = fixture.document;
    fixture.request.report = fixture.report;
    fixture.request.decisions = result.decisions;
    fixture.request.certificate = fixture.certificate;
    fixture.request.history = QList<pdf::PDFOperationHistoryEvent>{ opened, preflight, issuance };
    fixture.request.rollbackPoints = QList<pdf::PDFRollbackPoint>{ rollback };
    fixture.request.producer = QStringLiteral("UnitTests");

    return pdf::buildPreflightEvidenceBundle(fixture.request, fixture.bundle, error);
}

QString bundleFindings(const pdf::PreflightEvidenceBundleVerification& verification)
{
    QStringList lines;
    for (const pdf::PreflightEvidenceBundleFinding& finding : verification.findings)
    {
        lines.append(QStringLiteral("%1 [%2] %3").arg(finding.code, finding.member, finding.message));
    }
    return QStringLiteral("%1 :: %2").arg(verification.summary, lines.join(QStringLiteral(" | ")));
}

bool hasFinding(const pdf::PreflightEvidenceBundleVerification& verification,
                const QString& code,
                const QString& member = QString())
{
    for (const pdf::PreflightEvidenceBundleFinding& finding : verification.findings)
    {
        if (finding.code == code && (member.isEmpty() || finding.member == member))
        {
            return true;
        }
    }
    return false;
}

bool copyBundle(const QString& source, const QString& target)
{
    QDir targetDir(target);
    if (!QDir().mkpath(target))
    {
        return false;
    }
    for (const QString& name : QDir(source).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name))
    {
        if (!QFile::copy(QDir(source).filePath(name), targetDir.filePath(name)))
        {
            return false;
        }
    }
    return true;
}

bool rewriteMember(const QString& directory, const QString& name, const QByteArray& content)
{
    QFile file(QDir(directory).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    return file.write(content) == content.size();
}

QByteArray readMember(const QString& directory, const QString& name)
{
    QFile file(QDir(directory).filePath(name));
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return file.readAll();
}

/// True when any object in the document carries the key, at any depth.
bool containsKey(const QJsonValue& value, const QString& key)
{
    if (value.isObject())
    {
        const QJsonObject object = value.toObject();
        if (object.contains(key))
        {
            return true;
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            if (containsKey(it.value(), key))
            {
                return true;
            }
        }
        return false;
    }
    if (value.isArray())
    {
        for (const QJsonValue& item : value.toArray())
        {
            if (containsKey(item, key))
            {
                return true;
            }
        }
    }
    return false;
}

}   // namespace

void PreflightVerdictTest::evidenceBundle_bindsIdentitiesAndVerifiesOffline()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString documentPath = temporary.filePath(QStringLiteral("artwork.pdf"));

    BundleFixture fixture;
    QString error;
    QVERIFY2(buildBundleFixture(documentPath, fixture, error), qPrintable(error));
    QVERIFY(error.isEmpty());

    const QString bundleDirectory = temporary.filePath(QStringLiteral("bundle"));
    QVERIFY2(pdf::writePreflightEvidenceBundle(fixture.bundle, bundleDirectory, error), qPrintable(error));

    const pdf::PreflightEvidenceBundleVerification verification =
        pdf::verifyPreflightEvidenceBundle(bundleDirectory, &error);
    QVERIFY2(verification.valid, qPrintable(bundleFindings(verification)));
    QCOMPARE(verification.membersChecked, 4);

    // The manifest carries the effective profile, the coverage scope and the
    // revision digest, so a reader without Loop does not have to trust the
    // report to find them.
    const QJsonObject manifest = fixture.bundle.manifest;
    QCOMPARE(manifest.value(QStringLiteral("schema")).toString(),
             pdf::preflightEvidenceBundleSchemaKind());
    QCOMPARE(manifest.value(QStringLiteral("document")).toObject().value(QStringLiteral("revision_digest")).toString(),
             sha256Hex(fixture.document));
    QCOMPARE(manifest.value(QStringLiteral("document")).toObject().value(QStringLiteral("byte_count")).toInt(),
             fixture.document.size());
    QCOMPARE(manifest.value(QStringLiteral("document")).toObject().value(QStringLiteral("source_path_included")).toBool(true),
             false);
    QCOMPARE(manifest.value(QStringLiteral("effective_profile")).toObject().value(QStringLiteral("digest")).toString(),
             fixture.certificate.effectiveProfileDigest);
    QCOMPARE(manifest.value(QStringLiteral("coverage_scope")).toObject(),
             fixture.report.value(QStringLiteral("coverage_scope")).toObject());
    QVERIFY(!manifest.value(QStringLiteral("coverage_scope")).toObject().isEmpty());
    QCOMPARE(manifest.value(QStringLiteral("authority")).toObject().value(QStringLiteral("canonical_state")).toString(),
             QStringLiteral("internal"));
    QCOMPARE(manifest.value(QStringLiteral("certificate")).toObject().value(QStringLiteral("certificate_id")).toString(),
             fixture.certificate.certificateId);
    QCOMPARE(manifest.value(QStringLiteral("report")).toObject().value(QStringLiteral("certificate_binding")).toString(),
             QStringLiteral("path-omitted"));
    QCOMPARE(manifest.value(QStringLiteral("decisions")).toArray().size(), 1);
    QCOMPARE(manifest.value(QStringLiteral("history")).toObject().value(QStringLiteral("event_count")).toInt(), 3);
    QCOMPARE(manifest.value(QStringLiteral("members")).toArray().size(), 4);

    // A certificate that does not bind the supplied report is refused rather
    // than shipped with an unprovable binding.
    const pdf::PreflightEvidenceBundleRequest request = fixture.request;
    pdf::PreflightEvidenceBundleRequest unbound = request;
    QJsonObject mutatedReport = unbound.report;
    mutatedReport.insert(QStringLiteral("verdict_comment"), QStringLiteral("edited after certification"));
    unbound.report = mutatedReport;
    pdf::PreflightEvidenceBundle refused;
    QVERIFY(!pdf::buildPreflightEvidenceBundle(unbound, refused, error));
    QVERIFY2(error.contains(QStringLiteral("does not bind")), qPrintable(error));

    // The certificate's issuance point is inside the exported slice even though
    // the canonical chain continues past it: the exported head is the later
    // CertificateIssued event, and the certificate's head is present as an event.
    const QString historyHead =
        manifest.value(QStringLiteral("history")).toObject().value(QStringLiteral("head_event_id")).toString();
    QVERIFY(!historyHead.isEmpty());
    QVERIFY(historyHead != fixture.certificate.auditChainHeadEventId);
    const pdf::PreflightEvidenceBundleMember* historyMember =
        fixture.bundle.findMember(pdf::preflightEvidenceBundleHistoryMember());
    QVERIFY(historyMember != nullptr);
    QVERIFY(historyMember->content.contains(fixture.certificate.auditChainHeadEventId.toUtf8()));

    // Rollback references are digest-addressed.
    const QJsonObject rollback =
        manifest.value(QStringLiteral("rollback_references")).toArray().first().toObject();
    QCOMPARE(rollback.value(QStringLiteral("artifact_sha256")).toString(), QString(64, QLatin1Char('d')));
    QVERIFY(!rollback.contains(QStringLiteral("artifact_path")));

    // The exported chain is path-redacted, and the manifest says so together
    // with a digest over the canonical chain.
    const QJsonObject history = manifest.value(QStringLiteral("history")).toObject();
    QCOMPARE(history.value(QStringLiteral("chain_mode")).toString(), QStringLiteral("path-redacted"));
    QVERIFY(!history.value(QStringLiteral("canonical_chain_digest")).toString().isEmpty());
}

void PreflightVerdictTest::evidenceBundle_bindsGovernedSignOff()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString documentPath = temporary.filePath(QStringLiteral("artwork.pdf"));

    BundleFixture fixture;
    QString error;
    QVERIFY2(buildBundleFixture(documentPath, fixture, error), qPrintable(error));

    const QString publishedDigest(64, QLatin1Char('a'));
    const QString candidateDigest(64, QLatin1Char('c'));
    const QString approvalActor = QStringLiteral("operator");

    pdf::PDFApprovalRecord approval;
    approval.kind = pdf::PDFApprovalKind::Human;
    approval.actorId = approvalActor;
    approval.decision = QStringLiteral("publish");
    approval.rationale = QStringLiteral("Signed off after revalidation; sheet at %1").arg(documentPath);
    approval.decidedUtc = QDateTime::currentDateTimeUtc();

    const QJsonObject signOff{
        { QStringLiteral("schema"), QStringLiteral("loop.governed-sign-off") },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("plan_digest"), QString(64, QLatin1Char('e')) },
        { QStringLiteral("source_sha256"), sha256Hex(fixture.document) },
        { QStringLiteral("candidate_sha256"), candidateDigest },
        { QStringLiteral("published_sha256"), publishedDigest },
        { QStringLiteral("revalidation_report_sha256"), QString(64, QLatin1Char('f')) },
        { QStringLiteral("effective_profile_digest"), fixture.certificate.effectiveProfileDigest },
        { QStringLiteral("approval"), approval.toJson() }
    };

    pdf::PreflightEvidenceBundleRequest request = fixture.request;
    request.signOff = signOff;
    request.output = pdf::PreflightEvidenceBundleOutput{ publishedDigest, 4096 };

    pdf::PreflightEvidenceBundle bundle;
    QVERIFY2(pdf::buildPreflightEvidenceBundle(request, bundle, error), qPrintable(error));

    const QString bundleDirectory = temporary.filePath(QStringLiteral("signed-bundle"));
    QVERIFY2(pdf::writePreflightEvidenceBundle(bundle, bundleDirectory, error), qPrintable(error));
    const pdf::PreflightEvidenceBundleVerification verification =
        pdf::verifyPreflightEvidenceBundle(bundleDirectory, &error);
    QVERIFY2(verification.valid, qPrintable(bundleFindings(verification)));
    QCOMPARE(verification.membersChecked, 5);

    const QJsonObject manifest = bundle.manifest;
    QCOMPARE(manifest.value(QStringLiteral("sign_off")).toObject().value(QStringLiteral("published_sha256")).toString(),
             publishedDigest);
    QCOMPARE(manifest.value(QStringLiteral("sign_off")).toObject().value(QStringLiteral("effective_profile_digest")).toString(),
             fixture.certificate.effectiveProfileDigest);
    QCOMPARE(manifest.value(QStringLiteral("output")).toObject().value(QStringLiteral("sha256")).toString(),
             publishedDigest);
    QCOMPARE(manifest.value(QStringLiteral("output")).toObject().value(QStringLiteral("byte_count")).toInt(), 4096);

    // The approval travels with its actor and timestamp, and its free-form
    // rationale is path-redacted like every other member.
    const QJsonArray approvals = manifest.value(QStringLiteral("approvals")).toArray();
    QCOMPARE(approvals.size(), 1);
    const QJsonObject exportedApproval = approvals.last().toObject();
    QCOMPARE(exportedApproval.value(QStringLiteral("actor_id")).toString(), approvalActor);
    QVERIFY(!exportedApproval.value(QStringLiteral("decided_utc")).toString().isEmpty());
    QVERIFY(exportedApproval.value(QStringLiteral("rationale")).toString().contains(pdf::preflightEvidenceBundlePathPlaceholder()));

    // The sign-off record is bound to the exact accepted identities, so a source,
    // profile, or publication the manifest does not describe is refused.
    pdf::PreflightEvidenceBundle refused;
    pdf::PreflightEvidenceBundleRequest wrongSource = request;
    QJsonObject wrongSourceSignOff = signOff;
    wrongSourceSignOff.insert(QStringLiteral("source_sha256"), QString(64, QLatin1Char('9')));
    wrongSource.signOff = wrongSourceSignOff;
    QVERIFY(!pdf::buildPreflightEvidenceBundle(wrongSource, refused, error));
    QVERIFY2(error.contains(QStringLiteral("source revision")), qPrintable(error));

    pdf::PreflightEvidenceBundleRequest wrongOutput = request;
    wrongOutput.output = pdf::PreflightEvidenceBundleOutput{ candidateDigest, 4096 };
    QVERIFY(!pdf::buildPreflightEvidenceBundle(wrongOutput, refused, error));
    QVERIFY2(error.contains(QStringLiteral("sign-off published")), qPrintable(error));

    pdf::PreflightEvidenceBundleRequest wrongProfile = request;
    QJsonObject wrongProfileSignOff = signOff;
    wrongProfileSignOff.insert(QStringLiteral("effective_profile_digest"), QString(64, QLatin1Char('7')));
    wrongProfile.signOff = wrongProfileSignOff;
    QVERIFY(!pdf::buildPreflightEvidenceBundle(wrongProfile, refused, error));
    QVERIFY2(error.contains(QStringLiteral("effective profile")), qPrintable(error));

    // Tampering with the exported sign-off member is still attributable.
    QVERIFY(rewriteMember(bundleDirectory,
                          pdf::preflightEvidenceBundleSignOffMember(),
                          QByteArrayLiteral("{}")));
    const pdf::PreflightEvidenceBundleVerification tampered =
        pdf::verifyPreflightEvidenceBundle(bundleDirectory, &error);
    QVERIFY(!tampered.valid);
    QVERIFY2(hasFinding(tampered,
                        QStringLiteral("member.size-mismatch"),
                        pdf::preflightEvidenceBundleSignOffMember()),
             qPrintable(bundleFindings(tampered)));
}

void PreflightVerdictTest::evidenceBundle_detectsTamperedMembers()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString documentPath = temporary.filePath(QStringLiteral("artwork.pdf"));

    BundleFixture fixture;
    QString error;
    QVERIFY2(buildBundleFixture(documentPath, fixture, error), qPrintable(error));
    const QString pristine = temporary.filePath(QStringLiteral("pristine"));
    QVERIFY(pdf::writePreflightEvidenceBundle(fixture.bundle, pristine, error));
    QVERIFY(pdf::verifyPreflightEvidenceBundle(pristine, &error).valid);

    int caseIndex = 0;
    const auto nextCopy = [&](const QString& label) -> QString
    {
        const QString target = temporary.filePath(QStringLiteral("case-%1-%2").arg(++caseIndex).arg(label));
        return copyBundle(pristine, target) ? target : QString();
    };

    // A same-size edit of any member is a digest mismatch naming that member.
    {
        const QString directory = nextCopy(QStringLiteral("report-edit"));

        QVERIFY(!directory.isEmpty());
        QByteArray content = readMember(directory, QStringLiteral("report.json"));
        QVERIFY(!content.isEmpty());
        content[content.size() / 2] = content.at(content.size() / 2) == 'x' ? 'y' : 'x';
        QVERIFY(rewriteMember(directory, QStringLiteral("report.json"), content));
        const pdf::PreflightEvidenceBundleVerification verification =
            pdf::verifyPreflightEvidenceBundle(directory, &error);
        QVERIFY(!verification.valid);
        QVERIFY2(hasFinding(verification, QStringLiteral("member.digest-mismatch"), QStringLiteral("report.json")),
                 qPrintable(bundleFindings(verification)));
    }

    // Tampering with the exported chain is a chain break, not just a digest
    // mismatch: the finding names the member and the sequence.
    {
        const QString directory = nextCopy(QStringLiteral("history-status"));

        QVERIFY(!directory.isEmpty());
        QJsonObject history = QJsonDocument::fromJson(readMember(directory, QStringLiteral("history.json"))).object();
        QJsonArray events = history.value(QStringLiteral("events")).toArray();
        QJsonObject last = events.last().toObject();
        last.insert(QStringLiteral("status"), QStringLiteral("accepted"));
        events.replace(events.size() - 1, last);
        history.insert(QStringLiteral("events"), events);
        QVERIFY(rewriteMember(directory, QStringLiteral("history.json"),
                              QJsonDocument(history).toJson(QJsonDocument::Indented)));
        const pdf::PreflightEvidenceBundleVerification verification =
            pdf::verifyPreflightEvidenceBundle(directory, &error);
        QVERIFY(!verification.valid);
        QVERIFY2(hasFinding(verification, QStringLiteral("history.chain-broken"), QStringLiteral("history.json")),
                 qPrintable(bundleFindings(verification)));
    }

    // A missing member is attributable to the member.
    {
        const QString directory = nextCopy(QStringLiteral("missing-certificate"));

        QVERIFY(!directory.isEmpty());
        QVERIFY(QFile::remove(QDir(directory).filePath(QStringLiteral("certificate.json"))));
        const pdf::PreflightEvidenceBundleVerification verification =
            pdf::verifyPreflightEvidenceBundle(directory, &error);
        QVERIFY(!verification.valid);
        QVERIFY2(hasFinding(verification, QStringLiteral("member.missing"), QStringLiteral("certificate.json")),
                 qPrintable(bundleFindings(verification)));
    }

    // Content outside the declared set is refused by name.
    {
        const QString directory = nextCopy(QStringLiteral("undeclared"));

        QVERIFY(!directory.isEmpty());
        QVERIFY(rewriteMember(directory, QStringLiteral("notes.json"), QByteArrayLiteral("{}")));
        const pdf::PreflightEvidenceBundleVerification verification =
            pdf::verifyPreflightEvidenceBundle(directory, &error);
        QVERIFY(!verification.valid);
        QVERIFY2(hasFinding(verification, QStringLiteral("member.undeclared"), QStringLiteral("notes.json")),
                 qPrintable(bundleFindings(verification)));
    }

    // A manifest whose declared revision or coverage scope disagrees with the
    // report is refused, even though every member hash still matches.
    {
        const QString directory = nextCopy(QStringLiteral("manifest-revision"));

        QVERIFY(!directory.isEmpty());
        QJsonObject manifest =
            QJsonDocument::fromJson(readMember(directory, QStringLiteral("manifest.json"))).object();
        QJsonObject document = manifest.value(QStringLiteral("document")).toObject();
        document.insert(QStringLiteral("revision_digest"), QString(64, QLatin1Char('e')));
        manifest.insert(QStringLiteral("document"), document);
        QVERIFY(rewriteMember(directory, QStringLiteral("manifest.json"),
                              QJsonDocument(manifest).toJson(QJsonDocument::Indented)));
        const pdf::PreflightEvidenceBundleVerification verification =
            pdf::verifyPreflightEvidenceBundle(directory, &error);
        QVERIFY(!verification.valid);
        QVERIFY2(hasFinding(verification, QStringLiteral("bundle.document-digest-mismatch")),
                 qPrintable(bundleFindings(verification)));
    }
    {
        const QString directory = nextCopy(QStringLiteral("manifest-scope"));

        QVERIFY(!directory.isEmpty());
        QJsonObject manifest =
            QJsonDocument::fromJson(readMember(directory, QStringLiteral("manifest.json"))).object();
        manifest.insert(QStringLiteral("coverage_scope"),
                        QJsonObject{ { QStringLiteral("claim"), QStringLiteral("Loop claims full GWG conformance.") } });
        QVERIFY(rewriteMember(directory, QStringLiteral("manifest.json"),
                              QJsonDocument(manifest).toJson(QJsonDocument::Indented)));
        const pdf::PreflightEvidenceBundleVerification verification =
            pdf::verifyPreflightEvidenceBundle(directory, &error);
        QVERIFY(!verification.valid);
        QVERIFY2(hasFinding(verification, QStringLiteral("bundle.coverage-scope-mismatch")),
                 qPrintable(bundleFindings(verification)));
    }
}

void PreflightVerdictTest::evidenceBundle_carriesNoRawPaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString documentPath = temporary.filePath(QStringLiteral("artwork.pdf"));

    BundleFixture fixture;
    QString error;
    QVERIFY2(buildBundleFixture(documentPath, fixture, error), qPrintable(error));

    // Non-vacuity: the inputs really do carry the path.
    QCOMPARE(fixture.report.value(QStringLiteral("pdf")).toString(), documentPath);
    QVERIFY(fixture.request.rollbackPoints.first().artifactPath.startsWith(QStringLiteral("C:/jobs")));
    QVERIFY(fixture.request.rollbackPoints.first().planSummary.contains(documentPath));
    QVERIFY(fixture.report.value(QStringLiteral("warnings")).toArray().first().toObject().value(QStringLiteral("message")).toString().contains(documentPath));
    QCOMPARE(fixture.request.report.value(QStringLiteral("profile_identity")).toObject().value(QStringLiteral("source_path")).toString(),
             documentPath);

    const QString bundleDirectory = temporary.filePath(QStringLiteral("bundle"));
    QVERIFY2(pdf::writePreflightEvidenceBundle(fixture.bundle, bundleDirectory, error), qPrintable(error));
    QVERIFY(pdf::verifyPreflightEvidenceBundle(bundleDirectory, &error).valid);

    const QByteArray reportMember = readMember(bundleDirectory, pdf::preflightEvidenceBundleReportMember());
    const QByteArray manifestMember = readMember(bundleDirectory, pdf::preflightEvidenceBundleManifestMember());
    const QByteArray historyMember = readMember(bundleDirectory, pdf::preflightEvidenceBundleHistoryMember());
    const QByteArray rollbackMember = readMember(bundleDirectory, pdf::preflightEvidenceBundleRollbackMember());
    const QByteArray certificateMember = readMember(bundleDirectory, pdf::preflightEvidenceBundleCertificateMember());
    for (const QByteArray& member : { reportMember, manifestMember, historyMember, rollbackMember, certificateMember })
    {
        QVERIFY(!member.isEmpty());
    }

    const QStringList forbidden{ documentPath,
                                 QDir::toNativeSeparators(documentPath),
                                 temporary.path(),
                                 QDir::toNativeSeparators(temporary.path()),
                                 QStringLiteral("C:/jobs"),
                                 QStringLiteral("C:\\jobs"),
                                 QStringLiteral("artwork.pdf") };
    const QList<QPair<QString, QByteArray>> members{
        { pdf::preflightEvidenceBundleReportMember(), reportMember },
        { pdf::preflightEvidenceBundleManifestMember(), manifestMember },
        { pdf::preflightEvidenceBundleHistoryMember(), historyMember },
        { pdf::preflightEvidenceBundleRollbackMember(), rollbackMember },
        { pdf::preflightEvidenceBundleCertificateMember(), certificateMember }
    };
    for (const auto& member : members)
    {
        for (const QString& needle : forbidden)
        {
            QVERIFY2(!member.second.contains(needle.toUtf8()),
                     qPrintable(QStringLiteral("bundle member '%1' carries '%2'").arg(member.first, needle)));
        }
    }

    // The redaction is real: the placeholder is present where the inputs had a
    // path, and the path-bearing keys are gone rather than blanked.
    QVERIFY(reportMember.contains(pdf::preflightEvidenceBundlePathPlaceholder().toUtf8()));
    const QJsonObject report = QJsonDocument::fromJson(reportMember).object();
    QVERIFY(!containsKey(report, QStringLiteral("pdf")));
    QVERIFY(!containsKey(report, QStringLiteral("source_path")));
    QVERIFY(!containsKey(QJsonDocument::fromJson(rollbackMember).object(), QStringLiteral("artifactPath")));
    QVERIFY(!containsKey(QJsonDocument::fromJson(historyMember).object(), QStringLiteral("storageToken")));

    // The rollback reference keeps the content digest the path named.
    const QJsonObject rollback =
        QJsonDocument::fromJson(rollbackMember).object().value(QStringLiteral("references")).toArray().first().toObject();
    QCOMPARE(rollback.value(QStringLiteral("artifact_sha256")).toString(), QString(64, QLatin1Char('d')));
    QVERIFY(rollback.value(QStringLiteral("plan_summary")).toString().contains(pdf::preflightEvidenceBundlePathPlaceholder()));
}

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
    result.checkStatuses.append(pdf::PreflightCheckStatus{ QStringLiteral("ink-coverage"),
                                                           QStringLiteral("incomplete"),
                                                           QStringLiteral("budget-exceeded"),
                                                           QJsonObject{},
                                                           QStringList{},
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
    result.checkStatuses.append(pdf::PreflightCheckStatus{ QStringLiteral("image-resolution"),
                                                           QStringLiteral("incomplete"),
                                                           QStringLiteral("cancelled"),
                                                           QJsonObject{},
                                                           QStringList{},
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

void PreflightVerdictTest::receiptIdentity_matchesGoldenVector()
{
    ReceiptFixture fixture;
    pdf::PDFEvidenceRecord record;
    record.id = QStringLiteral("evidence-2");
    record.fidelity = QStringLiteral("sampled");
    fixture.evidence.records.append(record);
    pdf::PreflightFinding warning;
    warning.evidenceIds.append(QStringLiteral("evidence-1"));
    fixture.result.warnings.append(warning);

    pdf::PreflightInspectionReceipt receipt;
    QString error;
    QVERIFY2(pdf::buildPreflightInspectionReceipt(fixture.result, fixture.profile, fixture.revision,
                                                  fixture.evidence, receipt, error),
             qPrintable(error));
    QCOMPARE(receipt.identity, QStringLiteral("8e94a7fefac162eafa4a20516692fb281c28978788b9d91d4df3397061933a8b"));
    QCOMPARE(receipt.verdict.state, pdf::PreflightVerdictState::Pass);
    QCOMPARE(receipt.checks.size(), 1);
    QVERIFY(receipt.checks.first().complete);
    QCOMPARE(receipt.evidenceRefs, (QStringList{ QStringLiteral("evidence-1"), QStringLiteral("evidence-2") }));
    QCOMPARE(receipt.fidelity, QStringLiteral("sampled"));
    QCOMPARE(receipt.limitations, QStringList{ QStringLiteral("Limited to enabled checks.") });

    fixture.revision.documentRevision = 2;
    pdf::PreflightInspectionReceipt later;
    QVERIFY2(pdf::buildPreflightInspectionReceipt(fixture.result, fixture.profile, fixture.revision,
                                                  fixture.evidence, later, error),
             qPrintable(error));
    QCOMPARE(later.identity, receipt.identity);
    QCOMPARE(later.revision.documentRevision, pdf::DocumentRevision(2));

    fixture.profile.effectiveDigest = QString(64, QLatin1Char('c'));
    fixture.result.effectiveProfileDigest = fixture.profile.effectiveDigest;
    pdf::PreflightInspectionReceipt changedPolicy;
    QVERIFY2(pdf::buildPreflightInspectionReceipt(fixture.result, fixture.profile, fixture.revision,
                                                  fixture.evidence, changedPolicy, error),
             qPrintable(error));
    QCOMPARE(changedPolicy.identity, QStringLiteral("931952b4c72f56e67fa371c94eb01bec4383cab251286a3c675c7d8dcada11f8"));
}

void PreflightVerdictTest::receiptTerminalStates_data()
{
    QTest::addColumn<QString>("caseName");
    QTest::addColumn<pdf::PreflightVerdictState>("expected");
    QTest::newRow("pass") << QStringLiteral("pass") << pdf::PreflightVerdictState::Pass;
    QTest::newRow("fail") << QStringLiteral("fail") << pdf::PreflightVerdictState::Fail;
    QTest::newRow("missing-required") << QStringLiteral("missing-required") << pdf::PreflightVerdictState::Incomplete;
    QTest::newRow("unsupported") << QStringLiteral("unsupported") << pdf::PreflightVerdictState::Incomplete;
    QTest::newRow("budget-limited") << QStringLiteral("budget-limited") << pdf::PreflightVerdictState::Incomplete;
    QTest::newRow("cancelled") << QStringLiteral("cancelled") << pdf::PreflightVerdictState::Incomplete;
    QTest::newRow("parser-error") << QStringLiteral("parser-error") << pdf::PreflightVerdictState::Error;
}

void PreflightVerdictTest::receiptTerminalStates()
{
    QFETCH(QString, caseName);
    QFETCH(pdf::PreflightVerdictState, expected);
    ReceiptFixture fixture;
    if (caseName == QLatin1String("fail"))
    {
        fixture.result.errors.append(blockingFinding());
    }
    else if (caseName == QLatin1String("missing-required"))
    {
        fixture.result.checkStatuses.clear();
    }
    else if (caseName == QLatin1String("unsupported"))
    {
        fixture.result.checkStatuses.first().status = QStringLiteral("unsupported");
    }
    else if (caseName == QLatin1String("budget-limited"))
    {
        fixture.result.checkStatuses.first().budgetKind = QStringLiteral("raster-pixels");
    }
    else if (caseName == QLatin1String("cancelled"))
    {
        fixture.result.errorCode = QStringLiteral("cancelled");
    }
    else if (caseName == QLatin1String("parser-error"))
    {
        fixture.result.errorCode = QStringLiteral("parser-error");
    }

    pdf::PreflightInspectionReceipt receipt;
    QString error;
    QVERIFY2(pdf::buildPreflightInspectionReceipt(fixture.result, fixture.profile, fixture.revision,
                                                  fixture.evidence, receipt, error),
             qPrintable(error));
    QCOMPARE(receipt.verdict.state, expected);
    QCOMPARE(receipt.verdict.isPass(), expected == pdf::PreflightVerdictState::Pass);
    if (expected == pdf::PreflightVerdictState::Incomplete)
    {
        QVERIFY(!receipt.verdict.allowsCertificateIssuance());
    }
}

void PreflightVerdictTest::receiptRejectsMismatchedProvenance()
{
    ReceiptFixture fixture;
    fixture.evidence.artifact.sha256 = QString(64, QLatin1Char('c'));
    pdf::PreflightInspectionReceipt receipt;
    QString error;
    QVERIFY(!pdf::buildPreflightInspectionReceipt(fixture.result, fixture.profile, fixture.revision,
                                                  fixture.evidence, receipt, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(receipt.identity.isEmpty());

    fixture.evidence.artifact.sha256.clear();
    fixture.evidence.revision = fixture.revision;
    fixture.evidence.revision.documentRevision = 2;
    QVERIFY(!pdf::buildPreflightInspectionReceipt(fixture.result, fixture.profile, fixture.revision,
                                                  fixture.evidence, receipt, error));
    QVERIFY(receipt.identity.isEmpty());
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
    warningResult.checkStatuses.append(makeCheckStatus(QStringLiteral("fonts"), QStringLiteral("warning")));
    QVERIFY(pdf::preflightAllowsCertification(warningResult));

    pdf::PreflightResult waivedResult;
    waivedResult.inspectionComplete = true;
    waivedResult.documentRevisionDigest = documentDigest;
    waivedResult.effectiveProfileDigest = profileDigest;
    waivedResult.profileIdentity.insert(QStringLiteral("provisional"), false);
    waivedResult.errors.append(blockingFinding());
    waivedResult.checkStatuses.append(makeCheckStatus(QStringLiteral("color-mode"), QStringLiteral("failed")));

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
    skipped.checkStatuses.append(makeCheckStatus(QStringLiteral("ink-coverage"),
                                                 QStringLiteral("skipped"),
                                                 QStringLiteral("inspection incomplete")));
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
    result.checkStatuses.append(makeCheckStatus(QStringLiteral("bleed"), QStringLiteral("ok")));

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

void PreflightVerdictTest::auditRun_preservesEffectiveRestrictions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString documentPath = directory.filePath(QStringLiteral("restricted-audit.pdf"));
    const QByteArray document("%PDF-1.4\n%%EOF\n");
    const QString documentDigest =
        QString::fromLatin1(QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex());
    const QString profileDigest(64, QLatin1Char('e'));

    pdf::PreflightResult result;
    result.inspectionComplete = true;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = profileDigest;
    result.profileIdentity.insert(QStringLiteral("provisional"), false);
    result.profileIdentity.insert(QStringLiteral("effective_digest"), profileDigest);
    result.coverageScope.insert(QStringLiteral("scope_restrictions"),
                                QJsonObject{ { QStringLiteral("pages"), QJsonArray{ 2 } },
                                             { QStringLiteral("object_classes"), QJsonArray{ QStringLiteral("image") } } });
    result.checkStatuses.append(makeCheckStatus(QStringLiteral("image-resolution"), QStringLiteral("ok")));

    const QJsonObject summary = pdf::preflightAuditReportSummary(result, documentPath);
    QCOMPARE(summary.value(QStringLiteral("effective_profile_digest")).toString(), profileDigest);
    QCOMPARE(summary.value(QStringLiteral("coverage_scope")).toObject().value(QStringLiteral("scope_restrictions")).toObject(),
             result.coverageScope.value(QStringLiteral("scope_restrictions")).toObject());

    const pdf::PDFOperationResult appended =
        pdf::appendPreflightAuditRun(documentPath,
                                     document,
                                     result,
                                     pdf::PDFOperationHistoryStatus::Accepted,
                                     QStringLiteral("test"),
                                     summary);
    QVERIFY2(appended, qPrintable(appended.getErrorMessage()));

    pdf::PDFOperationHistoryStore history(
        QDir(QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history"))
            .filePath(QStringLiteral("history.sqlite3")));
    QString error;
    QVERIFY2(history.open(&error), qPrintable(error));
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const pdf::PDFOperationHistoryEvent& finished = events.back();
    QCOMPARE(finished.effectiveProfileDigest, profileDigest);
    QCOMPARE(finished.resultSummary.value(QStringLiteral("coverage_scope")).toObject().value(QStringLiteral("scope_restrictions")).toObject(),
             result.coverageScope.value(QStringLiteral("scope_restrictions")).toObject());
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
    result.checkStatuses.append(makeCheckStatus(QStringLiteral("bleed"), QStringLiteral("ok")));

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

void PreflightVerdictTest::certificate_bindsRestrictionDigest()
{
    const QByteArray document("restricted certified revision");
    const QString documentDigest = QString::fromLatin1(QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex());
    const QString restrictedDigest(64, QLatin1Char('f'));
    const QString unrestrictedDigest(64, QLatin1Char('a'));

    pdf::PreflightResult result;
    result.inspectionComplete = true;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = restrictedDigest;
    result.profileIdentity.insert(QStringLiteral("provisional"), false);
    result.coverageScope.insert(QStringLiteral("scope_restrictions"),
                                QJsonObject{ { QStringLiteral("pages"), QJsonArray{ 1 } } });
    result.checkStatuses.append(makeCheckStatus(QStringLiteral("image-resolution"), QStringLiteral("ok")));

    const QJsonObject summary = pdf::preflightAuditReportSummary(result, QStringLiteral("document.pdf"));

    pdf::PDFOperationHistoryEvent preflight;
    preflight.sequence = 1;
    preflight.entryId = QUuid::createUuid();
    preflight.executionId = QUuid::createUuid();
    preflight.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
    preflight.status = pdf::PDFOperationHistoryStatus::Accepted;
    preflight.documentRevisionDigest = documentDigest;
    preflight.effectiveProfileDigest = restrictedDigest;
    preflight.resultSummary = summary;
    preflight.createdUtc = QDateTime::currentDateTimeUtc();
    preflight.eventHash = pdf::computeOperationHistoryEventHash(preflight, {});

    pdf::PreflightCertificate certificate;
    QString error;
    QVERIFY(pdf::issuePreflightCertificate(result,
                                           summary,
                                           document,
                                           { preflight },
                                           QStringLiteral("operator"),
                                           certificate,
                                           error));
    QCOMPARE(certificate.effectiveProfileDigest, restrictedDigest);

    pdf::PreflightResult mismatched = result;
    mismatched.effectiveProfileDigest = unrestrictedDigest;
    pdf::PreflightCertificate refused;
    QVERIFY(!pdf::issuePreflightCertificate(mismatched,
                                            summary,
                                            document,
                                            { preflight },
                                            QStringLiteral("operator"),
                                            refused,
                                            error));
    QVERIFY(!error.isEmpty());
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
    result.checkStatuses.append(makeCheckStatus(QStringLiteral("color-mode"), QStringLiteral("failed")));

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
