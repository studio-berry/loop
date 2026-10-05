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

#include "pdfgovernedexecution.h"

#include "pdfartifactidentity.h"
#include "pdfdocumentreader.h"
#include "pdfoperationhistorystore.h"
#include "preflightprofileresolver.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

namespace pdf
{

namespace
{

QJsonArray plansToJson(const QList<PDFRepairPlan>& plans)
{
    QJsonArray result;
    for (const PDFRepairPlan& plan : plans)
    {
        result.append(plan.toJson());
    }
    return result;
}

QString digestHex(const QByteArray& canonical)
{
    return QString::fromLatin1(QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

bool sha256Matches(const QString& actual, const QString& expected)
{
    return actual.trimmed().compare(expected.trimmed(), Qt::CaseInsensitive) == 0;
}

bool approvalAuthorizesPublication(const PDFApprovalRecord& approval)
{
    if (approval.kind == PDFApprovalKind::None)
    {
        return false;
    }
    return approval.decision.trimmed().compare(QStringLiteral("approve"), Qt::CaseInsensitive) == 0;
}

QString digestJson(const QJsonObject& object)
{
    return digestHex(canonicalJson(object));
}

/// Fails closed unless the supplied digest is a well-formed SHA-256 that equals
/// the plan digest recomputed from the transaction's own plans, source bytes,
/// and merged save policy. This binds a preview to one exact analyzed plan.
PDFOperationResult validatePreviewPlanDigest(const PDFRepairTransaction& transaction,
                                             const QString& planDigest)
{
    if (!isPDFSha256(planDigest))
    {
        return PDFOperationResult(QStringLiteral("Preview plan digest is missing or malformed."));
    }
    const QString expected = computeOperationPlanDigest(transaction.plans(),
                                                        transaction.sourceSha256(),
                                                        transaction.savePolicy());
    if (!sha256Matches(planDigest, expected))
    {
        return PDFOperationResult(QStringLiteral("Preview plan digest does not match the analyzed operation plan."));
    }
    return PDFOperationResult(true);
}

/// Removes the artifacts a preview call created: the candidate file, the render
/// PNGs it rendered, and the candidate parent directory when this call created
/// it and it is now empty. A caller-named path is removed because the artifact
/// was never approved or published (the residue contract).
void removePreviewResidue(const QString& candidatePath,
                          bool candidateParentExisted,
                          const QString& renderDirectory,
                          const QVector<PDFRepairPageVisualDiff>& pages)
{
    if (!candidatePath.isEmpty())
    {
        QFile::remove(candidatePath);
    }
    if (!renderDirectory.isEmpty())
    {
        const QDir renderDir(renderDirectory);
        for (const PDFRepairPageVisualDiff& page : pages)
        {
            for (const QString& name : { page.beforeImagePath, page.afterImagePath, page.diffImagePath })
            {
                if (!name.isEmpty())
                {
                    QFile::remove(renderDir.filePath(name));
                }
            }
        }
    }
    if (candidateParentExisted || candidatePath.isEmpty())
    {
        return;
    }
    const QString parentPath = QFileInfo(candidatePath).absolutePath();
    const QDir parent(parentPath);
    if (parent.exists() && parent.isEmpty())
    {
        QDir().rmdir(parentPath);
    }
}

/// A preview did not complete: it is either a hard failure or an incomplete
/// report. Only a complete preview leaves its review artifact behind.
bool previewDidNotComplete(const PDFOperationResult& result, PDFRepairDiffStatus status)
{
    return !result || status == PDFRepairDiffStatus::Incomplete || status == PDFRepairDiffStatus::Failed;
}

/// Maps a reduced preflight verdict onto the explicit revalidation state.
/// Only a passing verdict is `complete`; a verdict that could not finish is
/// `incomplete`; a definite failure or engine error is `error`.
QString governedRevalidationStateForVerdict(const PreflightVerdict& verdict)
{
    switch (verdict.state)
    {
        case PreflightVerdictState::Pass:
            return QStringLiteral("complete");
        case PreflightVerdictState::Incomplete:
            return QStringLiteral("incomplete");
        case PreflightVerdictState::Fail:
        case PreflightVerdictState::Error:
            break;
    }
    return QStringLiteral("error");
}

/// A full-profile plan for the parsed profile, carrying every enabled check and
/// the fail-closed reason the targeted scope was rejected.
PDFRevalidationPlan fullGovernedRevalidationPlan(const PreflightProfileData& profile, const QString& reason)
{
    PDFRevalidationPlan plan;
    plan.full = true;
    plan.invalidatedDomains = pdfEvidenceAllDomains();
    plan.invalidatedEvidenceDomains = pdfEvidenceAllDomains();
    plan.reason = reason;
    for (const PreflightCheckConfig& check : profile.checks)
    {
        if (check.enabled)
        {
            plan.checkIds.append(check.id);
        }
    }
    return plan;
}

/// Resolves a governed profile exactly as `PreflightEngine::run(QJsonObject)`
/// does — import validation, variable binding, parse, and the effective digest —
/// so the revalidation's profile identity matches a plain engine run. Returns
/// false and fills the fail-closed code/message on refusal.
bool resolveGovernedProfileData(const QJsonObject& profile,
                                PreflightProfileData* profileData,
                                QString* errorCode,
                                QString* errorMessage)
{
    const PreflightProfileImportResult imported = importPreflightProfile(profile);
    if (!imported.ok)
    {
        *errorCode = imported.errorCode.isEmpty() ? QStringLiteral("profile-invalid") : imported.errorCode;
        *errorMessage = imported.errorMessage;
        return false;
    }
    const PreflightVariableBindResult bound = bindPreflightProfileVariables(imported.profile);
    if (!bound.ok)
    {
        *errorCode = bound.errorCode.isEmpty() ? QStringLiteral("profile-invalid") : bound.errorCode;
        *errorMessage = bound.errorMessage;
        return false;
    }
    if (!PreflightEngine::parseProfile(bound.profile, *profileData, *errorMessage))
    {
        *errorCode = QStringLiteral("profile-invalid");
        return false;
    }
    profileData->variableBindings = bound.bindings;
    profileData->fileDigest = imported.identity.digest;
    profileData->effectiveDigest = computeProfileDigest(bound.profile);
    profileData->provisional = imported.identity.provisional;
    profileData->profileIdentity = imported.identity.toJson();
    profileData->profileIdentity.insert(QStringLiteral("digest"), profileData->fileDigest);
    profileData->profileIdentity.insert(QStringLiteral("effective_digest"), profileData->effectiveDigest);
    return true;
}

}   // namespace

QString computeOperationPlanDigest(const QList<PDFRepairPlan>& plans,
                                   const QString& sourceSha256,
                                   const PDFOperationSavePolicy& savePolicy)
{
    const QJsonObject envelope{
        { QStringLiteral("schema_kind"), QStringLiteral("operation-plan") },
        { QStringLiteral("schema_version"), QStringLiteral("1.0") },
        { QStringLiteral("source_sha256"), sourceSha256.trimmed().toLower() },
        { QStringLiteral("save_policy"), savePolicy.toJson() },
        { QStringLiteral("registry_digest"), PDFRepairRegistry::instance().digest() },
        { QStringLiteral("plans"), plansToJson(plans) }
    };
    return digestHex(canonicalJson(envelope));
}

QJsonObject PDFTechnicalPreview::toJson() const
{
    QJsonArray changesJson;
    for (const PDFRepairStructuralChange& change : structuralChanges)
    {
        changesJson.append(QJsonObject{
            { QStringLiteral("path"), change.path },
            { QStringLiteral("kind"), change.kind },
            { QStringLiteral("before"), change.beforeValue },
            { QStringLiteral("after"), change.afterValue },
            { QStringLiteral("classification"), pdfRepairChangeClassName(change.classification) } });
    }

    return QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop.technical-preview") },
        { QStringLiteral("schema_version"), schemaVersion },
        { QStringLiteral("plan_digest"), planDigest },
        { QStringLiteral("source_sha256"), sourceSha256 },
        { QStringLiteral("candidate_sha256"), candidateSha256 },
        { QStringLiteral("fidelity_mode"), pdfRepairPreviewFidelityName(fidelity) },
        { QStringLiteral("status"), pdfRepairDiffStatusName(status) },
        { QStringLiteral("structural_changes"), changesJson },
        { QStringLiteral("warnings"), QJsonArray::fromStringList(warnings) },
        { QStringLiteral("incomplete_reasons"), QJsonArray::fromStringList(incompleteReasons) }
    };
}

QJsonObject PDFVisualPreview::toJson() const
{
    QJsonArray pagesJson;
    for (const PDFRepairPageVisualDiff& page : pages)
    {
        pagesJson.append(QJsonObject{
            { QStringLiteral("page_index"), page.pageIndex },
            { QStringLiteral("pixel_size"), QJsonObject{
                                                { QStringLiteral("width"), page.pixelSize.width() },
                                                { QStringLiteral("height"), page.pixelSize.height() } } },
            { QStringLiteral("changed_pixel_count"), static_cast<qint64>(page.changedPixelCount) },
            { QStringLiteral("unexpected_changed_pixel_count"), static_cast<qint64>(page.unexpectedChangedPixelCount) },
            { QStringLiteral("changed_pixel_ratio"), page.changedPixelRatio },
            { QStringLiteral("artifacts"), QJsonObject{ { QStringLiteral("before"), page.beforeImagePath }, { QStringLiteral("after"), page.afterImagePath }, { QStringLiteral("diff"), page.diffImagePath } } },
            { QStringLiteral("warnings"), QJsonArray::fromStringList(page.warnings) } });
    }

    return QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop.visual-preview") },
        { QStringLiteral("schema_version"), schemaVersion },
        { QStringLiteral("plan_digest"), planDigest },
        { QStringLiteral("source_sha256"), sourceSha256 },
        { QStringLiteral("candidate_sha256"), candidateSha256 },
        { QStringLiteral("fidelity_mode"), pdfRepairPreviewFidelityName(fidelity) },
        { QStringLiteral("status"), pdfRepairDiffStatusName(status) },
        { QStringLiteral("pages"), pagesJson },
        { QStringLiteral("warnings"), QJsonArray::fromStringList(warnings) },
        { QStringLiteral("incomplete_reasons"), QJsonArray::fromStringList(incompleteReasons) }
    };
}

bool PDFGovernedExecutionApproval::isValid() const
{
    return isPDFSha256(planDigest) && isPDFSha256(sourceSha256) && isPDFSha256(candidateSha256) && approval.isValid();
}

QJsonObject PDFGovernedExecutionApproval::toJson() const
{
    return QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop.governed-approval") },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("plan_digest"), planDigest },
        { QStringLiteral("source_sha256"), sourceSha256 },
        { QStringLiteral("candidate_sha256"), candidateSha256 },
        { QStringLiteral("effective_profile_digest"), effectiveProfileDigest },
        { QStringLiteral("approval"), approval.toJson() }
    };
}

