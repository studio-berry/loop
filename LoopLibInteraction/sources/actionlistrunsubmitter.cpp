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

#include "actionlistrunsubmitter.h"
#include "pdfsecurityhandler.h"

#include "pdfdocumentwriter.h"
#include "pdfgovernedexecution.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>

#include <stdexcept>

namespace
{

void appendValidationDiagnostic(pdf::PDFActionListStepResult* step, const QString& message)
{
    step->status = pdf::PDFActionListStepStatus::Failed;
    step->diagnostics.append(QJsonObject{
        { QStringLiteral("code"), QStringLiteral("action-list.validation-failed") },
        { QStringLiteral("severity"), QStringLiteral("error") },
        { QStringLiteral("message"), message } });
}

QVector<pdf::PDFActionListStepResult> validationSteps(const pdf::PDFActionList& actionList,
                                                      const QStringList& errors)
{
    QVector<pdf::PDFActionListStepResult> steps;
    steps.reserve(actionList.steps.size());
    for (const pdf::PDFActionListStep& actionStep : actionList.steps)
    {
        pdf::PDFActionListStepResult step;
        step.stepId = actionStep.id;
        step.operationId = actionStep.operationId;
        step.status = pdf::PDFActionListStepStatus::Pending;
        for (const QString& error : errors)
        {
            if (error.contains(QStringLiteral("step '%1'").arg(actionStep.id), Qt::CaseInsensitive) ||
                error.contains(QStringLiteral("step.%1.").arg(actionStep.id), Qt::CaseSensitive))
            {
                appendValidationDiagnostic(&step, error);
            }
        }
        steps.append(std::move(step));
    }
    return steps;
}

}   // namespace

