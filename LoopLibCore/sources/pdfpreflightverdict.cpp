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

PDFRevalidationPlan planRepairStepPreflight(const PDFRepairOperation* operation,
                                            const PDFDocument& document,
                                            const QJsonObject& parameters,
                                            const QStringList& enabledCheckIds)
{
    PDFRevalidationPlan plan;
    plan.full = false;
    plan.reason = QStringLiteral("step-scoped");

    static const QHash<QString, QStringList> operationChecks = {
        { QStringLiteral("add-bleed"),
          { QStringLiteral("bleed"), QStringLiteral("content-bleed"), QStringLiteral("trim"), QStringLiteral("page-size") } },
        { QStringLiteral("rgb-to-cmyk"), { QStringLiteral("color-mode"), QStringLiteral("color-inventory") } },
        { QStringLiteral("downsample-images"), { QStringLiteral("image-resolution") } },
    };

    if (operation)
    {
        for (const QString& checkId : operationChecks.value(operation->id()))
        {
            if (enabledCheckIds.contains(checkId))
            {
                plan.checkIds.append(checkId);
            }
        }

        PDFOperationImpact impact = operation->impact(&document, parameters);
        impact.documentWide = false;
        const PDFRevalidationPlan domainPlan = planRevalidation(impact, enabledCheckIds);
        if (!domainPlan.full)
        {
            for (const QString& checkId : domainPlan.checkIds)
            {
                if (!plan.checkIds.contains(checkId))
                {
                    plan.checkIds.append(checkId);
                }
            }
            plan.pages.unite(domainPlan.pages);
        }
    }

    if (plan.checkIds.isEmpty())
    {
        plan.full = true;
        plan.checkIds = enabledCheckIds;
        plan.reason = QStringLiteral("step-scope-fallback");
    }
    return plan;
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

PDFOperationResult runDeclaredRepairValidators(PDFDocument* document,
                                               const PDFRepairPlan& plan,
                                               const QString& profilePath,
                                               PDFRepairResult* result,
                                               MandatoryPostflightOptions options,
                                               const PDFRepairOperation* operation,
                                               const QJsonObject& operationParameters,
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
                stepPlan = planRepairStepPreflight(operation,
                                                   *document,
                                                   operationParameters,
                                                   enabledPreflightCheckIds(profileObject));
                validatorOptions.revalidationPlan = &stepPlan;
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

    const bool inspectedCandidate = needsPreflight && !preflight.profileName.isEmpty();
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
            validation.summary = inspectedCandidate
                                     ? preflightVerdictOperatorSummary(verdict)
                                     : QStringLiteral("Normal preflight was not inspected: %1").arg(failure);
            validation.status = inspectedCandidate && inspected && verdict.isPass()
                                    ? PDFRepairStatus::Passed
                                : inspectedCandidate && verdict.state == PreflightVerdictState::Fail
                                    ? PDFRepairStatus::Failed
                                    : PDFRepairStatus::Incomplete;
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
            verdict.state = PreflightVerdictState::Incomplete;
            verdict.reasonCode = QStringLiteral("validator-not-inspected");
            verdict.reason = failure;
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