PDFGovernedExecutionApproval PDFGovernedExecutionApproval::fromJson(const QJsonObject& object, QString* error)
{
    PDFGovernedExecutionApproval approval;
    approval.planDigest = object.value(QStringLiteral("plan_digest")).toString().toLower();
    approval.sourceSha256 = object.value(QStringLiteral("source_sha256")).toString().toLower();
    approval.candidateSha256 = object.value(QStringLiteral("candidate_sha256")).toString().toLower();
    approval.effectiveProfileDigest = object.value(QStringLiteral("effective_profile_digest")).toString().toLower();
    approval.approval = PDFApprovalRecord::fromJson(object.value(QStringLiteral("approval")).toObject());
    if (!approval.isValid())
    {
        if (error)
        {
            *error = QStringLiteral("Governed approval is missing plan/source/candidate digests or a valid approval record.");
        }
    }
    return approval;
}

QJsonObject PDFGovernedRevalidationScope::toJson() const
{
    QJsonObject object{
        { QStringLiteral("plan"), plan.toJson() },
        { QStringLiteral("impact"), impact.toJson() },
        { QStringLiteral("has_baseline"), hasBaseline }
    };
    if (hasBaseline)
    {
        object.insert(QStringLiteral("baseline_report"), baseline.toJson());
    }
    return object;
}

bool PDFGovernedExecutionRevalidation::isSignOffEligible() const
{
    return state == QStringLiteral("complete") && bytesVerified && isPDFSha256(artifactSha256) &&
           isPDFSha256(reportSha256) && isPDFSha256(effectiveProfileDigest) && verdict.isPass();
}

QJsonObject PDFGovernedExecutionRevalidation::toJson() const
{
    return QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop.governed-revalidation") },
        { QStringLiteral("schema_version"), schemaVersion },
        { QStringLiteral("state"), state },
        { QStringLiteral("reason_code"), reasonCode },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("bytes_verified"), bytesVerified },
        { QStringLiteral("artifact_sha256"), artifactSha256 },
        { QStringLiteral("report_sha256"), reportSha256 },
        { QStringLiteral("effective_profile_digest"), effectiveProfileDigest },
        { QStringLiteral("verdict"), verdict.toJson() },
        { QStringLiteral("sign_off_eligible"), isSignOffEligible() },
        { QStringLiteral("report"), report }
    };
}

