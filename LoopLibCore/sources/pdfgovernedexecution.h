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

#include "pdfoperationhistory.h"
#include "pdfpreflightverdict.h"
#include "pdfrepairdiff.h"
#include "pdfrepairoperation.h"
#include "pdfsafefilewriter.h"
#include "preflightengine.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace pdf
{

struct LOOPLIBCORESHARED_EXPORT PDFTechnicalPreview
{
    int schemaVersion = 1;
    QString planDigest;
    QString sourceSha256;
    QString candidateSha256;
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
                                                                     const QString& expectedCandidateSha256);

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
                                                                    const QString& expectedCandidateSha256);

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
    PDFGovernedExecutionSignOff* signOff);

/// Publishes reviewed candidate bytes only after governed approval validation succeeds.
LOOPLIBCORESHARED_EXPORT PDFOperationResult publishGovernedArtifact(const PDFGovernedExecutionApproval& approval,
                                                                    const QString& expectedPlanDigest,
                                                                    const QString& expectedSourceSha256,
                                                                    const QByteArray& candidateBytes,
                                                                    const QString& outputPath,
                                                                    PDFSafeFileWriter::OverwritePolicy overwritePolicy);

}   // namespace pdf

#endif   // PDFGOVERNEDEXECUTION_H
