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
#include "pdfartifactstore.h"
#include "pdfoperationhistorystore.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QUuid>
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

bool retainHistoryForArtifact(const QString& sourcePath, const QString& destinationPath, QString* error)
{
    const QString sourceRoot = QFileInfo(sourcePath).absoluteFilePath() + QStringLiteral(".loop-history");
    const QString destinationRoot = QFileInfo(destinationPath).absoluteFilePath() + QStringLiteral(".loop-history");
    const QString databasePath = QDir(sourceRoot).filePath(QStringLiteral("history.sqlite3"));
    if (!QFileInfo::exists(databasePath))
        return true;
    if (QFileInfo::exists(destinationRoot))
    {
        *error = QStringLiteral("The new revision history destination already exists.");
        return false;
    }
    pdf::PDFOperationHistoryStore source(databasePath);
    if (!source.open(error))
        return false;
    const auto verification = source.verify();
    source.close();
    if (!verification.verified)
    {
        *error = verification.errorMessage;
        return false;
    }
    if (QFileInfo(databasePath + QStringLiteral("-wal")).size() > 0)
    {
        *error = QStringLiteral("History is in use; retry when its writer has finished.");
        return false;
    }
    QTemporaryDir staging(QFileInfo(destinationPath).absoluteDir().filePath(QStringLiteral(".loop-history-XXXXXX")));
    if (!staging.isValid())
    {
        *error = QStringLiteral("Could not stage the retained history.");
        return false;
    }
    QDirIterator files(sourceRoot, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (files.hasNext())
    {
        const QString file = files.next();
        const QString relative = QDir(sourceRoot).relativeFilePath(file);
        if (relative.endsWith(QStringLiteral("-wal")) || relative.endsWith(QStringLiteral("-shm")))
            continue;
        const QString target = QDir(staging.path()).filePath(relative);
        if (files.fileInfo().isSymLink() || !QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(file, target))
        {
            *error = QStringLiteral("Could not retain history artifact %1.").arg(relative);
            return false;
        }
    }
    pdf::PDFOperationHistoryStore snapshot(QDir(staging.path()).filePath(QStringLiteral("history.sqlite3")));
    if (!snapshot.open(error))
        return false;
    const auto copied = snapshot.verify();
    snapshot.close();
    if (!copied.verified || !QDir().rename(staging.path(), destinationRoot))
    {
        *error = QStringLiteral("The retained history snapshot could not be verified or published.");
        return false;
    }
    return true;
}

ActionListRunWorker makeActionListRunWorker(ActionListRunPhase phase,
                                            pdf::PDFActionList actionList,
                                            pdf::PDFDocumentPointer document,
                                            QJsonObject bindings,
                                            std::shared_ptr<ActionListWorkerOutcome> outcome,
                                            QString preflightProfilePath,
                                            QJsonObject preflightProfile,
                                            QJsonObject preflightProfileBindings,
                                            QString publicationSourcePath)
{
    return [phase, actionList = std::move(actionList), document = std::move(document), bindings = std::move(bindings), outcome = std::move(outcome), preflightProfilePath = std::move(preflightProfilePath), preflightProfile = std::move(preflightProfile), preflightProfileBindings = std::move(preflightProfileBindings), publicationSourcePath = std::move(publicationSourcePath)](pdf::PDFJobContext& context)
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
        if (!security || !security->isAllowed(pdf::PDFSecurityHandler::Permission::CopyContent) ||
            !(security->isAllowed(pdf::PDFSecurityHandler::Permission::PrintLowResolution) ||
              security->isAllowed(pdf::PDFSecurityHandler::Permission::PrintHighResolution)) ||
            !security->isAllowed(pdf::PDFSecurityHandler::Permission::Modify) ||
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
                const QString publicationPath = publicationSourcePath.isEmpty()
                                                    ? publicationDirectory.filePath(QStringLiteral("published.pdf"))
                                                    : QFileInfo(publicationSourcePath).absoluteDir().filePath(QStringLiteral("%1-revision-%2.pdf").arg(QFileInfo(publicationSourcePath).completeBaseName().left(32), QUuid::createUuid().toString(QUuid::WithoutBraces)));
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
                        std::unique_ptr<pdf::PDFOperationHistoryStore> history;
                        if (!publicationSourcePath.isEmpty())
                        {
                            QFile sourceFile(publicationSourcePath);
                            if (!sourceFile.open(QIODevice::ReadOnly))
                                throw std::runtime_error("The governed source artifact cannot be read.");
                            const QByteArray sourceBytes = sourceFile.readAll();
                            if (QString::fromLatin1(QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256).toHex()) != approval.sourceSha256)
                                throw std::runtime_error("The governed source artifact changed before publication.");
                            QString historyError;
                            if (!retainHistoryForArtifact(publicationSourcePath, publicationPath, &historyError))
                                throw std::runtime_error(historyError.toStdString());
                            const QString historyRoot = publicationPath + QStringLiteral(".loop-history");
                            pdf::PDFArtifactStore artifacts(historyRoot);
                            const auto input = artifacts.importBytes(sourceBytes, { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
                            const auto output = artifacts.importBytes(candidateData, { QStringLiteral("application/pdf"), QStringLiteral("published.pdf") });
                            if (!input.success || !output.success)
                                throw std::runtime_error((input.success ? output.errorMessage : input.errorMessage).toStdString());
                            history = std::make_unique<pdf::PDFOperationHistoryStore>(QDir(historyRoot).filePath(QStringLiteral("history.sqlite3")));
                            if (!history->open(&historyError))
                                throw std::runtime_error(historyError.toStdString());
                            const auto registeredInput = history->rollbackPoints().isEmpty() ? history->registerOriginalInput(input.artifact)
                                                                                             : history->registerArtifact(input.artifact);
                            if (!registeredInput || !history->registerArtifact(output.artifact))
                                throw std::runtime_error("The governed history artifacts could not be registered.");
                            mutation.history = history.get();
                            mutation.authorization.history = history.get();
                            mutation.operationId = QStringLiteral("action-list.%1").arg(actionList.id);
                            mutation.inputArtifact = input.artifact;
                            mutation.outputArtifact = output.artifact;
                            mutation.parameters = QJsonObject{ { QStringLiteral("planDigest"), approval.planDigest } };
                            mutation.resultSummary = [approval](const pdf::PDFGovernedMutationReceipt& receipt)
                            {
                                return QJsonObject{ { QStringLiteral("governed"), QJsonObject{
                                                                                      { QStringLiteral("approval"), approval.toJson() },
                                                                                      { QStringLiteral("revalidation"), receipt.revalidation.toJson() },
                                                                                      { QStringLiteral("sign_off"), receipt.signOff.toJson() },
                                                                                      { QStringLiteral("receipt"), receipt.toJson() } } } };
                            };
                        }
                        pdf::PDFGovernedMutationReceipt receipt;
                        const pdf::PDFOperationResult governedResult = pdf::executeGovernedMutation(mutation, &receipt);
                        if (receipt.isPublished() && !publicationSourcePath.isEmpty())
                            outcome->publishedPath = publicationPath;
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