bool PDFGovernedExecutionSignOff::isValid() const
{
    return isPDFSha256(planDigest) && isPDFSha256(sourceSha256) && isPDFSha256(candidateSha256) &&
           isPDFSha256(publishedSha256) && isPDFSha256(revalidationReportSha256) &&
           isPDFSha256(effectiveProfileDigest) && approval.isValid();
}

QJsonObject PDFGovernedExecutionSignOff::toJson() const
{
    return QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop.governed-sign-off") },
        { QStringLiteral("schema_version"), schemaVersion },
        { QStringLiteral("plan_digest"), planDigest },
        { QStringLiteral("source_sha256"), sourceSha256 },
        { QStringLiteral("candidate_sha256"), candidateSha256 },
        { QStringLiteral("published_sha256"), publishedSha256 },
        { QStringLiteral("revalidation_report_sha256"), revalidationReportSha256 },
        { QStringLiteral("effective_profile_digest"), effectiveProfileDigest },
        { QStringLiteral("approval"), approval.toJson() }
    };
}

PDFGovernedExecutionSignOff PDFGovernedExecutionSignOff::fromJson(const QJsonObject& object, QString* error)
{
    PDFGovernedExecutionSignOff signOff;
    signOff.schemaVersion = object.value(QStringLiteral("schema_version")).toInt(1);
    signOff.planDigest = object.value(QStringLiteral("plan_digest")).toString().toLower();
    signOff.sourceSha256 = object.value(QStringLiteral("source_sha256")).toString().toLower();
    signOff.candidateSha256 = object.value(QStringLiteral("candidate_sha256")).toString().toLower();
    signOff.publishedSha256 = object.value(QStringLiteral("published_sha256")).toString().toLower();
    signOff.revalidationReportSha256 = object.value(QStringLiteral("revalidation_report_sha256")).toString().toLower();
    signOff.effectiveProfileDigest = object.value(QStringLiteral("effective_profile_digest")).toString().toLower();
    signOff.approval = PDFApprovalRecord::fromJson(object.value(QStringLiteral("approval")).toObject());
    if (!signOff.isValid() && error)
    {
        *error = QStringLiteral("Governed sign-off is missing a plan/source/candidate/published/report/profile digest or a valid approval record.");
    }
    return signOff;
}

PDFOperationResult verifyGovernedSignOffAgainstArtifact(const PDFGovernedExecutionSignOff& signOff,
                                                        const QString& publishedPath)
{
    if (!signOff.isValid())
    {
        return PDFOperationResult(QStringLiteral("The governed sign-off is incomplete or invalid."));
    }
    QFile publishedFile(publishedPath);
    if (!publishedFile.open(QIODevice::ReadOnly))
    {
        return PDFOperationResult(QStringLiteral("artifact-unreadable: could not open the signed-off artifact for verification."));
    }
    const QByteArray bytes = publishedFile.readAll();
    if (publishedFile.error() != QFileDevice::NoError)
    {
        return PDFOperationResult(QStringLiteral("artifact-unreadable: could not read the signed-off artifact for verification."));
    }
    const QString actual = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (!sha256Matches(actual, signOff.publishedSha256))
    {
        return PDFOperationResult(QStringLiteral("invalid-document-changed: the artifact no longer matches the bytes the sign-off published (expected %1, actual %2).")
                                      .arg(signOff.publishedSha256.trimmed().toLower(), actual));
    }
    return PDFOperationResult(true);
}

bool preflightDecisionQualifiesAsOperationApproval(const PreflightDecision& decision)
{
    Q_UNUSED(decision);
    return false;
}

PDFOperationResult buildTechnicalPreview(PDFRepairTransaction& transaction,
                                         const QString& candidatePath,
                                         const QString& planDigest,
                                         PDFTechnicalPreview* preview)
{
    if (!preview)
    {
        return PDFOperationResult(QStringLiteral("Technical preview output is null."));
    }
    if (const PDFOperationResult binding = validatePreviewPlanDigest(transaction, planDigest); !binding)
    {
        return binding;
    }

    const bool candidateParentExisted = QFileInfo::exists(QFileInfo(candidatePath).absolutePath());

    PDFRepairDiffOptions options;
    options.renderVisualDiff = false;
    options.fidelity = PDFRepairPreviewFidelity::Exact;
    options.operationControl = transaction.operationControl();
    PDFRepairDiffReport report;
    const PDFOperationResult compareResult = transaction.compareCandidate(candidatePath, options, &report);
    if (previewDidNotComplete(compareResult, report.status))
    {
        removePreviewResidue(candidatePath, candidateParentExisted, options.renderDirectory, report.pages);
    }
    if (!compareResult)
    {
        return compareResult;
    }

    *preview = PDFTechnicalPreview();
    preview->fidelity = PDFRepairPreviewFidelity::Exact;
    preview->planDigest = planDigest;
    preview->sourceSha256 = report.sourceFingerprint;
    preview->candidateSha256 = report.candidateFingerprint;
    preview->status = report.status;
    preview->structuralChanges = report.structuralChanges;
    preview->warnings = report.warnings;
    preview->incompleteReasons = report.incompleteReasons;
    return PDFOperationResult(true);
}

PDFOperationResult buildVisualPreview(PDFRepairTransaction& transaction,
                                      const QString& candidatePath,
                                      const QString& planDigest,
                                      const PDFRepairDiffOptions& options,
                                      PDFVisualPreview* preview)
{
    if (!preview)
    {
        return PDFOperationResult(QStringLiteral("Visual preview output is null."));
    }
    if (const PDFOperationResult binding = validatePreviewPlanDigest(transaction, planDigest); !binding)
    {
        return binding;
    }

    const bool candidateParentExisted = QFileInfo::exists(QFileInfo(candidatePath).absolutePath());

    PDFRepairDiffOptions visualOptions = options;
    visualOptions.renderVisualDiff = true;
    visualOptions.compareMetadata = false;
    visualOptions.compareResources = false;
    visualOptions.compareAnnotations = false;
    visualOptions.fidelity = PDFRepairPreviewFidelity::Simulated;
    if (!visualOptions.operationControl)
    {
        visualOptions.operationControl = transaction.operationControl();
    }

    PDFRepairDiffReport report;
    const PDFOperationResult compareResult = transaction.compareCandidate(candidatePath, visualOptions, &report);
    if (previewDidNotComplete(compareResult, report.status))
    {
        removePreviewResidue(candidatePath, candidateParentExisted, visualOptions.renderDirectory, report.pages);
    }
    if (!compareResult)
    {
        return compareResult;
    }

    *preview = PDFVisualPreview();
    preview->fidelity = PDFRepairPreviewFidelity::Simulated;
    preview->planDigest = planDigest;
    preview->sourceSha256 = report.sourceFingerprint;
    preview->candidateSha256 = report.candidateFingerprint;
    preview->status = report.status;
    preview->pages = report.pages;
    preview->warnings = report.warnings;
    preview->incompleteReasons = report.incompleteReasons;
    return PDFOperationResult(true);
}

