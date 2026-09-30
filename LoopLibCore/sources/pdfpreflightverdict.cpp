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

#include "pdfdocumentsession.h"
#include "pdfrepairdiff.h"
#include "pdfrepairoperation.h"
#include "preflightprofileresolver.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <iterator>

namespace pdf
{

namespace
{

bool isNonBlockingIncompleteFinding(const PreflightFinding& finding)
{
    return finding.type == QStringLiteral("budget-exceeded") || finding.type == QStringLiteral("check-incomplete") || finding.type == QStringLiteral("evidence-incomplete") || finding.evidence.value(QStringLiteral("budget_exceeded")).toBool(false);
}

bool isActiveDecisionForFinding(const PreflightFinding& finding,
                                const QList<PreflightDecision>& decisions,
                                const QString& documentDigest,
                                const QString& profileDigest)
{
    const QString findingId = finding.stableId();
    for (const PreflightDecision& decision : decisions)
    {
        if (decision.findingId == findingId && decision.countsForSignoff(documentDigest, profileDigest))
        {
            return true;
        }
    }
    return false;
}

QString incompleteReasonCode(const PreflightResult& result)
{
    for (const PreflightCheckStatus& status : result.checkStatuses)
    {
        if (status.status == QStringLiteral("incomplete") || status.status == QStringLiteral("unsupported"))
        {
            if (status.reason == QStringLiteral("budget-exceeded") || !status.budgetKind.isEmpty())
            {
                return QStringLiteral("budget-exceeded");
            }
            if (!status.reason.isEmpty())
            {
                return status.reason;
            }
        }
    }

    if (result.pdfx.has_value() && result.pdfx->status == PDFXConformanceStatus::Incomplete)
    {
        return QStringLiteral("pdfx-evidence-incomplete");
    }
    return QStringLiteral("inspection-incomplete");
}

QString incompleteReason(const PreflightResult& result)
{
    for (const PreflightCheckStatus& status : result.checkStatuses)
    {
        if ((status.status == QStringLiteral("incomplete") || status.status == QStringLiteral("unsupported")) && !status.reason.isEmpty())
        {
            return status.reason;
        }
    }
    if (result.pdfx.has_value() && result.pdfx->status == PDFXConformanceStatus::Incomplete)
    {
        return QStringLiteral("Mandatory PDF/X evidence was not available.");
    }
    return QStringLiteral("Required inspection evidence was not collected.");
}

bool isFailClosedIncompleteErrorCode(const QString& errorCode)
{
    return errorCode == QLatin1String("unsupported-scope") || errorCode == QLatin1String("unresolved-variable") || errorCode == QLatin1String("budget-exceeded") || errorCode == QLatin1String("evidence-incomplete") || errorCode == QLatin1String("cancelled");
}

QStringList enabledPreflightCheckIds(const QJsonObject& profileObject)
{
    QStringList enabledCheckIds;
    const QJsonArray checks = profileObject.value(QStringLiteral("checks")).toArray();
    for (const QJsonValue& value : checks)
    {
        const QJsonObject check = value.toObject();
        if (check.value(QStringLiteral("enabled")).toBool(true))
        {
            enabledCheckIds.append(check.value(QStringLiteral("id")).toString());
        }
    }
    return enabledCheckIds;
}

PDFOperationResult resolveMandatoryPostflightProfile(const QString& profilePath,
                                                     MandatoryPostflightOptions options,
                                                     QJsonObject* profileObject,
                                                     PreflightProfileData* profileData,
                                                     QString* errorMessage)
{
    if (options.profileJson.isEmpty() && profilePath.trimmed().isEmpty())
    {
        return PDFOperationResult(QStringLiteral("A preflight profile path is required before a candidate can be published."));
    }

    QJsonObject profile = options.profileJson;
    if (profile.isEmpty())
    {
        QString loadError;
        if (!PreflightEngine::loadProfile(profilePath, profile, loadError))
        {
            return PDFOperationResult(loadError);
        }
    }

    const PreflightProfileImportResult imported = importPreflightProfile(profile,
                                                                         profilePath.trimmed().isEmpty()
                                                                             ? profile.value(QStringLiteral("id")).toString()
                                                                             : profilePath);
    if (!imported.ok)
    {
        return PDFOperationResult(imported.errorMessage);
    }

    const PreflightVariableBindResult bound = bindPreflightProfileVariables(imported.profile, options.profileBindings);
    if (!bound.ok)
    {
        return PDFOperationResult(bound.errorMessage);
    }

    PreflightProfileData parsedProfile;
    QString parseError;
    if (!PreflightEngine::parseProfile(bound.profile, parsedProfile, parseError))
    {
        return PDFOperationResult(parseError);
    }
    parsedProfile.variableBindings = bound.bindings;
    parsedProfile.fileDigest = imported.identity.digest;
    parsedProfile.effectiveDigest = computeProfileDigest(bound.profile);
    parsedProfile.profileIdentity = imported.identity.toJson();
    parsedProfile.profileIdentity.insert(QStringLiteral("effective_digest"), parsedProfile.effectiveDigest);

    if (profileObject)
    {
        *profileObject = bound.profile;
    }
    if (profileData)
    {
        *profileData = std::move(parsedProfile);
    }
    if (errorMessage)
    {
        errorMessage->clear();
    }
    return PDFOperationResult(true);
}

}   // namespace

PDFRevalidationPlan planRepairStepPreflight(const PDFRepairOperation* operation,
                                            const PDFDocument& document,
                                            const QJsonObject& parameters,
                                            const QStringList& enabledCheckIds,
                                            const PDFRepairPlan& repairPlan)
{
    PDFRevalidationPlan full;
    full.full = true;
    full.checkIds = enabledCheckIds;
    full.reason = QStringLiteral("operation-impact-unknown");
    if (!operation)
    {
        return full;
    }

    const PDFOperationImpact declared = operation->impact(&document, parameters);
    // The operation-owned impact is authoritative. A page-local target in a
    // repair plan cannot narrow a declared document-wide/full-rewrite/oracle
    // impact, and cannot repair an unknown or incomplete impact declaration.
    PDFRevalidationPlan plan = planRevalidation(declared, enabledCheckIds);
    if (plan.full)
    {
        return plan;
    }

    QSet<int> targetPages;
    for (const PDFRepairTarget& target : repairPlan.targets)
    {
        if (target.pageIndex < 0)
        {
            plan.pages.clear();
            plan.reason = QStringLiteral("document-target");
            return plan;
        }
        targetPages.insert(target.pageIndex);
    }
    plan.pages.unite(targetPages);

    // Only these object-evidence checks can currently honor a page subset
    // without deriving document-global evidence from a partial collection.
    // Others still benefit from selected checks but run on all document pages.
    if (!plan.pages.isEmpty())
    {
        const bool allChecksPageLocal = std::all_of(
            plan.checkIds.cbegin(), plan.checkIds.cend(),
            [](const QString& checkId)
            {
                return checkId == QLatin1String("image-resolution") ||
                       checkId == QLatin1String("thin-strokes");
            });
        if (!allChecksPageLocal)
        {
            plan.pages.clear();
            plan.reason = QStringLiteral("check-scoped-document-pages");
        }
    }
    return plan;
}

QString preflightVerdictStateToString(PreflightVerdictState state)
{
    switch (state)
    {
        case PreflightVerdictState::Pass:
            return QStringLiteral("pass");
        case PreflightVerdictState::Fail:
            return QStringLiteral("fail");
        case PreflightVerdictState::Incomplete:
            return QStringLiteral("incomplete");
        case PreflightVerdictState::Error:
            return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

PreflightVerdict preflightVerdictFromJson(const QJsonObject& object)
{
    PreflightVerdict verdict;
    const QString state = object.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("pass"))
    {
        verdict.state = PreflightVerdictState::Pass;
    }
    else if (state == QStringLiteral("fail"))
    {
        verdict.state = PreflightVerdictState::Fail;
    }
    else if (state == QStringLiteral("incomplete"))
    {
        verdict.state = PreflightVerdictState::Incomplete;
    }
    else
    {
        verdict.state = PreflightVerdictState::Error;
    }
    verdict.reasonCode = object.value(QStringLiteral("reason_code")).toString();
    verdict.reason = object.value(QStringLiteral("reason")).toString();
    const QJsonArray blocking = object.value(QStringLiteral("blocking_finding_ids")).toArray();
    for (const QJsonValue& value : blocking)
    {
        verdict.blockingFindingIds.append(value.toString());
    }
    const QJsonArray waived = object.value(QStringLiteral("waived_finding_ids")).toArray();
    for (const QJsonValue& value : waived)
    {
        verdict.waivedFindingIds.append(value.toString());
    }
    return verdict;
}

int preflightVerdictProcessExitCode(PreflightVerdictState state)
{
    switch (state)
    {
        case PreflightVerdictState::Pass:
            return 0;
        case PreflightVerdictState::Fail:
            return 1;
        case PreflightVerdictState::Incomplete:
            return 8;
        case PreflightVerdictState::Error:
            return 9;
    }
    return 9;
}

QString preflightVerdictOperatorSummary(const PreflightVerdict& verdict)
{
    // These strings are rendered verbatim by PreflightPane.qml, so they go
    // through the same Core translation context as preflightGateFailureMessage()
    // rather than being English-only literals.
    const char* context = "pdf::PreflightVerdict";
    switch (verdict.state)
    {
        case PreflightVerdictState::Pass:
            return verdict.waivedFindingIds.isEmpty()
                       ? QCoreApplication::translate(context, "No problems found.")
                       : QCoreApplication::translate(context, "No problems found. Active dispositions cover previously blocking findings.");
        case PreflightVerdictState::Fail:
            return verdict.reason.isEmpty()
                       ? QCoreApplication::translate(context, "Blocking findings require resolution or an active disposition.")
                       : verdict.reason;
        case PreflightVerdictState::Incomplete:
            return QCoreApplication::translate(context, "Could not finish inspecting. %1")
                .arg(verdict.reason.isEmpty()
                         ? QCoreApplication::translate(context, "Required inspection evidence was not collected.")
                         : verdict.reason);
        case PreflightVerdictState::Error:
            return verdict.reason.isEmpty()
                       ? QCoreApplication::translate(context, "The preflight engine could not complete the operation.")
                       : verdict.reason;
    }
    return QCoreApplication::translate(context, "The preflight engine could not complete the operation.");
}

QString preflightGateFailureMessage(const QString& fileName,
                                    PreflightVerdictState state,
                                    bool revalidation)
{
    const QString prefix = revalidation ? QStringLiteral("Final preflight revalidation") : QStringLiteral("Preflight");
    switch (state)
    {
        case PreflightVerdictState::Incomplete:
            return QCoreApplication::translate("pdf::PreflightVerdict",
                                               "%1 could not finish inspecting '%2'.")
                .arg(prefix, fileName);
        case PreflightVerdictState::Error:
            return QCoreApplication::translate("pdf::PreflightVerdict",
                                               "%1 error for '%2'.")
                .arg(prefix, fileName);
        case PreflightVerdictState::Fail:
        case PreflightVerdictState::Pass:
            break;
    }
    return QCoreApplication::translate("pdf::PreflightVerdict",
                                       "%1 failed for '%2'.")
        .arg(prefix, fileName);
}

QJsonObject PreflightVerdict::toJson() const
{
    QJsonArray blocking;
    for (const QString& findingId : blockingFindingIds)
    {
        blocking.append(findingId);
    }
    QJsonArray waived;
    for (const QString& findingId : waivedFindingIds)
    {
        waived.append(findingId);
    }
    return QJsonObject{
        { QStringLiteral("state"), preflightVerdictStateToString(state) },
        { QStringLiteral("reason_code"), reasonCode },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("blocking_finding_ids"), blocking },
        { QStringLiteral("waived_finding_ids"), waived }
    };
}

PreflightVerdict reducePreflightVerdict(const PreflightResult& result,
                                        const PreflightProfileData* effectiveProfile)
{
    PreflightVerdict verdict;

    if (!result.errorCode.trimmed().isEmpty())
    {
        const QString code = result.errorCode.trimmed();
        if (isFailClosedIncompleteErrorCode(code))
        {
            verdict.state = PreflightVerdictState::Incomplete;
            verdict.reasonCode = code;
            verdict.reason = result.errorMessage.isEmpty()
                                 ? incompleteReason(result)
                                 : result.errorMessage;
            return verdict;
        }

        verdict.state = PreflightVerdictState::Error;
        verdict.reasonCode = code;
        verdict.reason = result.errorMessage.isEmpty()
                             ? QStringLiteral("The preflight engine could not complete the operation.")
                             : result.errorMessage;
        return verdict;
    }

    for (const PreflightFinding& finding : result.errors)
    {
        if (isNonBlockingIncompleteFinding(finding))
        {
            continue;
        }

        const QString findingId = finding.stableId();
        if (isActiveDecisionForFinding(finding,
                                       result.decisions,
                                       result.documentRevisionDigest,
                                       result.effectiveProfileDigest))
        {
            verdict.waivedFindingIds.append(findingId);
        }
        else
        {
            verdict.blockingFindingIds.append(findingId);
        }
    }

    if (effectiveProfile && verdict.blockingFindingIds.isEmpty())
    {
        for (const PreflightCheckConfig& check : effectiveProfile->checks)
        {
            if (!check.enabled || !check.required)
            {
                continue;
            }

            const auto status = std::find_if(result.checkStatuses.cbegin(),
                                             result.checkStatuses.cend(),
                                             [&check](const PreflightCheckStatus& candidate)
                                             {
                                                 return candidate.id == check.id;
                                             });
            if (status == result.checkStatuses.cend())
            {
                verdict.state = PreflightVerdictState::Incomplete;
                verdict.reasonCode = QStringLiteral("required-check-not-run");
                verdict.reason = QStringLiteral("Required check '%1' did not produce an execution status.").arg(check.id);
                return verdict;
            }
        }
    }

    if (!verdict.blockingFindingIds.isEmpty())
    {
        verdict.state = PreflightVerdictState::Fail;
        verdict.reasonCode = QStringLiteral("blocking-findings");
        verdict.reason = QStringLiteral("One or more blocking findings require resolution or an active disposition.");
    }
    else if (!result.inspectionComplete)
    {
        verdict.state = PreflightVerdictState::Incomplete;
        verdict.reasonCode = incompleteReasonCode(result);
        verdict.reason = incompleteReason(result);
    }
    else
    {
        verdict.state = PreflightVerdictState::Pass;
        verdict.reasonCode = verdict.waivedFindingIds.isEmpty()
                                 ? QStringLiteral("no-blocking-findings")
                                 : QStringLiteral("blocking-findings-waived");
        verdict.reason = verdict.waivedFindingIds.isEmpty()
                             ? QStringLiteral("Inspection completed with no blocking findings.")
                             : QStringLiteral("Inspection completed; all blocking findings have an active disposition.");
    }

    return verdict;
}

bool buildPreflightInspectionReceipt(const PreflightResult& result,
                                     const PreflightProfileData& profile,
                                     const PDFRevisionIdentity& revision,
                                     const PDFEvidenceGraph& evidence,
                                     PreflightInspectionReceipt& receipt,
                                     QString& errorMessage)
{
    receipt = {};
    if (!isPDFSha256(result.documentRevisionDigest) || !isPDFSha256(profile.effectiveDigest) ||
        result.effectiveProfileDigest.compare(profile.effectiveDigest, Qt::CaseInsensitive) != 0 ||
        !revision.isValid())
    {
        errorMessage = QStringLiteral("Inspection receipt requires a valid input digest, matching effective profile digest and document revision.");
        return false;
    }
    if ((!evidence.artifact.sha256.isEmpty() &&
         evidence.artifact.sha256.compare(result.documentRevisionDigest, Qt::CaseInsensitive) != 0) ||
        (evidence.revision.isValid() && evidence.revision != revision))
    {
        errorMessage = QStringLiteral("Inspection evidence belongs to a different input or revision.");
        return false;
    }

    PreflightInspectionReceipt candidate;
    candidate.inputDigest = result.documentRevisionDigest.toLower();
    candidate.revision = revision;
    candidate.effectiveProfileDigest = profile.effectiveDigest.toLower();
    candidate.profileIdentity = result.profileIdentity.isEmpty() ? profile.profileIdentity : result.profileIdentity;
    candidate.coverageScope = result.coverageScope;
    candidate.verdict = reducePreflightVerdict(result, &profile);

    bool incompleteCoverage = false;
    QSet<QString> declaredChecks;
    const auto appendCheck = [&](const QString& id, bool required)
    {
        PreflightReceiptCheck check;
        check.id = id;
        check.required = required;
        const auto status = std::find_if(result.checkStatuses.cbegin(), result.checkStatuses.cend(),
                                         [&id](const PreflightCheckStatus& value)
                                         { return value.id == id; });
        if (status != result.checkStatuses.cend())
        {
            check.status = status->status;
            check.reason = status->reason;
            const bool unique = std::find_if(std::next(status), result.checkStatuses.cend(),
                                             [&id](const PreflightCheckStatus& value)
                                             { return value.id == id; }) == result.checkStatuses.cend();
            check.complete = unique && status->budgetKind.isEmpty() &&
                             (status->status == QLatin1String("ok") ||
                              status->status == QLatin1String("warning") ||
                              status->status == QLatin1String("failed"));
        }
        if (!check.complete)
        {
            incompleteCoverage = true;
            QString reason = check.reason;
            if (reason.isEmpty())
            {
                reason = check.status.isEmpty() ? QStringLiteral("no status") : check.status;
            }
            candidate.limitations.append(QStringLiteral("Check '%1' did not complete: %2")
                                             .arg(id, reason));
        }
        candidate.checks.append(std::move(check));
    };

    for (const PreflightCheckConfig& check : profile.checks)
    {
        if (!check.enabled)
        {
            continue;
        }
        if (check.id.isEmpty() || declaredChecks.contains(check.id))
        {
            errorMessage = QStringLiteral("Inspection profile has an empty or duplicate enabled check ID.");
            return false;
        }
        declaredChecks.insert(check.id);
        appendCheck(check.id, check.required);
    }
    if (profile.pdfx.has_value())
    {
        appendCheck(QStringLiteral("pdfx"), true);
    }
    if (candidate.checks.isEmpty() || candidate.coverageScope.isEmpty())
    {
        incompleteCoverage = true;
        candidate.limitations.append(QStringLiteral("Inspection check coverage or scope was not recorded."));
    }
    for (const PreflightCheckStatus& status : result.checkStatuses)
    {
        if (!declaredChecks.contains(status.id) && status.id != QLatin1String("pdfx") &&
            (!status.budgetKind.isEmpty() || status.status == QLatin1String("incomplete") ||
             status.status == QLatin1String("unsupported") || status.status == QLatin1String("skipped") ||
             status.status == QLatin1String("not_inspected")))
        {
            incompleteCoverage = true;
            candidate.limitations.append(QStringLiteral("Inspection status '%1' did not complete.").arg(status.id));
        }
    }

    QSet<QString> evidenceIds;
    int fidelityRank = 3;
    for (const PDFEvidenceRecord& record : evidence.records)
    {
        if ((!record.artifact.sha256.isEmpty() &&
             record.artifact.sha256.compare(result.documentRevisionDigest, Qt::CaseInsensitive) != 0) ||
            (record.revision.isValid() && record.revision != revision))
        {
            errorMessage = QStringLiteral("Inspection evidence record belongs to a different input or revision.");
            return false;
        }
        if (!record.id.isEmpty())
        {
            evidenceIds.insert(record.id);
        }
        if (!record.incompleteReason.isEmpty())
        {
            incompleteCoverage = true;
            candidate.limitations.append(record.incompleteReason);
        }
        if (record.fidelity == QLatin1String("catalog"))
        {
            fidelityRank = std::min(fidelityRank, 1);
        }
        else if (record.fidelity == QLatin1String("sampled"))
        {
            fidelityRank = std::min(fidelityRank, 2);
        }
        else if (record.fidelity != QLatin1String("exact"))
        {
            fidelityRank = 0;
            incompleteCoverage = true;
            candidate.limitations.append(QStringLiteral("Evidence fidelity is unsupported or unknown."));
        }
    }
    const auto appendFindingEvidence = [&evidenceIds](const QList<PreflightFinding>& findings)
    {
        for (const PreflightFinding& finding : findings)
        {
            for (const QString& id : finding.evidenceIds)
            {
                if (!id.isEmpty())
                {
                    evidenceIds.insert(id);
                }
            }
        }
    };
    appendFindingEvidence(result.errors);
    appendFindingEvidence(result.warnings);
    candidate.evidenceRefs = evidenceIds.values();
    candidate.evidenceRefs.sort();
    candidate.fidelity = evidence.records.isEmpty() ? QStringLiteral("not-recorded")
                         : fidelityRank == 3        ? QStringLiteral("exact")
                         : fidelityRank == 2        ? QStringLiteral("sampled")
                         : fidelityRank == 1        ? QStringLiteral("catalog")
                                                    : QStringLiteral("unsupported");
    if (!evidence.isComplete())
    {
        incompleteCoverage = true;
        candidate.limitations.append(evidence.incompleteReason.isEmpty()
                                         ? QStringLiteral("Evidence collection did not complete.")
                                         : evidence.incompleteReason);
    }
    const QString coverageClaim = candidate.coverageScope.value(QStringLiteral("claim")).toString();
    if (!coverageClaim.isEmpty())
    {
        candidate.limitations.append(coverageClaim);
    }
    candidate.limitations.removeDuplicates();
    if (candidate.verdict.isPass() && incompleteCoverage)
    {
        candidate.verdict.state = PreflightVerdictState::Incomplete;
        candidate.verdict.reasonCode = QStringLiteral("receipt-evidence-incomplete");
        candidate.verdict.reason = QStringLiteral("Required inspection coverage or evidence did not complete.");
    }

    const QJsonObject identityData{
        { QStringLiteral("kind"), QStringLiteral("loop.inspection-receipt-identity.v1") },
        { QStringLiteral("input_digest"), candidate.inputDigest },
        { QStringLiteral("effective_profile_digest"), candidate.effectiveProfileDigest },
        { QStringLiteral("coverage_scope"), candidate.coverageScope }
    };
    candidate.identity = QString::fromLatin1(
        QCryptographicHash::hash(canonicalJson(identityData), QCryptographicHash::Sha256).toHex());
    receipt = std::move(candidate);
    errorMessage.clear();
    return true;
}

PDFOperationResult runMandatoryPostflight(PDFDocument* document,
                                          const QString& profilePath,
                                          PreflightVerdict* verdictOut,
                                          PreflightResult* resultOut,
                                          MandatoryPostflightOptions options)
{
    if (!document || !verdictOut)
    {
        return PDFOperationResult(QStringLiteral("Mandatory postflight requires a document and verdict output."));
    }

    QJsonObject profileObject;
    PreflightProfileData profileData;
    const PDFOperationResult profileResult = resolveMandatoryPostflightProfile(profilePath, options, &profileObject, &profileData, nullptr);
    if (!profileResult)
    {
        return profileResult;
    }

    QTemporaryDir tempDir;
    if (!tempDir.isValid())
    {
        return PDFOperationResult(QStringLiteral("Mandatory postflight could not create a temporary candidate file."));
    }
    const QString candidatePath = tempDir.filePath(QStringLiteral("candidate.pdf"));

    PDFDocument reopenedCandidate;
    const PDFOperationResult serialized = PDFRepairDiffEngine::buildSerializedCandidate(
        *document,
        [](PDFDocument*)
        { return PDFOperationResult(true); },
        candidatePath,
        &reopenedCandidate,
        nullptr);
    if (!serialized)
    {
        return serialized;
    }

    PDFDocumentSession* session = PDFDocumentSession::createForInspection(&reopenedCandidate);
    PreflightEngine engine(session);
    engine.setOperationControl(options.operationControl);
    const PreflightResult preflight = options.revalidationPlan
                                          ? engine.run(profileData, *options.revalidationPlan)
                                          : engine.run(profileData);
    PDFDocumentSession::destroy(session);

    if (PDFOperationControl::isOperationCancelled(options.operationControl))
    {
        PreflightVerdict cancelledVerdict;
        cancelledVerdict.state = PreflightVerdictState::Incomplete;
        cancelledVerdict.reasonCode = QStringLiteral("cancelled");
        cancelledVerdict.reason = QStringLiteral("Mandatory postflight was cancelled.");
        *verdictOut = cancelledVerdict;
        if (resultOut)
        {
            *resultOut = preflight;
            resultOut->inspectionComplete = false;
            resultOut->errorCode = cancelledVerdict.reasonCode;
            resultOut->errorMessage = cancelledVerdict.reason;
        }
        return PDFOperationResult(cancelledVerdict.reason);
    }

    const PreflightVerdict verdict = reducePreflightVerdict(preflight, &profileData);
    *verdictOut = verdict;
    if (resultOut)
    {
        *resultOut = preflight;
    }

    if (verdict.state == PreflightVerdictState::Incomplete && options.allowIncomplete)
    {
        return PDFOperationResult(true);
    }
    if (verdict.state == PreflightVerdictState::Pass)
    {
        return PDFOperationResult(true);
    }

    const QString summary = preflightVerdictOperatorSummary(verdict);
    if (verdict.state == PreflightVerdictState::Incomplete)
    {
        return PDFOperationResult(QStringLiteral("Postflight did not inspect the complete candidate: %1").arg(summary));
    }
    return PDFOperationResult(summary);
}

PDFRepairFindingDelta computeRepairStepFindingDelta(const PDFDocument& baselineDocument,
                                                    PDFDocument* afterDocument,
                                                    const PDFRepairPlan& plan,
                                                    const QString& profilePath,
                                                    MandatoryPostflightOptions options,
                                                    const PDFRepairOperation* operation,
                                                    const QJsonObject& operationParameters)
{
    PDFRepairFindingDelta empty;
    if (!afterDocument)
    {
        return empty;
    }

    MandatoryPostflightOptions validatorOptions = options;
    PDFRevalidationPlan stepPlan;
    if (operation && !validatorOptions.revalidationPlan)
    {
        QJsonObject profileObject = validatorOptions.profileJson;
        if (profileObject.isEmpty())
        {
            QString loadError;
            if (!PreflightEngine::loadProfile(profilePath, profileObject, loadError))
            {
                return empty;
            }
        }
        stepPlan = planRepairStepPreflight(operation,
                                           baselineDocument,
                                           operationParameters,
                                           enabledPreflightCheckIds(profileObject),
                                           plan);
        validatorOptions.revalidationPlan = &stepPlan;
    }

    PDFDocument baselineCandidate = baselineDocument;
    PreflightVerdict baselineVerdict;
    PreflightResult baselineResult;
    MandatoryPostflightOptions baselineOptions = validatorOptions;
    baselineOptions.allowIncomplete = true;
    runMandatoryPostflight(&baselineCandidate,
                           profilePath,
                           &baselineVerdict,
                           &baselineResult,
                           baselineOptions);

    PreflightVerdict afterVerdict;
    PreflightResult afterResult;
    runMandatoryPostflight(afterDocument,
                           profilePath,
                           &afterVerdict,
                           &afterResult,
                           validatorOptions);

    return computeFindingDelta(baselineResult, afterResult);
}

PDFOperationResult runDeclaredRepairValidators(PDFDocument* document,
                                               const PDFRepairPlan& plan,
                                               const QString& profilePath,
                                               PDFRepairResult* result,
                                               MandatoryPostflightOptions options,
                                               const PDFRepairOperation* operation,
                                               const QJsonObject& operationParameters,
                                               const PDFDocument* baselineDocument,
                                               PreflightResult* postflightOut)
{
    if (!document || !result)
    {
        return PDFOperationResult(QStringLiteral("Declared repair validators require a document and result output."));
    }

    QList<PDFRepairValidatorKind> declared = plan.validators;
    if (plan.requiresPostflight && !declared.contains(PDFRepairValidatorKind::NormalPreflight))
    {
        declared.append(PDFRepairValidatorKind::NormalPreflight);
    }
    if (declared.isEmpty())
    {
        return PDFOperationResult(true);
    }

    const bool needsPreflight = std::any_of(declared.cbegin(), declared.cend(),
                                            [](PDFRepairValidatorKind kind)
                                            { return kind != PDFRepairValidatorKind::StructuralIntegrity; });
    PreflightVerdict verdict;
    PreflightResult preflight;
    PreflightResult baselineResult;
    bool baselineCompared = false;
    PDFOperationResult inspected(true);
    if (needsPreflight)
    {
        MandatoryPostflightOptions validatorOptions = options;
        validatorOptions.allowIncomplete = false;
        PDFRevalidationPlan stepPlan;
        if (operation && !validatorOptions.revalidationPlan)
        {
            QJsonObject profileObject = validatorOptions.profileJson;
            if (profileObject.isEmpty())
            {
                QString loadError;
                if (!PreflightEngine::loadProfile(profilePath, profileObject, loadError))
                {
                    inspected = PDFOperationResult(loadError);
                }
            }
            if (inspected)
            {
                // The declared operation impact owns the revalidation scope; the
                // pre-fix document is the one whose pages the repair targeted.
                const PDFDocument& impactDocument = baselineDocument ? *baselineDocument : *document;
                stepPlan = planRepairStepPreflight(operation,
                                                   impactDocument,
                                                   operationParameters,
                                                   enabledPreflightCheckIds(profileObject),
                                                   plan);
                validatorOptions.revalidationPlan = &stepPlan;
            }
        }
        if (inspected && baselineDocument)
        {
            PDFDocument baselineCandidate = *baselineDocument;
            PreflightVerdict baselineVerdict;
            MandatoryPostflightOptions baselineOptions = validatorOptions;
            baselineOptions.allowIncomplete = true;
            const PDFOperationResult baselineRun = runMandatoryPostflight(&baselineCandidate,
                                                                          profilePath,
                                                                          &baselineVerdict,
                                                                          &baselineResult,
                                                                          baselineOptions);
            // A failing baseline is expected when a repair targets a finding. Only
            // a baseline that produced no normalized report at all is an
            // infrastructure or profile failure that would make the delta lie.
            baselineCompared = !baselineResult.profileName.isEmpty();
            if (!baselineRun && !baselineCompared)
            {
                result->status = PDFRepairStatus::Incomplete;
                result->incompleteReasons.append(baselineRun.getErrorMessage());
                inspected = baselineRun;
            }
        }
        if (inspected)
        {
            inspected = runMandatoryPostflight(document, profilePath, &verdict, &preflight, validatorOptions);
        }
        if (postflightOut)
        {
            *postflightOut = preflight;
        }
    }
    else
    {
        QTemporaryDir directory;
        if (!directory.isValid())
        {
            inspected = PDFOperationResult(QStringLiteral("Structural validation could not create a candidate directory."));
        }
        else
        {
            PDFDocument reopened;
            inspected = PDFRepairDiffEngine::buildSerializedCandidate(
                *document,
                [](PDFDocument*)
                { return PDFOperationResult(true); },
                directory.filePath(QStringLiteral("candidate.pdf")),
                &reopened,
                nullptr);
        }
    }

    // The delta is evidence about the repair, so it must be computed whenever the
    // after-run produced a normalized report — including a failing verdict. A repair
    // that introduces a finding is exactly the case the delta has to name.
    const bool inspectedCandidate = needsPreflight && !preflight.profileName.isEmpty();
    bool deltaIncomplete = false;
    bool introducedFindings = false;
    if (inspectedCandidate && baselineCompared)
    {
        result->findingDelta = computeFindingDelta(baselineResult, preflight);
        deltaIncomplete = !baselineResult.inspectionComplete || !preflight.inspectionComplete ||
                          !result->findingDelta.incompleteFindingIds.isEmpty();
        introducedFindings = !result->findingDelta.introducedFindingIds.isEmpty();
    }

    QString failure;
    if (!inspected)
    {
        failure = inspected.getErrorMessage();
    }

    for (PDFRepairValidatorKind kind : declared)
    {
        PDFRepairValidationResult validation;
        validation.validatorId = pdfRepairValidatorName(kind);
        if (kind == PDFRepairValidatorKind::StructuralIntegrity)
        {
            if (!inspected && !inspectedCandidate)
            {
                validation.status = PDFRepairStatus::Incomplete;
                validation.summary = QStringLiteral("Candidate serialization and reopening were not verified: %1").arg(failure);
            }
            else
            {
                validation.status = PDFRepairStatus::Passed;
                validation.summary = QStringLiteral("Candidate was serialized and reopened successfully.");
            }
        }
        else if (kind == PDFRepairValidatorKind::NormalPreflight)
        {
            if (inspectedCandidate && deltaIncomplete)
            {
                validation.status = PDFRepairStatus::Incomplete;
                validation.summary = QStringLiteral("Repair revalidation was incomplete; no finding was cleared without complete evidence.");
            }
            else if (inspectedCandidate && introducedFindings)
            {
                validation.status = PDFRepairStatus::Failed;
                validation.summary = QStringLiteral("Repair introduced %1 new preflight finding(s).")
                                         .arg(result->findingDelta.introducedFindingIds.size());
                validation.evidence = result->findingDelta.introducedFindingIds;
            }
            else
            {
                validation.summary = inspectedCandidate
                                         ? preflightVerdictOperatorSummary(verdict)
                                         : QStringLiteral("Normal preflight was not inspected: %1").arg(failure);
                validation.status = inspectedCandidate && inspected && verdict.isPass()
                                        ? PDFRepairStatus::Passed
                                    : inspectedCandidate && verdict.state == PreflightVerdictState::Fail
                                        ? PDFRepairStatus::Failed
                                        : PDFRepairStatus::Incomplete;
            }
        }
        else
        {
            QString requiredCheck;
            switch (kind)
            {
                case PDFRepairValidatorKind::ImageResolution:
                    requiredCheck = QStringLiteral("image-resolution");
                    break;
                case PDFRepairValidatorKind::ColorMode:
                    requiredCheck = QStringLiteral("color-mode");
                    break;
                case PDFRepairValidatorKind::OutputIntent:
                    requiredCheck = QStringLiteral("output-intent");
                    break;
                case PDFRepairValidatorKind::FontIntegrity:
                    requiredCheck = QStringLiteral("font-integrity");
                    break;
                case PDFRepairValidatorKind::StructuralIntegrity:
                case PDFRepairValidatorKind::NormalPreflight:
                case PDFRepairValidatorKind::TextExtraction:
                case PDFRepairValidatorKind::SignatureState:
                case PDFRepairValidatorKind::Custom:
                    break;
            }
            const auto status = std::find_if(preflight.checkStatuses.cbegin(), preflight.checkStatuses.cend(),
                                             [&](const PreflightCheckStatus& check)
                                             { return check.id == requiredCheck; });
            if (!requiredCheck.isEmpty() && inspectedCandidate && status != preflight.checkStatuses.cend() &&
                (status->status == QStringLiteral("ok") || status->status == QStringLiteral("warning")))
            {
                validation.status = PDFRepairStatus::Passed;
                validation.summary = QStringLiteral("Check '%1' was inspected in the effective postflight.").arg(requiredCheck);
            }
            else
            {
                validation.status = PDFRepairStatus::Incomplete;
                validation.summary = requiredCheck.isEmpty()
                                         ? QStringLiteral("Validator '%1' has no executable verification contract.").arg(validation.validatorId)
                                         : QStringLiteral("Validator '%1' requires a completed '%2' postflight check.").arg(validation.validatorId, requiredCheck);
            }
        }

        if (validation.status == PDFRepairStatus::Incomplete)
        {
            result->incompleteReasons.append(validation.summary);
            if (failure.isEmpty())
            {
                failure = validation.summary;
            }
        }
        else if (validation.status == PDFRepairStatus::Failed)
        {
            result->validationFailures.append(validation.summary);
            if (failure.isEmpty())
            {
                failure = validation.summary;
            }
        }
        result->validations.append(std::move(validation));
    }

    if (needsPreflight)
    {
        if (!inspectedCandidate)
        {
            // No normalized report means no validator evidence at all: fail closed
            // with an explicit incomplete status rather than a bare failure.
            result->status = PDFRepairStatus::Incomplete;
            verdict.state = PreflightVerdictState::Incomplete;
            verdict.reasonCode = QStringLiteral("validator-not-inspected");
            verdict.reason = failure;
        }
        if (deltaIncomplete && verdict.isPass())
        {
            verdict.state = PreflightVerdictState::Incomplete;
            verdict.reasonCode = QStringLiteral("revalidation-incomplete");
            verdict.reason = result->incompleteReasons.isEmpty() ? failure : result->incompleteReasons.first();
        }
        if (introducedFindings && (verdict.isPass() || verdict.state == PreflightVerdictState::Incomplete))
        {
            verdict.state = PreflightVerdictState::Fail;
            verdict.reasonCode = QStringLiteral("repair-introduced-findings");
            verdict.reason = result->validationFailures.isEmpty() ? failure : result->validationFailures.first();
        }
        if (!result->incompleteReasons.isEmpty() && verdict.isPass())
        {
            verdict.state = PreflightVerdictState::Incomplete;
            verdict.reasonCode = QStringLiteral("declared-validator-incomplete");
            verdict.reason = result->incompleteReasons.first();
        }
        result->verdict = verdict.toJson();
    }

    if (!failure.isEmpty())
    {
        return PDFOperationResult(failure);
    }
    return inspected;
}

bool preflightAllowsCertification(const PreflightResult& result)
{
    if (!result.inspectionComplete)
    {
        return false;
    }
    for (const PreflightCheckStatus& status : result.checkStatuses)
    {
        const QString normalizedStatus = status.status.trimmed().toLower();
        const QString normalizedReason = status.reason.trimmed().toLower();
        if (normalizedStatus.isEmpty() ||
            normalizedStatus == QLatin1String("skipped") ||
            normalizedStatus == QLatin1String("incomplete") ||
            normalizedStatus == QLatin1String("unsupported") ||
            normalizedStatus == QLatin1String("not_inspected") ||
            normalizedStatus == QLatin1String("not-inspected") ||
            normalizedReason == QLatin1String("budget-exceeded") ||
            !status.budgetKind.isEmpty())
        {
            return false;
        }
    }
    const PreflightVerdict verdict = reducePreflightVerdict(result);
    if (!verdict.allowsCertificateIssuance())
    {
        return false;
    }
    return !result.profileIdentity.value(QStringLiteral("provisional")).toBool(false);
}

}   // namespace pdf