namespace pdfinteraction
{

ActionListRunWorker makeActionListRunWorker(ActionListRunPhase phase,
                                            pdf::PDFActionList actionList,
                                            pdf::PDFDocumentPointer document,
                                            QJsonObject bindings,
                                            std::shared_ptr<ActionListWorkerOutcome> outcome,
                                            QString preflightProfilePath,
                                            QJsonObject preflightProfile,
                                            QJsonObject preflightProfileBindings)
{
    return [phase, actionList = std::move(actionList), document = std::move(document), bindings = std::move(bindings), outcome = std::move(outcome), preflightProfilePath = std::move(preflightProfilePath), preflightProfile = std::move(preflightProfile), preflightProfileBindings = std::move(preflightProfileBindings)](pdf::PDFJobContext& context)
    {
        if (!outcome || !document)
        {
            throw std::runtime_error("Action List worker inputs are unavailable.");
        }
        if (context.isCancellationRequested())
        {
            return;
        }

        outcome->phase = phase;
        const auto* security = document->getStorage().getSecurityHandler();
        if (!security || !security->isAllowed(pdf::PDFSecurityHandler::Permission::Modify) ||
            !security->isAllowed(pdf::PDFSecurityHandler::Permission::Assemble))
        {
            outcome->ok = false;
            outcome->errorMessage = QStringLiteral("Authenticated document permissions prohibit correction planning and publication.");
            outcome->executionResult.status = QStringLiteral("failed");
            outcome->executionResult.diagnostics.append(QJsonObject{
                { QStringLiteral("code"), QStringLiteral("document/correction-permission-denied") },
                { QStringLiteral("severity"), QStringLiteral("error") },
                { QStringLiteral("message"), QStringLiteral("Authenticated document permissions prohibit correction planning and publication.") } });
            return;
        }
        QJsonObject effectivePreflightProfile = preflightProfile;
        if (effectivePreflightProfile.isEmpty() && !preflightProfilePath.trimmed().isEmpty())
        {
            QString profileError;
            if (!pdf::PreflightEngine::loadProfile(preflightProfilePath, effectivePreflightProfile, profileError))
            {
                outcome->ok = false;
                outcome->executionResult.status = QStringLiteral("failed");
                outcome->executionResult.diagnostics.append(QJsonObject{
                    { QStringLiteral("code"), QStringLiteral("action-list.profile-unreadable") },
                    { QStringLiteral("severity"), QStringLiteral("error") },
                    { QStringLiteral("message"), profileError } });
                return;
            }
        }

        pdf::PDFActionListExecutionOptions options =
            pdf::makeActionListExecutionOptions(*document, bindings, context.operationControl());
        options.preflightProfilePath = preflightProfilePath;
        options.preflightProfile = effectivePreflightProfile;
        options.preflightProfileBindings = preflightProfileBindings;
        options.requirePostflight = phase == ActionListRunPhase::Execute;
        pdf::PDFActionListExecutor executor;

        context.reportProgress(10);
        if (phase == ActionListRunPhase::Validate)
        {
            const pdf::PDFOperationResult validation = executor.validate(actionList, options, &outcome->validationErrors);
            outcome->ok = bool(validation);
            if (!outcome->ok)
            {
                outcome->validationSteps = ::validationSteps(actionList, outcome->validationErrors);
            }
            context.reportProgress(95);
            context.setResultSummary(outcome->ok ? QStringLiteral("Action List validated.")
                                                 : QStringLiteral("Action List validation failed."));
            return;
        }

        if (phase == ActionListRunPhase::Plan)
        {
            const pdf::PDFOperationResult planResult = executor.plan(actionList, *document, options, &outcome->executionResult);
            outcome->ok = bool(planResult);
            context.reportProgress(95);
            context.setResultSummary(outcome->ok ? QStringLiteral("Action List planned.")
                                                 : QStringLiteral("Action List planning failed."));
            return;
        }

        pdf::PDFDocument candidate;
        const pdf::PDFOperationResult executeResult = executor.execute(actionList, *document, options, &candidate, &outcome->executionResult);
        outcome->ok = bool(executeResult);
        if (context.isCancellationRequested())
        {
            return;
        }
        if (outcome->ok && candidate != pdf::PDFDocument())
        {
            if (!effectivePreflightProfile.isEmpty())
            {
                QTemporaryDir stagingDirectory;
                const QString stagedCandidatePath =
                    stagingDirectory.isValid() ? stagingDirectory.filePath(QStringLiteral("editor-candidate.pdf")) : QString();
                QTemporaryDir publicationDirectory;
                const QString publicationPath =
                    publicationDirectory.isValid() ? publicationDirectory.filePath(QStringLiteral("editor-publication.pdf")) : QString();
                // The trusted source path is not known at this layer and the
                // publication target is a fresh temporary artifact, so this
                // validation cannot refuse today; it keeps the publication
                // boundary on the same save contract as the CLI write paths.
                pdf::PDFSaveRequest publicationRequest;
                publicationRequest.outputPath = publicationPath;
                publicationRequest.required = outcome->executionResult.savePolicy;
                publicationRequest.requested = outcome->executionResult.savePolicy;
                publicationRequest.appendInPlace = false;
                const pdf::PDFOperationResult saveRequest = pdf::validateSaveRequest(publicationRequest);
                if (stagedCandidatePath.isEmpty() || publicationPath.isEmpty())
                {
                    outcome->ok = false;
                    outcome->executionResult.status = QStringLiteral("failed");
                    outcome->executionResult.diagnostics.append(QJsonObject{
                        { QStringLiteral("code"), QStringLiteral("action-list.publication-serialize-failed") },
                        { QStringLiteral("severity"), QStringLiteral("error") },
                        { QStringLiteral("message"), QStringLiteral("The Editor candidate could not be staged for governed revalidation.") } });
                }
                else if (!saveRequest)
                {
                    outcome->ok = false;
                    outcome->executionResult.status = QStringLiteral("failed");
                    outcome->executionResult.diagnostics.append(QJsonObject{
                        { QStringLiteral("code"), QStringLiteral("save-policy.refused") },
                        { QStringLiteral("severity"), QStringLiteral("error") },
                        { QStringLiteral("message"), saveRequest.getErrorMessage() } });
                }
                else if (!pdf::PDFStandardConversion::writeCandidate(
                             candidate, stagedCandidatePath, outcome->executionResult.standardValidationRequirements,
                             &candidate, nullptr, &outcome->executionResult.independentValidation, context.operationControl()))
                {
                    outcome->ok = false;
                    outcome->executionResult.status = QStringLiteral("failed");
                    outcome->executionResult.diagnostics.append(QJsonObject{
                        { QStringLiteral("code"), QStringLiteral("action-list.publication-serialize-failed") },
                        { QStringLiteral("severity"), QStringLiteral("error") },
                        { QStringLiteral("message"), QStringLiteral("The Editor candidate could not be serialized for governed revalidation.") } });
                }
                else
                {
                    QFile stagedFile(stagedCandidatePath);
                    QByteArray candidateData;
                    if (!stagedFile.open(QIODevice::ReadOnly))
                    {
                        outcome->ok = false;
                        outcome->executionResult.status = QStringLiteral("failed");
                        outcome->executionResult.diagnostics.append(QJsonObject{
                            { QStringLiteral("code"), QStringLiteral("action-list.publication-read-failed") },
                            { QStringLiteral("severity"), QStringLiteral("error") },
                            { QStringLiteral("message"), QStringLiteral("The Editor candidate could not be read back for governed revalidation.") } });
                    }
                    else
                    {
                        candidateData = stagedFile.readAll();
                        const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256).toHex());
                        const QString expectedProfileDigest = effectivePreflightProfile.isEmpty()
                                                                  ? QString()
                                                                  : pdf::computeProfileDigest(effectivePreflightProfile);
                        pdf::PDFGovernedExecutionApproval approval;
                        approval.planDigest = outcome->executionResult.planDigest;
                        approval.sourceSha256 = outcome->executionResult.sourceSha256;
                        approval.candidateSha256 = candidateSha256;
                        approval.effectiveProfileDigest = expectedProfileDigest;
                        approval.approval.kind = pdf::PDFApprovalKind::Human;
                        approval.approval.actorId = QStringLiteral("Editor");
                        approval.approval.decision = QStringLiteral("approve");
                        approval.approval.policyId = QStringLiteral("desktop-confirmation");
                        approval.approval.rationale = QStringLiteral("The operator confirmed the Action List plan in the Editor.");
                        approval.approval.evidenceSha256 = approval.planDigest;
                        approval.approval.decisionReference = QStringLiteral("editor-confirmation:%1").arg(approval.planDigest);
                        approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();
                        // The Editor worker has no operation-history store in scope, so
                        // revocation cannot be resolved here; #37 centralizes execution
                        // where the chain becomes available.
                        pdf::PDFApprovalAuthorizationContext authorizationContext;
                        authorizationContext.evaluatedUtc = QDateTime::currentDateTimeUtc();
                        authorizationContext.expectedProfileDigest = expectedProfileDigest;

                        // The gateway validates the exact staged bytes the worker applies,
                        // never a second serialization; the artifact the operator receives
                        // is the reopened candidate from those same bytes.
                        pdf::PDFGovernedMutationRequest mutation;
                        mutation.approval = approval;
                        mutation.authorization = authorizationContext;
                        mutation.planDigest = approval.planDigest;
                        mutation.sourceSha256 = approval.sourceSha256;
                        mutation.candidateBytes = candidateData;
                        mutation.stagedCandidatePath = stagedCandidatePath;
                        mutation.destinationPath = publicationPath;
                        mutation.overwritePolicy = pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite;
                        mutation.profile = effectivePreflightProfile;
                        mutation.profileDigest = expectedProfileDigest;
                        mutation.signOffActor = QStringLiteral("Editor");
                        mutation.signOffPolicy = QStringLiteral("desktop-postflight");
                        mutation.operationControl = context.operationControl();
                        pdf::PDFGovernedMutationReceipt receipt;
                        const pdf::PDFOperationResult governedResult = pdf::executeGovernedMutation(mutation, &receipt);
                        outcome->executionResult.governed = QJsonObject{
                            { QStringLiteral("approval"), approval.toJson() },
                            { QStringLiteral("revalidation"), receipt.revalidation.toJson() },
                            { QStringLiteral("sign_off"), receipt.signOff.toJson() },
                            { QStringLiteral("receipt"), receipt.toJson() }
                        };
                        if (!governedResult)
                        {
                            outcome->ok = false;
                            outcome->executionResult.status = QStringLiteral("failed");
                            outcome->executionResult.diagnostics.append(QJsonObject{
                                { QStringLiteral("code"), QStringLiteral("action-list.publication-revalidation-failed") },
                                { QStringLiteral("severity"), QStringLiteral("error") },
                                { QStringLiteral("message"), governedResult.getErrorMessage() } });
                        }
                    }
                }
            }
        }
        if (outcome->ok && candidate != pdf::PDFDocument())
        {
            outcome->candidate = pdf::PDFDocumentPointer(new pdf::PDFDocument(std::move(candidate)));
        }
        context.reportProgress(95);
        context.setResultSummary(outcome->ok ? QStringLiteral("Action List executed.")
                                             : QStringLiteral("Action List execution finished."));
    };
}

}   // namespace pdfinteraction