PDFApprovalAuthorization resolveApprovalAuthorization(const PDFGovernedExecutionApproval& approval,
                                                      const PDFApprovalAuthorizationContext& context)
{
    const auto refuse = [](const QString& code, const QString& reason)
    {
        PDFApprovalAuthorization authorization;
        authorization.allowed = false;
        authorization.code = code;
        authorization.reason = reason;
        return authorization;
    };

    const PDFApprovalRecord& record = approval.approval;
    if (!approvalAuthorizesPublication(record))
    {
        return refuse(QStringLiteral("approval-unauthorized"),
                      QStringLiteral("The approval does not carry an affirmative non-None decision."));
    }
    if (record.actorId.trimmed().isEmpty())
    {
        return refuse(QStringLiteral("approval-unauthorized"),
                      QStringLiteral("The approval does not name an approver."));
    }
    if (!context.policy.authorizedKinds.isEmpty() && !context.policy.authorizedKinds.contains(record.kind))
    {
        return refuse(QStringLiteral("approval-unauthorized"),
                      QStringLiteral("The approval kind is not authorized by the declared policy."));
    }
    if (!context.policy.authorizedActorIds.isEmpty() &&
        !context.policy.authorizedActorIds.contains(record.actorId.trimmed(), Qt::CaseInsensitive))
    {
        return refuse(QStringLiteral("approval-unauthorized"),
                      QStringLiteral("The approver is not authorized by the declared policy."));
    }

    if (context.policy.requireExpiry && !record.expiresUtc.isValid())
    {
        return refuse(QStringLiteral("approval-expired"),
                      QStringLiteral("The declared policy requires an expiry and the approval declares none."));
    }
    if (record.expiresUtc.isValid())
    {
        if (!context.evaluatedUtc.isValid())
        {
            return refuse(QStringLiteral("approval-expired"),
                          QStringLiteral("The approval declares an expiry that cannot be evaluated."));
        }
        if (record.isExpiredAt(context.evaluatedUtc))
        {
            return refuse(QStringLiteral("approval-expired"),
                          QStringLiteral("The approval expired at or before the evaluation time."));
        }
    }

    if (context.history && !record.decisionReference.trimmed().isEmpty())
    {
        QString historyError;
        const QList<PDFOperationHistoryEvent> events = context.history->events(&historyError);
        for (const PDFOperationHistoryEvent& event : events)
        {
            if (event.kind == PDFOperationHistoryEventKind::ApprovalRevoked &&
                event.approval.decisionReference.compare(record.decisionReference, Qt::CaseInsensitive) == 0)
            {
                return refuse(QStringLiteral("approval-revoked"),
                              QStringLiteral("A later ApprovalRevoked event revoked this approval."));
            }
        }
    }

    if (!context.expectedProfileDigest.trimmed().isEmpty() &&
        approval.effectiveProfileDigest.trimmed().compare(context.expectedProfileDigest.trimmed(), Qt::CaseInsensitive) != 0)
    {
        return refuse(QStringLiteral("profile-binding"),
                      QStringLiteral("The approval is bound to a different effective profile."));
    }

    PDFApprovalAuthorization authorization;
    authorization.allowed = true;
    return authorization;
}

PDFOperationResult validateGovernedApproval(const PDFGovernedExecutionApproval& approval,
                                            const QString& expectedPlanDigest,
                                            const QString& expectedSourceSha256,
                                            const QString& expectedCandidateSha256,
                                            const PDFApprovalAuthorizationContext& context)
{
    if (!approval.isValid())
    {
        return PDFOperationResult(QStringLiteral("Governed approval is incomplete or invalid."));
    }
    if (!sha256Matches(approval.planDigest, expectedPlanDigest))
    {
        return PDFOperationResult(QStringLiteral("Governed approval does not match the analyzed operation plan."));
    }
    if (!sha256Matches(approval.sourceSha256, expectedSourceSha256))
    {
        return PDFOperationResult(QStringLiteral("Governed approval does not match the trusted source artifact."));
    }
    if (!sha256Matches(approval.candidateSha256, expectedCandidateSha256))
    {
        return PDFOperationResult(QStringLiteral("Governed approval does not match the reviewed candidate artifact."));
    }
    if (approval.approval.decisionReference.startsWith(QStringLiteral("preflight-decision:"), Qt::CaseInsensitive))
    {
        return PDFOperationResult(QStringLiteral("Preflight finding decisions are not operation approval."));
    }
    if (!approvalAuthorizesPublication(approval.approval))
    {
        return PDFOperationResult(QStringLiteral("Governed approval requires an affirmative non-None approval decision."));
    }

    const PDFApprovalAuthorization authorization = resolveApprovalAuthorization(approval, context);
    if (!authorization.allowed)
    {
        return PDFOperationResult(QStringLiteral("Governed approval is not authorized (%1): %2")
                                      .arg(authorization.code, authorization.reason));
    }
    return PDFOperationResult(true);
}

