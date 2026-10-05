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

#ifndef PDFGOVERNEDEXECUTION_H
#define PDFGOVERNEDEXECUTION_H

#include "pdfoperationcontrol.h"
#include "pdfoperationhistory.h"
#include "pdfpreflightverdict.h"
#include "pdfrepairdiff.h"
#include "pdfrepairoperation.h"
#include "pdfsafefilewriter.h"
#include "preflightengine.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace pdf
{

class PDFOperationHistoryStore;

struct LOOPLIBCORESHARED_EXPORT PDFTechnicalPreview
{
    int schemaVersion = 1;
    QString planDigest;
    QString sourceSha256;
    QString candidateSha256;
    PDFRepairPreviewFidelity fidelity = PDFRepairPreviewFidelity::Exact;
    PDFRepairDiffStatus status = PDFRepairDiffStatus::Complete;
    QVector<PDFRepairStructuralChange> structuralChanges;
    QStringList warnings;
    QStringList incompleteReasons;

    QJsonObject toJson() const;
};

struct LOOPLIBCORESHARED_EXPORT PDFVisualPreview
{
    int schemaVersion = 1;
    QString planDigest;
    QString sourceSha256;
    QString candidateSha256;
    PDFRepairPreviewFidelity fidelity = PDFRepairPreviewFidelity::Simulated;
    PDFRepairDiffStatus status = PDFRepairDiffStatus::Complete;
    QVector<PDFRepairPageVisualDiff> pages;
    QStringList warnings;
    QStringList incompleteReasons;

    QJsonObject toJson() const;
};

/// Operator approval bound to one exact dry-run plan. Finding waivers and other
/// preflight decisions are not interchangeable with this record.
struct LOOPLIBCORESHARED_EXPORT PDFGovernedExecutionApproval
{
    QString planDigest;
    QString sourceSha256;
    QString candidateSha256;
    /// Optional binding of the approval to the effective preflight profile the
    /// caller will revalidate with. Empty means the caller does not bind a profile.
    QString effectiveProfileDigest;
    PDFApprovalRecord approval;

    bool isValid() const;
    QJsonObject toJson() const;
    static PDFGovernedExecutionApproval fromJson(const QJsonObject& object, QString* error = nullptr);
};

/// Evidence produced by inspecting the bytes that were actually published.
/// The report is not eligible for sign-off until the bytes, PDF reader, and
/// complete preflight verdict all pass.
struct LOOPLIBCORESHARED_EXPORT PDFGovernedExecutionRevalidation
{
    int schemaVersion = 1;
    bool bytesVerified = false;
    QString artifactSha256;
    QString reportSha256;
    QString effectiveProfileDigest;
    PreflightVerdict verdict;
    QJsonObject report;

    bool isSignOffEligible() const;
    QJsonObject toJson() const;
};

/// Certificate identity for one governed publication. Every digest binds the
/// certificate to the exact plan, source, published bytes, and revalidation.
struct LOOPLIBCORESHARED_EXPORT PDFGovernedExecutionSignOff
{
    int schemaVersion = 1;
    QString planDigest;
    QString sourceSha256;
    QString candidateSha256;
    QString publishedSha256;
    QString revalidationReportSha256;
    QString effectiveProfileDigest;
    PDFApprovalRecord approval;

    bool isValid() const;
    QJsonObject toJson() const;
};

/// Declared approver rights for one authorization decision. Empty allowlists mean
/// the caller declares no restriction on that axis; `requireExpiry` forces a
/// declared validity window.
struct LOOPLIBCORESHARED_EXPORT PDFApprovalAuthorizationPolicy
{
    QStringList authorizedActorIds;   // empty = any non-empty actor
    QList<PDFApprovalKind> authorizedKinds;   // empty = any non-None kind
    bool requireExpiry = false;
};

/// Inputs the gateway needs to decide whether an approval is current and
/// authorized. Every field is fail-closed: an unset field narrows, never widens.
struct LOOPLIBCORESHARED_EXPORT PDFApprovalAuthorizationContext
{
    PDFApprovalAuthorizationPolicy policy;   // allowlists; empty lists = no restriction declared
    QDateTime evaluatedUtc;   // invalid = expiry cannot be evaluated
    const PDFOperationHistoryStore* history = nullptr;   // null = revocation cannot be resolved
    QString expectedProfileDigest;   // empty = profile not bound by this caller
};

struct LOOPLIBCORESHARED_EXPORT PDFApprovalAuthorization
{
    bool allowed = false;
    QString code;
    QString reason;
};

/// Resolves one affirmative, current, authorized approval. Refuses (allowed=false)
/// on a non-affirmative decision, an unauthorized kind or actor, an expired or
/// undeclared-required expiry, a revoked decision reference, or a profile-digest
/// mismatch. `code` is a short fail-closed reason such as `approval-expired`.
///
/// The governed approval (not the bare record) is the subject because the profile
/// binding lives on the envelope: `approval.effectiveProfileDigest`.
LOOPLIBCORESHARED_EXPORT PDFApprovalAuthorization resolveApprovalAuthorization(
    const PDFGovernedExecutionApproval& approval,
    const PDFApprovalAuthorizationContext& context);

/// Returns a deterministic SHA-256 digest for the analyzed operation plan.
LOOPLIBCORESHARED_EXPORT QString computeOperationPlanDigest(const QList<PDFRepairPlan>& plans,
                                                            const QString& sourceSha256,
                                                            const PDFOperationSavePolicy& savePolicy);

/// Builds a structural-only preview. Visual page renders are intentionally omitted.
LOOPLIBCORESHARED_EXPORT PDFOperationResult buildTechnicalPreview(PDFRepairTransaction& transaction,
                                                                  const QString& candidatePath,
                                                                  const QString& planDigest,
                                                                  PDFTechnicalPreview* preview);

/// Builds a visual-only preview. Structural metadata comparison is omitted.
LOOPLIBCORESHARED_EXPORT PDFOperationResult buildVisualPreview(PDFRepairTransaction& transaction,
                                                               const QString& candidatePath,
                                                               const QString& planDigest,
                                                               const PDFRepairDiffOptions& options,
                                                               PDFVisualPreview* preview);

/// Returns false because preflight finding decisions are not operation approval.
LOOPLIBCORESHARED_EXPORT bool preflightDecisionQualifiesAsOperationApproval(const PreflightDecision& decision);

/// Validates that approval names the exact plan and artifact identities under review.
LOOPLIBCORESHARED_EXPORT PDFOperationResult validateGovernedApproval(const PDFGovernedExecutionApproval& approval,
                                                                     const QString& expectedPlanDigest,
                                                                     const QString& expectedSourceSha256,
                                                                     const QString& expectedCandidateSha256,
                                                                     const PDFApprovalAuthorizationContext& context = {});

/// Reopens the published path, verifies its bytes, and runs the supplied
/// profile against that reopened document.
LOOPLIBCORESHARED_EXPORT PDFOperationResult revalidateGovernedArtifact(const QString& publishedPath,
                                                                       const QJsonObject& profile,
                                                                       const QString& expectedSha256,
                                                                       PDFGovernedExecutionRevalidation* revalidation);

/// Validates the complete identity chain required for certificate issuance.
LOOPLIBCORESHARED_EXPORT PDFOperationResult validateGovernedSignOff(const PDFGovernedExecutionSignOff& signOff,
                                                                    const PDFGovernedExecutionApproval& approval,
                                                                    const PDFGovernedExecutionRevalidation& revalidation,
                                                                    const QString& expectedPlanDigest,
                                                                    const QString& expectedSourceSha256,
                                                                    const QString& expectedCandidateSha256,
                                                                    const PDFApprovalAuthorizationContext& context = {});

/// Completes the common post-publication gate used by every surface. The
/// supplied approval authorizes the reviewed candidate; the returned sign-off
/// is a separate certificate bound to the bytes that were actually reopened.
LOOPLIBCORESHARED_EXPORT PDFOperationResult finalizeGovernedPublication(
    const PDFGovernedExecutionApproval& approval,
    const QString& expectedPlanDigest,
    const QString& expectedSourceSha256,
    const QString& expectedCandidateSha256,
    const QString& publishedPath,
    const QJsonObject& profile,
    const QString& signOffActor,
    const QString& signOffPolicy,
    PDFGovernedExecutionRevalidation* revalidation,
    PDFGovernedExecutionSignOff* signOff,
    const PDFApprovalAuthorizationContext& context = {});

/// Publishes reviewed candidate bytes only after governed approval validation succeeds.
LOOPLIBCORESHARED_EXPORT PDFOperationResult publishGovernedArtifact(const PDFGovernedExecutionApproval& approval,
                                                                    const QString& expectedPlanDigest,
                                                                    const QString& expectedSourceSha256,
                                                                    const QByteArray& candidateBytes,
                                                                    const QString& outputPath,
                                                                    PDFSafeFileWriter::OverwritePolicy overwritePolicy,
                                                                    const PDFApprovalAuthorizationContext& context = {});

/// Terminal outcome of one governed mutation attempt. The receipt is the evidence
/// that a refusal/failure/cancel stopped before the destination was touched, and
/// the sole record of a publication. Schema `loop.governed-mutation-receipt`.
struct LOOPLIBCORESHARED_EXPORT PDFGovernedMutationReceipt
{
    int schemaVersion = 1;
    /// One of `published`, `refused`, `failed`, `cancelled`.
    QString status;
    /// Stable terminal code (`approval-stale`, `already-terminal`, `cancelled`, ...).
    /// Empty only for `published`.
    QString reasonCode;
    QString planDigest;
    QString sourceSha256;
    QString candidateSha256;
    QString destinationPath;
    /// SHA-256 of the bytes now at the destination. Empty when the destination was
    /// not touched (nonpublication). Set for a post-commit failure because the
    /// reviewed bytes are present.
    QString publishedSha256;
    /// True once the destination holds the reviewed candidate bytes. False means the
    /// destination was never written by this attempt.
    bool destinationTouched = false;
    /// Operation-history execution that recorded this attempt, when a store was in scope.
    QUuid executionId;
    /// Populated only when the staged bytes passed revalidation and sign-off.
    PDFGovernedExecutionRevalidation revalidation;
    PDFGovernedExecutionSignOff signOff;

    bool isPublished() const { return status == QStringLiteral("published"); }
    QJsonObject toJson() const;
};

/// One requested governed mutation. The destination and overwrite policy are
/// execution inputs and are never part of the plan digest (D3).
struct LOOPLIBCORESHARED_EXPORT PDFGovernedMutationRequest
{
    /// Approval bound to the exact plan, source, and reviewed candidate.
    PDFGovernedExecutionApproval approval;
    /// Authorization decision inputs (#36): policy, evaluated time, and optional
    /// history for revocation resolution.
    PDFApprovalAuthorizationContext authorization;
    QString planDigest;
    QString sourceSha256;
    /// Exact reviewed candidate bytes.
    QByteArray candidateBytes;
    /// Optional caller-owned staged file that already holds exactly `candidateBytes`.
    /// When set, the gateway finalizes against it instead of writing its own staging
    /// file, and does not remove it. The bytes are still verified against
    /// `candidateBytes` before finalize.
    QString stagedCandidatePath;
    QString destinationPath;
    PDFSafeFileWriter::OverwritePolicy overwritePolicy = PDFSafeFileWriter::OverwritePolicy::Fail;
    QJsonObject profile;
    /// Optional effective-profile digest the approval must match. Overrides
    /// `authorization.expectedProfileDigest` when non-empty.
    QString profileDigest;
    QString signOffActor;
    QString signOffPolicy;
    /// When false the gateway commits without revalidation/sign-off. The receipt is
    /// still `published` but carries empty revalidation and sign-off (an unsigned
    /// publication, e.g. a PageMaster export with no preflight profile).
    bool requireRevalidation = true;
    /// When true a revalidation failure whose bytes were verified does not block the
    /// commit (PageMaster `forcePreflight`: publish the bytes even though the profile
    /// failed). The receipt records `revalidation-forced` as its reason code.
    bool publishOnRevalidationFailure = false;
    /// Cancellation control. Checked before staging and again at the `beforeCommit` seam.
    const PDFOperationControl* operationControl = nullptr;
    /// Test/qualification seam invoked after revalidation and before the commit,
    /// mirroring `PDFPageMasterExportJob::beforeOutputCommit`. May request cancellation.
    std::function<void()> beforeCommit;

    /// Operation-history store to scan for an already-terminal execution and to
    /// append the canonical chain events to. Null leaves both to the caller.
    PDFOperationHistoryStore* history = nullptr;
    /// Existing execution to append to. When null and `history` is set, the gateway
    /// begins one from `operationId` / `inputArtifact` / `parameters`.
    QUuid executionId;
    QString operationId;
    int operationVersion = 1;
    PDFArtifactIdentity inputArtifact;
    QJsonObject parameters;
    /// Artifact identity of the published bytes, referenced by the accepted event.
    PDFArtifactIdentity outputArtifact;
    /// Builds the accepted event's result summary from the final receipt. When unset
    /// the gateway records a canonical `{status, reason_code}` summary.
    std::function<QJsonObject(const PDFGovernedMutationReceipt&)> resultSummary;
};

/// The one cancellation-safe entry point for an approved corrective mutation:
/// validate approval + authorization, refuse an already-terminal replay, check
/// cancellation, stage the candidate, finalize (revalidate + sign-off) against the
/// staged bytes, run the `beforeCommit` seam and cancel check, commit atomically,
/// read back, and return a receipt. Any refusal/failure/cancel before the commit
/// leaves no destination artifact; a failure after the commit is terminal `failed`
/// with the artifact present and the reason recorded.
LOOPLIBCORESHARED_EXPORT PDFOperationResult executeGovernedMutation(const PDFGovernedMutationRequest& request,
                                                                    PDFGovernedMutationReceipt* receipt);

}   // namespace pdf

#endif   // PDFGOVERNEDEXECUTION_H
