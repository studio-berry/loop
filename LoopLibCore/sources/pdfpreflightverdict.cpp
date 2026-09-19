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
#include <QJsonArray>
#include <QTemporaryDir>

#include <algorithm>

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
                                               PreflightResult* postflightResultOut)
{
    if (!document || !result)
    {
        return PDFOperationResult(QStringLiteral("Declared repair validators require a document and result output."));
    }

    const bool requiresNormalPreflight = plan.requiresPostflight ||
                                         plan.validators.contains(PDFRepairValidatorKind::NormalPreflight);
    if (!requiresNormalPreflight)
    {
        return PDFOperationResult(true);
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
                return PDFOperationResult(loadError);
            }
        }
        const PDFDocument& impactDocument = baselineDocument ? *baselineDocument : *document;
        stepPlan = planRepairStepPreflight(operation,
                                           impactDocument,
                                           operationParameters,
                                           enabledPreflightCheckIds(profileObject),
                                           plan);
        validatorOptions.revalidationPlan = &stepPlan;
    }

    PreflightResult baselineResult;
    if (baselineDocument)
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
        // A failing baseline is expected when a repair is targeting a finding.
        // Only failures that happened before a normalized result existed are
        // infrastructure/profile failures that prevent a trustworthy delta.
        if (!baselineRun && baselineResult.profileName.isEmpty())
        {
            result->status = PDFRepairStatus::Incomplete;
            result->incompleteReasons.append(baselineRun.getErrorMessage());
            return baselineRun;
        }
    }

    PreflightVerdict verdict;
    PreflightResult preflightResult;
    const PDFOperationResult postflight = runMandatoryPostflight(document,
                                                                 profilePath,
                                                                 &verdict,
                                                                 &preflightResult,
                                                                 validatorOptions);
    if (!postflight && preflightResult.profileName.isEmpty())
    {
        result->status = PDFRepairStatus::Incomplete;
        result->incompleteReasons.append(postflight.getErrorMessage());
        return postflight;
    }
    result->verdict = verdict.toJson();
    if (postflightResultOut)
    {
        *postflightResultOut = preflightResult;
    }

    bool deltaIncomplete = false;
    bool introducedFindings = false;
    if (baselineDocument)
    {
        result->findingDelta = computeFindingDelta(baselineResult, preflightResult);
        deltaIncomplete = !baselineResult.inspectionComplete ||
                          !preflightResult.inspectionComplete ||
                          !result->findingDelta.incompleteFindingIds.isEmpty();
        introducedFindings = !result->findingDelta.introducedFindingIds.isEmpty();
    }

    PDFRepairValidationResult validation;
    validation.validatorId = QStringLiteral("normal-preflight");

    PDFOperationResult validationResult = postflight;
    if (deltaIncomplete)
    {
        validation.status = PDFRepairStatus::Incomplete;
        validation.summary = QStringLiteral("Repair revalidation was incomplete; no finding was cleared without complete evidence.");
        result->incompleteReasons.append(validation.summary);
        result->status = PDFRepairStatus::Incomplete;
        validationResult = PDFOperationResult(validation.summary);
    }
    else if (introducedFindings)
    {
        validation.status = PDFRepairStatus::Failed;
        validation.summary = QStringLiteral("Repair introduced %1 new preflight finding(s).")
                                 .arg(result->findingDelta.introducedFindingIds.size());
        validation.evidence = result->findingDelta.introducedFindingIds;
        result->validationFailures.append(validation.summary);
        result->status = PDFRepairStatus::Failed;
        validationResult = PDFOperationResult(validation.summary);
    }
    else if (postflight && verdict.isPass())
    {
        validation.status = PDFRepairStatus::Passed;
        validation.summary = preflightVerdictOperatorSummary(verdict);
        result->status = PDFRepairStatus::Passed;
    }
    else if (verdict.state == PreflightVerdictState::Incomplete)
    {
        validation.status = PDFRepairStatus::Incomplete;
        validation.summary = preflightVerdictOperatorSummary(verdict);
        result->incompleteReasons.append(validation.summary);
        result->status = PDFRepairStatus::Incomplete;
    }
    else
    {
        validation.status = PDFRepairStatus::Failed;
        validation.summary = preflightVerdictOperatorSummary(verdict);
        result->validationFailures.append(validation.summary);
        result->status = PDFRepairStatus::Failed;
    }
    result->validations.append(std::move(validation));

    return validationResult;
}

bool preflightAllowsCertification(const PreflightResult& result)
{
    const PreflightVerdict verdict = reducePreflightVerdict(result);
    if (!verdict.allowsCertificateIssuance())
    {
        return false;
    }
    return !result.profileIdentity.value(QStringLiteral("provisional")).toBool(false);
}

}   // namespace pdf