PDFOperationResult revalidateGovernedArtifact(const QString& publishedPath,
                                              const QJsonObject& profile,
                                              const QString& expectedSha256,
                                              PDFGovernedExecutionRevalidation* revalidation,
                                              const PDFGovernedRevalidationScope& scope)
{
    if (!revalidation)
    {
        return PDFOperationResult(QStringLiteral("Governed revalidation output is null."));
    }
    *revalidation = PDFGovernedExecutionRevalidation();

    const auto refuse = [revalidation](const QString& code, const QString& message)
    {
        revalidation->state = QStringLiteral("error");
        revalidation->reasonCode = code;
        revalidation->reason = message;
        return PDFOperationResult(message);
    };

    // P1: an empty or malformed expected digest is refused outright. An empty
    // value must never silently skip binding the published bytes.
    if (!isPDFSha256(expectedSha256))
    {
        return refuse(QStringLiteral("expected-digest-missing"),
                      QStringLiteral("Governed revalidation requires the exact expected published-artifact digest."));
    }
    if (profile.isEmpty())
    {
        return refuse(QStringLiteral("profile-missing"),
                      QStringLiteral("Governed revalidation requires a non-empty preflight profile."));
    }

    QFile publishedFile(publishedPath);
    if (!publishedFile.open(QIODevice::ReadOnly))
    {
        return refuse(QStringLiteral("artifact-unreadable"),
                      QStringLiteral("Could not open the published artifact for revalidation."));
    }
    const QByteArray publishedBytes = publishedFile.readAll();
    if (publishedFile.error() != QFileDevice::NoError)
    {
        return refuse(QStringLiteral("artifact-unreadable"),
                      QStringLiteral("Could not read the published artifact for revalidation."));
    }

    revalidation->artifactSha256 = QString::fromLatin1(QCryptographicHash::hash(publishedBytes, QCryptographicHash::Sha256).toHex());
    if (!sha256Matches(revalidation->artifactSha256, expectedSha256))
    {
        return refuse(QStringLiteral("artifact-digest-mismatch"),
                      QStringLiteral("Published artifact bytes do not match the reviewed candidate (expected %1, actual %2).")
                          .arg(expectedSha256.trimmed().toLower(), revalidation->artifactSha256));
    }

    PDFDocumentReader reader(nullptr, [](bool*)
                             { return QString(); }, false, false);
    PDFDocument document = reader.readFromFile(publishedPath);
    if (reader.getReadingResult() != PDFDocumentReader::Result::OK)
    {
        return refuse(QStringLiteral("artifact-reopen-failed"),
                      QStringLiteral("The published artifact could not be reopened for revalidation."));
    }

    PreflightProfileData profileData;
    QString profileErrorCode;
    QString profileError;
    if (!resolveGovernedProfileData(profile, &profileData, &profileErrorCode, &profileError))
    {
        return refuse(profileErrorCode,
                      profileError.isEmpty()
                          ? QStringLiteral("The effective preflight profile could not be resolved for revalidation.")
                          : profileError);
    }

    // P2: a targeted plan is honored only when the declared impact is complete
    // and a baseline inspection is available. An undeclared, incomplete, or
    // document-wide impact (and a missing baseline) falls back to a full run.
    PDFRevalidationPlan plan = scope.plan;
    if (!plan.full && (!scope.impact.declared || !scope.impact.impactComplete || scope.impact.isFullRevalidation()))
    {
        const QString reason = !scope.impact.declared
                                   ? QStringLiteral("impact-undeclared")
                                   : (!scope.impact.impactComplete ? QStringLiteral("impact-incomplete")
                                                                   : QStringLiteral("impact-document-wide"));
        plan = fullGovernedRevalidationPlan(profileData, reason);
    }
    const bool targeted = !plan.full && scope.hasBaseline;
    if (!plan.full && !scope.hasBaseline)
    {
        plan = fullGovernedRevalidationPlan(profileData, QStringLiteral("baseline-unavailable"));
    }

    PDFDocumentSession session(&document);
    PreflightEngine engine(&session);
    PreflightResult result;
    if (targeted)
    {
        if (!scope.baselineEvidence.records.isEmpty())
        {
            result = engine.revalidate(profileData, plan, scope.baseline, scope.baselineEvidence);
        }
        else
        {
            result = engine.revalidate(profileData, scope.baseline, plan);
        }
    }
    else
    {
        result = engine.run(profileData, plan);
    }

    // P2: the finding delta is computed against the baseline inspection and
    // folded — with the scope mode and reason — into the report the sign-off
    // binds. `reportSha256` therefore covers the delta; a certificate cannot
    // omit it.
    PDFRepairFindingDelta delta;
    QString baselineReportSha256;
    if (scope.hasBaseline)
    {
        delta = computeFindingDelta(scope.baseline, result);
        baselineReportSha256 = digestJson(scope.baseline.toJson());
    }

    QString scopeMode;
    QString scopeReason = plan.reason;
    const QJsonObject engineProvenance = !result.revalidation.isEmpty()
                                             ? result.revalidation
                                             : result.coverageScope.value(QStringLiteral("revalidation")).toObject();
    if (!engineProvenance.isEmpty())
    {
        scopeMode = engineProvenance.value(QStringLiteral("mode")).toString();
        const QString engineReason = engineProvenance.value(QStringLiteral("reason")).toString();
        if (!engineReason.isEmpty())
        {
            scopeReason = engineReason;
        }
    }
    if (scopeMode.isEmpty())
    {
        // No targeted provenance was produced, so the run was a full inspection
        // (either requested or a fail-closed fallback).
        scopeMode = QStringLiteral("full");
    }

    QJsonObject report = result.toJson();
    report.insert(QStringLiteral("scope"), QJsonObject{
                                               { QStringLiteral("mode"), scopeMode },
                                               { QStringLiteral("reason"), scopeReason },
                                               { QStringLiteral("plan"), plan.toJson() },
                                               { QStringLiteral("impact"), scope.impact.toJson() } });
    report.insert(QStringLiteral("finding_delta"), delta.toJson());
    report.insert(QStringLiteral("baseline_report_sha256"), baselineReportSha256);

    revalidation->effectiveProfileDigest = result.effectiveProfileDigest;
    if (!isPDFSha256(revalidation->effectiveProfileDigest))
    {
        revalidation->effectiveProfileDigest = digestJson(profile);
    }
    revalidation->verdict = reducePreflightVerdict(result);
    revalidation->state = governedRevalidationStateForVerdict(revalidation->verdict);
    if (revalidation->state != QStringLiteral("complete"))
    {
        revalidation->reasonCode = revalidation->verdict.reasonCode.isEmpty()
                                       ? QStringLiteral("preflight-%1").arg(revalidation->state)
                                       : revalidation->verdict.reasonCode;
        revalidation->reason = revalidation->verdict.reason.isEmpty()
                                   ? preflightVerdictOperatorSummary(revalidation->verdict)
                                   : revalidation->verdict.reason;
    }
    report.insert(QStringLiteral("state"), revalidation->state);
    report.insert(QStringLiteral("reason_code"), revalidation->reasonCode);
    report.insert(QStringLiteral("reason"), revalidation->reason);
    revalidation->report = report;
    revalidation->reportSha256 = digestJson(report);
    revalidation->bytesVerified = true;
    if (!revalidation->isSignOffEligible())
    {
        return PDFOperationResult(preflightVerdictOperatorSummary(revalidation->verdict));
    }
    return PDFOperationResult(true);
}

