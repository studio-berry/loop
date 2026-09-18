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

#include <QCryptographicHash>
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
        { QStringLiteral("approval"), approval.toJson() }
    };
}

PDFGovernedExecutionApproval PDFGovernedExecutionApproval::fromJson(const QJsonObject& object, QString* error)
{
    PDFGovernedExecutionApproval approval;
    approval.planDigest = object.value(QStringLiteral("plan_digest")).toString().toLower();
    approval.sourceSha256 = object.value(QStringLiteral("source_sha256")).toString().toLower();
    approval.candidateSha256 = object.value(QStringLiteral("candidate_sha256")).toString().toLower();
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

    PDFRepairDiffOptions options;
    options.renderVisualDiff = false;
    PDFRepairDiffReport report;
    const PDFOperationResult compareResult = transaction.compareCandidate(candidatePath, options, &report);
    if (!compareResult)
    {
        return compareResult;
    }

    *preview = PDFTechnicalPreview();
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

    PDFRepairDiffOptions visualOptions = options;
    visualOptions.renderVisualDiff = true;
    visualOptions.compareMetadata = false;
    visualOptions.compareResources = false;
    visualOptions.compareAnnotations = false;

    PDFRepairDiffReport report;
    const PDFOperationResult compareResult = transaction.compareCandidate(candidatePath, visualOptions, &report);
    if (!compareResult)
    {
        return compareResult;
    }

    *preview = PDFVisualPreview();
    preview->planDigest = planDigest;
    preview->sourceSha256 = report.sourceFingerprint;
    preview->candidateSha256 = report.candidateFingerprint;
    preview->status = report.status;
    preview->pages = report.pages;
    preview->warnings = report.warnings;
    preview->incompleteReasons = report.incompleteReasons;
    return PDFOperationResult(true);
}

PDFOperationResult validateGovernedApproval(const PDFGovernedExecutionApproval& approval,
                                            const QString& expectedPlanDigest,
                                            const QString& expectedSourceSha256,
                                            const QString& expectedCandidateSha256)
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
    return PDFOperationResult(true);
}

PDFOperationResult publishGovernedArtifact(const PDFGovernedExecutionApproval& approval,
                                           const QString& expectedPlanDigest,
                                           const QString& expectedSourceSha256,
                                           const QByteArray& candidateBytes,
                                           const QString& outputPath,
                                           PDFSafeFileWriter::OverwritePolicy overwritePolicy)
{
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateBytes, QCryptographicHash::Sha256).toHex());
    const PDFOperationResult validation = validateGovernedApproval(approval,
                                                                   expectedPlanDigest,
                                                                   expectedSourceSha256,
                                                                   candidateSha256);
    if (!validation)
    {
        return validation;
    }

    return PDFSafeFileWriter::writeData(outputPath, candidateBytes, overwritePolicy);
}

}   // namespace pdf