PDFOperationResult finalizeGovernedPublication(const PDFGovernedExecutionApproval& approval,
                                               const QString& expectedPlanDigest,
                                               const QString& expectedSourceSha256,
                                               const QString& expectedCandidateSha256,
                                               const QString& publishedPath,
                                               const QJsonObject& profile,
                                               const QString& signOffActor,
                                               const QString& signOffPolicy,
                                               PDFGovernedExecutionRevalidation* revalidation,
                                               PDFGovernedExecutionSignOff* signOff,
                                               const PDFGovernedRevalidationScope& scope,
                                               const PDFApprovalAuthorizationContext& context)
{
    if (!revalidation || !signOff)
    {
        return PDFOperationResult(QStringLiteral("Governed publication outputs are null."));
    }
    if (const PDFOperationResult approvalResult = validateGovernedApproval(approval,
                                                                           expectedPlanDigest,
                                                                           expectedSourceSha256,
                                                                           expectedCandidateSha256,
                                                                           context);
        !approvalResult)
    {
        return approvalResult;
    }

    if (const PDFOperationResult revalidationResult = revalidateGovernedArtifact(publishedPath,
                                                                                 profile,
                                                                                 expectedCandidateSha256,
                                                                                 revalidation,
                                                                                 scope);
        !revalidationResult)
    {
        return revalidationResult;
    }

    PDFApprovalRecord certificateApproval;
    certificateApproval.kind = PDFApprovalKind::System;
    certificateApproval.actorId = signOffActor.trimmed();
    certificateApproval.decision = QStringLiteral("approve");
    certificateApproval.policyId = signOffPolicy.trimmed();
    certificateApproval.rationale = QStringLiteral("Published bytes were reopened and passed the effective preflight profile.");
    certificateApproval.evidenceSha256 = revalidation->reportSha256;
    certificateApproval.decisionReference = QStringLiteral("published-revalidation:%1").arg(revalidation->artifactSha256);
    certificateApproval.decidedUtc = QDateTime::currentDateTimeUtc();

    *signOff = PDFGovernedExecutionSignOff();
    signOff->planDigest = expectedPlanDigest.trimmed().toLower();
    signOff->sourceSha256 = expectedSourceSha256.trimmed().toLower();
    signOff->candidateSha256 = expectedCandidateSha256.trimmed().toLower();
    signOff->publishedSha256 = revalidation->artifactSha256;
    signOff->revalidationReportSha256 = revalidation->reportSha256;
    signOff->effectiveProfileDigest = revalidation->effectiveProfileDigest;
    signOff->approval = certificateApproval;
    return validateGovernedSignOff(*signOff,
                                   approval,
                                   *revalidation,
                                   expectedPlanDigest,
                                   expectedSourceSha256,
                                   expectedCandidateSha256,
                                   context);
}

PDFOperationResult validateGovernedSignOff(const PDFGovernedExecutionSignOff& signOff,
                                           const PDFGovernedExecutionApproval& approval,
                                           const PDFGovernedExecutionRevalidation& revalidation,
                                           const QString& expectedPlanDigest,
                                           const QString& expectedSourceSha256,
                                           const QString& expectedCandidateSha256,
                                           const PDFApprovalAuthorizationContext& context)
{
    if (!signOff.isValid())
    {
        return PDFOperationResult(QStringLiteral("Governed sign-off is incomplete or invalid."));
    }
    if (const PDFOperationResult approvalResult = validateGovernedApproval(approval,
                                                                           expectedPlanDigest,
                                                                           expectedSourceSha256,
                                                                           expectedCandidateSha256,
                                                                           context);
        !approvalResult)
    {
        return approvalResult;
    }
    if (!revalidation.isSignOffEligible())
    {
        return PDFOperationResult(QStringLiteral("Governed sign-off requires a complete passing revalidation."));
    }
    if (!sha256Matches(signOff.planDigest, expectedPlanDigest) ||
        !sha256Matches(signOff.sourceSha256, expectedSourceSha256) ||
        !sha256Matches(signOff.candidateSha256, expectedCandidateSha256) ||
        !sha256Matches(signOff.publishedSha256, revalidation.artifactSha256) ||
        !sha256Matches(signOff.revalidationReportSha256, revalidation.reportSha256) ||
        !sha256Matches(signOff.effectiveProfileDigest, revalidation.effectiveProfileDigest))
    {
        return PDFOperationResult(QStringLiteral("Governed sign-off does not match the published artifact or revalidation evidence."));
    }
    return PDFOperationResult(true);
}

PDFOperationResult publishGovernedArtifact(const PDFGovernedExecutionApproval& approval,
                                           const QString& expectedPlanDigest,
                                           const QString& expectedSourceSha256,
                                           const QByteArray& candidateBytes,
                                           const QString& outputPath,
                                           PDFSafeFileWriter::OverwritePolicy overwritePolicy,
                                           const PDFApprovalAuthorizationContext& context)
{
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateBytes, QCryptographicHash::Sha256).toHex());
    const PDFOperationResult validation = validateGovernedApproval(approval,
                                                                   expectedPlanDigest,
                                                                   expectedSourceSha256,
                                                                   candidateSha256,
                                                                   context);
    if (!validation)
    {
        return validation;
    }

    return PDFSafeFileWriter::writeData(outputPath, candidateBytes, overwritePolicy);
}

QJsonObject PDFGovernedMutationReceipt::toJson() const
{
    QJsonObject object{
        { QStringLiteral("schema"), QStringLiteral("loop.governed-mutation-receipt") },
        { QStringLiteral("schema_version"), schemaVersion },
        { QStringLiteral("status"), status },
        { QStringLiteral("reason_code"), reasonCode },
        { QStringLiteral("plan_digest"), planDigest },
        { QStringLiteral("source_sha256"), sourceSha256 },
        { QStringLiteral("candidate_sha256"), candidateSha256 },
        { QStringLiteral("destination_path"), destinationPath },
        { QStringLiteral("published_sha256"), publishedSha256 },
        { QStringLiteral("destination_touched"), destinationTouched }
    };
    if (!executionId.isNull())
    {
        object.insert(QStringLiteral("execution_id"), executionId.toString(QUuid::WithoutBraces));
    }
    if (isPublished() || !publishedSha256.isEmpty())
    {
        object.insert(QStringLiteral("revalidation"), revalidation.toJson());
        object.insert(QStringLiteral("sign_off"), signOff.toJson());
    }
    return object;
}

namespace
{

/// A staging file the gateway created for this attempt. It is removed on every
/// exit, including an early refusal, so a failed attempt leaves no residue beside
/// the destination. A caller-supplied staged path is never removed.
struct StagingFileGuard
{
    QString path;
    bool owned = false;

    ~StagingFileGuard()
    {
        if (owned && !path.isEmpty())
        {
            QFile::remove(path);
        }
    }
};

QJsonObject canonicalMutationSummary(const PDFGovernedMutationReceipt& receipt)
{
    return QJsonObject{
        { QStringLiteral("status"), receipt.status },
        { QStringLiteral("reason_code"), receipt.reasonCode }
    };
}

}   // namespace

PDFOperationResult executeGovernedMutation(const PDFGovernedMutationRequest& request,
                                           PDFGovernedMutationReceipt* receipt)
{
    if (!receipt)
    {
        return PDFOperationResult(QStringLiteral("Governed mutation receipt output is null."));
    }
    *receipt = PDFGovernedMutationReceipt();
    receipt->planDigest = request.planDigest.trimmed().toLower();
    receipt->sourceSha256 = request.sourceSha256.trimmed().toLower();
    receipt->candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(request.candidateBytes, QCryptographicHash::Sha256).toHex());
    receipt->destinationPath = request.destinationPath;

    const auto refuse = [receipt](const QString& status, const QString& code, const QString& message)
    {
        receipt->status = status;
        receipt->reasonCode = code;
        return PDFOperationResult(message);
    };

    // 1. Malformed request: nothing is inspected, nothing is written.
    if (!isPDFSha256(receipt->planDigest) || !isPDFSha256(receipt->sourceSha256) ||
        request.destinationPath.trimmed().isEmpty() || request.candidateBytes.isEmpty() ||
        !request.approval.isValid())
    {
        return refuse(QStringLiteral("refused"), QStringLiteral("malformed-request"),
                      QStringLiteral("The governed mutation request is incomplete or malformed."));
    }
    // 2. The approval must name the exact plan, source, and reviewed candidate.
    if (!sha256Matches(request.approval.planDigest, receipt->planDigest) ||
        !sha256Matches(request.approval.sourceSha256, receipt->sourceSha256) ||
        !sha256Matches(request.approval.candidateSha256, receipt->candidateSha256))
    {
        return refuse(QStringLiteral("refused"), QStringLiteral("approval-stale"),
                      QStringLiteral("The governed approval does not name the requested plan, source, and candidate bytes."));
    }
    if (request.approval.approval.decisionReference.startsWith(QStringLiteral("preflight-decision:"), Qt::CaseInsensitive))
    {
        return refuse(QStringLiteral("refused"), QStringLiteral("approval-invalid"),
                      QStringLiteral("Preflight finding decisions are not operation approval."));
    }
    // 3. Authorization (#36): the single current-and-authorized decision point.
    PDFApprovalAuthorizationContext authorization = request.authorization;
    if (!request.profileDigest.trimmed().isEmpty())
    {
        authorization.expectedProfileDigest = request.profileDigest;
    }
    const PDFApprovalAuthorization decision = resolveApprovalAuthorization(request.approval, authorization);
    if (!decision.allowed)
    {
        return refuse(QStringLiteral("refused"), decision.code, decision.reason);
    }
    // 4. Already-terminal replay: one plan produces one published candidate.
    if (request.history)
    {
        QString historyError;
        const QList<PDFOperationHistoryEvent> events = request.history->events(&historyError);
        const QString decisionReference = request.approval.approval.decisionReference.trimmed();
        for (const PDFOperationHistoryEvent& event : events)
        {
            if (event.kind != PDFOperationHistoryEventKind::FixApplied ||
                event.status != PDFOperationHistoryStatus::Accepted)
            {
                continue;
            }
            bool match = false;
            if (event.output.has_value() && sha256Matches(event.output->sha256, receipt->candidateSha256))
            {
                match = true;
            }
            if (!match && !decisionReference.isEmpty() &&
                event.approval.decisionReference.compare(decisionReference, Qt::CaseInsensitive) == 0)
            {
                match = true;
            }
            if (!match)
            {
                const QString storedPlanDigest = event.resultSummary.value(QStringLiteral("approval"))
                                                     .toObject()
                                                     .value(QStringLiteral("plan_digest"))
                                                     .toString();
                if (!storedPlanDigest.isEmpty() && sha256Matches(storedPlanDigest, receipt->planDigest))
                {
                    match = true;
                }
            }
            if (match)
            {
                return refuse(QStringLiteral("refused"), QStringLiteral("already-terminal"),
                              QStringLiteral("This execution identity is already terminal in the operation history."));
            }
        }
    }
    // 5. Cancellation before any staging side effect.
    if (PDFOperationControl::isOperationCancelled(request.operationControl))
    {
        return refuse(QStringLiteral("cancelled"), QStringLiteral("cancelled"),
                      QStringLiteral("The governed mutation was cancelled before publication."));
    }

    // 6. Stage the reviewed bytes beside the destination so finalize can reopen
    //    them without the destination ever holding un-finalized bytes.
    StagingFileGuard staging;
    staging.path = request.stagedCandidatePath.trimmed();
    staging.owned = staging.path.isEmpty();
    if (staging.owned)
    {
        const QFileInfo destinationInfo(request.destinationPath);
        staging.path = QDir(destinationInfo.absolutePath())
                           .filePath(destinationInfo.fileName() + QStringLiteral(".loop-staging-") +
                                     QUuid::createUuid().toString(QUuid::WithoutBraces));
        const PDFOperationResult staged = PDFSafeFileWriter::writeData(staging.path,
                                                                       request.candidateBytes,
                                                                       PDFSafeFileWriter::OverwritePolicy::Fail);
        if (!staged)
        {
            return refuse(QStringLiteral("failed"), QStringLiteral("staging-failed"), staged.getErrorMessage());
        }
    }
    else
    {
        QFile stagedFile(staging.path);
        if (!stagedFile.open(QIODevice::ReadOnly) ||
            QString::fromLatin1(QCryptographicHash::hash(stagedFile.readAll(), QCryptographicHash::Sha256).toHex()) != receipt->candidateSha256)
        {
            return refuse(QStringLiteral("failed"), QStringLiteral("staging-mismatch"),
                          QStringLiteral("The staged candidate does not match the reviewed candidate bytes."));
        }
    }

    // P4: every event this gateway appends binds the effective profile the
    // approval was authorized against, not only the accepted completion. A
    // running/failed event that drops it leaves an identity hole a reader
    // cannot reconstruct "which profile this attempt ran under".
    const QString effectiveProfileDigest =
        isPDFSha256(request.profileDigest)
            ? request.profileDigest.trimmed().toLower()
            : (isPDFSha256(request.approval.effectiveProfileDigest)
                   ? request.approval.effectiveProfileDigest.trimmed().toLower()
                   : QString());

    // 7. Chain append: begin the execution and record it as running before the commit.
    bool historyStarted = false;
    QUuid executionId = request.executionId;
    if (request.history)
    {
        if (executionId.isNull())
        {
            PDFOperationHistoryExecution execution;
            execution.operationId = request.operationId;
            execution.operationVersion = request.operationVersion;
            execution.input = request.inputArtifact;
            execution.parameters = request.parameters;
            execution.startedUtc = QDateTime::currentDateTimeUtc();
            if (!request.history->beginExecution(execution, &executionId))
            {
                return refuse(QStringLiteral("failed"), QStringLiteral("history-failed"),
                              QStringLiteral("Could not begin the governed execution history."));
            }
        }
        PDFOperationHistoryEvent running;
        running.executionId = executionId;
        running.kind = PDFOperationHistoryEventKind::FixApplied;
        running.status = PDFOperationHistoryStatus::Running;
        running.operatorIdentity = request.approval.approval.actorId;
        running.documentRevisionDigest = receipt->sourceSha256;
        running.effectiveProfileDigest = effectiveProfileDigest;
        running.approval = request.approval.approval;
        if (!request.history->appendEvent(running))
        {
            return refuse(QStringLiteral("failed"), QStringLiteral("history-failed"),
                          QStringLiteral("Could not append the governed execution start event."));
        }
        historyStarted = true;
        receipt->executionId = executionId;
    }
    const auto appendHistoryFailed = [&request, receipt, &executionId, historyStarted, effectiveProfileDigest](const QString& reason)
    {
        if (!request.history || !historyStarted)
        {
            return;
        }
        PDFOperationHistoryEvent failed;
        failed.executionId = executionId;
        failed.kind = PDFOperationHistoryEventKind::FixApplied;
        failed.status = PDFOperationHistoryStatus::Failed;
        failed.operatorIdentity = request.approval.approval.actorId;
        failed.documentRevisionDigest = receipt->candidateSha256;
        failed.effectiveProfileDigest = effectiveProfileDigest;
        failed.resultSummary = QJsonObject{ { QStringLiteral("status"), receipt->status },
                                            { QStringLiteral("reason_code"), reason } };
        failed.approval = request.approval.approval;
        request.history->appendEvent(failed);
    };

    // 8. Finalize against the staged bytes: reopen, revalidate, sign off. A failure
    //    here removes the staging file and leaves the destination untouched.
    PDFGovernedExecutionRevalidation revalidation;
    PDFGovernedExecutionSignOff signOff;
    bool revalidationFailed = false;
    if (request.requireRevalidation)
    {
        const PDFOperationResult finalizeResult = finalizeGovernedPublication(request.approval,
                                                                              receipt->planDigest,
                                                                              receipt->sourceSha256,
                                                                              receipt->candidateSha256,
                                                                              staging.path,
                                                                              request.profile,
                                                                              request.signOffActor,
                                                                              request.signOffPolicy,
                                                                              &revalidation,
                                                                              &signOff,
                                                                              request.revalidationScope,
                                                                              authorization);
        if (!finalizeResult)
        {
            receipt->revalidation = revalidation;
            if (!request.publishOnRevalidationFailure || !revalidation.bytesVerified)
            {
                appendHistoryFailed(QStringLiteral("revalidation-failed"));
                return refuse(QStringLiteral("failed"), QStringLiteral("revalidation-failed"), finalizeResult.getErrorMessage());
            }
            revalidationFailed = true;
        }
    }

    // 9. The commit seam and the last cancellation check: still nothing at the destination.
    if (request.beforeCommit)
    {
        request.beforeCommit();
    }
    if (PDFOperationControl::isOperationCancelled(request.operationControl))
    {
        appendHistoryFailed(QStringLiteral("cancelled"));
        return refuse(QStringLiteral("cancelled"), QStringLiteral("cancelled"),
                      QStringLiteral("The governed mutation was cancelled before publication."));
    }

    // 10. Atomic commit into the destination.
    const PDFOperationResult commitResult = PDFSafeFileWriter::writeData(request.destinationPath,
                                                                         request.candidateBytes,
                                                                         request.overwritePolicy);
    if (!commitResult)
    {
        const bool conflict = request.overwritePolicy == PDFSafeFileWriter::OverwritePolicy::Fail &&
                              QFileInfo::exists(request.destinationPath);
        const QString code = conflict ? QStringLiteral("destination-conflict") : QStringLiteral("commit-failed");
        appendHistoryFailed(code);
        return refuse(QStringLiteral("failed"), code, commitResult.getErrorMessage());
    }
    receipt->destinationTouched = true;
    receipt->publishedSha256 = receipt->candidateSha256;

    // 11. Read back and verify the destination holds exactly the reviewed bytes.
    QFile publishedFile(request.destinationPath);
    if (!publishedFile.open(QIODevice::ReadOnly) ||
        QString::fromLatin1(QCryptographicHash::hash(publishedFile.readAll(), QCryptographicHash::Sha256).toHex()) != receipt->candidateSha256)
    {
        appendHistoryFailed(QStringLiteral("read-back-failed"));
        return refuse(QStringLiteral("failed"), QStringLiteral("read-back-failed"),
                      QStringLiteral("The committed artifact does not match the reviewed candidate."));
    }

    // 12. Published: durable chain completion.
    receipt->revalidation = revalidation;
    receipt->signOff = signOff;
    receipt->status = QStringLiteral("published");
    receipt->reasonCode = revalidationFailed ? QStringLiteral("revalidation-forced") : QString();
    if (request.history && historyStarted)
    {
        PDFOperationHistoryEvent accepted;
        accepted.executionId = executionId;
        accepted.kind = PDFOperationHistoryEventKind::FixApplied;
        accepted.status = PDFOperationHistoryStatus::Accepted;
        accepted.operatorIdentity = signOff.approval.actorId;
        accepted.documentRevisionDigest = receipt->candidateSha256;
        accepted.effectiveProfileDigest = signOff.effectiveProfileDigest;
        if (!request.outputArtifact.sha256.trimmed().isEmpty())
        {
            accepted.output = request.outputArtifact;
        }
        accepted.reportArtifactSha256 = signOff.revalidationReportSha256;
        accepted.resultSummary = request.resultSummary ? request.resultSummary(*receipt) : canonicalMutationSummary(*receipt);
        accepted.approval = signOff.approval;
        if (!request.history->appendEvent(accepted))
        {
            receipt->status = QStringLiteral("failed");
            receipt->reasonCode = QStringLiteral("history-failed");
            return PDFOperationResult(QStringLiteral("The governed artifact was published, but its accepted history event could not be persisted."));
        }
    }
    return PDFOperationResult(true);
}

}   // namespace pdf
