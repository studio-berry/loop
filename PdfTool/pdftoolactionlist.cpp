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

#include "pdftoolactionlist.h"

#include "pdftoolcancel.h"
#include "pdfdocumentreader.h"
#include "pdfartifactstore.h"
#include "pdfgovernedexecution.h"
#include "pdfoperationhistorystore.h"
#include "pdfpreflightverdict.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"
#include "pdfsafefilewriter.h"
#include "pdfobjectselector.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDateTime>
#include <QTemporaryDir>

namespace pdftool
{

namespace
{

class ActionListCancelControl final : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override { return isCancelRequested(); }
};

bool readJsonFile(const QString& path, QJsonObject* object, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
            *error = QStringLiteral("Unable to read Action List recipe '%1'.").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error)
            *error = QStringLiteral("Action List recipe '%1' is not a JSON object: %2.").arg(path, parseError.errorString());
        return false;
    }
    *object = document.object();
    return true;
}

bool readBytesFile(const QString& path, QByteArray* bytes, QString* error)
{
    if (!bytes)
    {
        if (error)
            *error = QStringLiteral("Output bytes destination is null.");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
            *error = QStringLiteral("Unable to read serialized Action List output '%1'.").arg(path);
        return false;
    }
    *bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
    {
        if (error)
            *error = QStringLiteral("Unable to read serialized Action List output '%1'.").arg(path);
        return false;
    }
    return true;
}

bool parseBinding(const QString& assignment, QString* key, QJsonValue* value, QString* error)
{
    const int separator = assignment.indexOf(QLatin1Char('='));
    if (separator <= 0)
    {
        if (error)
            *error = QStringLiteral("Action List parameter '%1' must use key=value.").arg(assignment);
        return false;
    }
    *key = assignment.left(separator).trimmed();
    const QString text = assignment.mid(separator + 1).trimmed();
    if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
        *value = true;
    else if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
        *value = false;
    else if (text.compare(QStringLiteral("null"), Qt::CaseInsensitive) == 0)
        *value = QJsonValue(QJsonValue::Null);
    else
    {
        bool integerOk = false;
        const qlonglong integer = text.toLongLong(&integerOk);
        if (integerOk)
            *value = integer;
        else
        {
            bool numberOk = false;
            const double number = text.toDouble(&numberOk);
            *value = numberOk ? QJsonValue(number) : QJsonValue(text);
        }
    }
    return true;
}

bool parseBindings(const QStringList& assignments, QJsonObject* bindings, QString* error)
{
    for (const QString& assignment : assignments)
    {
        QString key;
        QJsonValue value;
        if (!parseBinding(assignment, &key, &value, error))
            return false;
        bindings->insert(key, value);
    }
    return true;
}

bool readDocumentFromPath(const PDFToolOptions& options, const QString& path, pdf::PDFDocument* document, QByteArray* sourceData, QString* error)
{
    pdf::PDFDocumentReader reader(nullptr, [&options](bool*)
                                  { return options.password; }, options.permissiveReading, false);
    *document = reader.readFromFile(path);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        if (error)
            *error = reader.getErrorMessage();
        return false;
    }
    if (sourceData)
        *sourceData = reader.getSource();
    return true;
}

QJsonObject resultWithInput(const pdf::PDFActionListExecutionResult& result,
                            const QString& input,
                            const QByteArray& inputData)
{
    QJsonObject object = result.toJson();
    object.insert(QStringLiteral("input"), QJsonObject{
                                               { QStringLiteral("path"), input },
                                               { QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(inputData, QCryptographicHash::Sha256).toHex()) } });
    return object;
}

PDFToolExitCode statusExitCode(const pdf::PDFActionListExecutionResult& result)
{
    if (result.status == QStringLiteral("cancelled"))
        return PDFToolExitCode::Cancelled;
    if (result.status == QStringLiteral("failed"))
        return PDFToolExitCode::ProcessingFailure;
    return PDFToolExitCode::Success;
}

bool recordActionListHistory(const QString& outputPath,
                             const QString& stagedCandidatePath,
                             const QByteArray& sourceData,
                             const QByteArray& candidateData,
                             const QString& operationId,
                             const QJsonObject& parameters,
                             const QString& planDigest,
                             const QJsonObject& preflightProfile,
                             QJsonObject* summary,
                             QString* error)
{
    if (!summary)
    {
        if (error)
            *error = QStringLiteral("Action List history summary is null.");
        return false;
    }
    const QString sourceSha256 = QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex());
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256).toHex());
    const QString historyDirectory = QFileInfo(outputPath).absoluteFilePath() + QStringLiteral(".loop-history");
    pdf::PDFArtifactStore artifacts(historyDirectory);
    const auto input = artifacts.importBytes(sourceData, { QStringLiteral("application/pdf"), QStringLiteral("original-input.pdf") });
    const auto output = artifacts.importBytes(candidateData, { QStringLiteral("application/pdf"), QStringLiteral("candidate-output.pdf") });
    if (!input.success || !output.success)
    {
        if (error)
            *error = input.success ? output.errorMessage : input.errorMessage;
        return false;
    }
    pdf::PDFOperationHistoryStore history(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString historyError;
    if (!history.open(&historyError) || !history.registerOriginalInput(input.artifact) || !history.registerArtifact(output.artifact))
    {
        if (error)
            *error = historyError.isEmpty() ? QStringLiteral("Could not register Action List history artifacts.") : historyError;
        return false;
    }

    const QString expectedProfileDigest = preflightProfile.isEmpty() ? QString() : pdf::computeProfileDigest(preflightProfile);
    pdf::PDFApprovalAuthorizationContext authorizationContext;
    authorizationContext.evaluatedUtc = QDateTime::currentDateTimeUtc();
    authorizationContext.expectedProfileDigest = expectedProfileDigest;
    authorizationContext.history = &history;

    pdf::PDFGovernedExecutionApproval governedApproval;
    governedApproval.planDigest = planDigest;
    governedApproval.sourceSha256 = sourceSha256;
    governedApproval.candidateSha256 = candidateSha256;
    governedApproval.effectiveProfileDigest = expectedProfileDigest;
    governedApproval.approval.kind = pdf::PDFApprovalKind::System;
    governedApproval.approval.actorId = QStringLiteral("PdfTool");
    governedApproval.approval.decision = QStringLiteral("approve");
    governedApproval.approval.policyId = QStringLiteral("action-list-plan");
    governedApproval.approval.rationale = QStringLiteral("Action List plan was validated before publication.");
    governedApproval.approval.evidenceSha256 = planDigest;
    governedApproval.approval.decisionReference = QStringLiteral("action-list-plan:%1").arg(planDigest);
    governedApproval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    const auto recordGovernedSummary = [summary, &governedApproval](const pdf::PDFGovernedMutationReceipt& receipt)
    {
        // P3: the governed summary carries an explicit status and reason code so
        // a consumer never has to infer signed-off vs incomplete vs error from
        // the raw receipt.
        const QString state = receipt.revalidation.state;
        QString status = QStringLiteral("error");
        if (receipt.isPublished())
        {
            status = receipt.signOff.isValid() ? QStringLiteral("signed-off") : QStringLiteral("not-certified");
        }
        else if (receipt.status == QStringLiteral("refused"))
        {
            status = QStringLiteral("refused");
        }
        else if (receipt.status == QStringLiteral("cancelled"))
        {
            status = QStringLiteral("cancelled");
        }
        else if (state == QStringLiteral("incomplete"))
        {
            status = QStringLiteral("incomplete");
        }
        const QString reasonCode = !receipt.reasonCode.isEmpty() ? receipt.reasonCode : receipt.revalidation.reasonCode;
        summary->insert(QStringLiteral("governed"), QJsonObject{
                                                        { QStringLiteral("status"), status },
                                                        { QStringLiteral("reason_code"), reasonCode },
                                                        { QStringLiteral("state"), state },
                                                        { QStringLiteral("approval"), governedApproval.toJson() },
                                                        { QStringLiteral("revalidation"), receipt.revalidation.toJson() },
                                                        { QStringLiteral("sign_off"), receipt.signOff.toJson() },
                                                        { QStringLiteral("receipt"), receipt.toJson() } });
        return *summary;
    };

    pdf::PDFGovernedMutationRequest mutation;
    mutation.approval = governedApproval;
    mutation.authorization = authorizationContext;
    mutation.planDigest = planDigest;
    mutation.sourceSha256 = sourceSha256;
    mutation.candidateBytes = candidateData;
    mutation.stagedCandidatePath = stagedCandidatePath;
    mutation.destinationPath = outputPath;
    mutation.overwritePolicy = pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite;
    mutation.profile = preflightProfile;
    mutation.profileDigest = expectedProfileDigest;
    mutation.signOffActor = QStringLiteral("PdfTool");
    mutation.signOffPolicy = QStringLiteral("action-list-postflight");
    mutation.history = &history;
    mutation.operationId = operationId;
    mutation.inputArtifact = input.artifact;
    mutation.parameters = parameters;
    mutation.outputArtifact = output.artifact;
    mutation.resultSummary = recordGovernedSummary;

    pdf::PDFGovernedMutationReceipt receipt;
    const pdf::PDFOperationResult mutationResult = pdf::executeGovernedMutation(mutation, &receipt);
    // Record the governed block even on a nonpublication receipt.
    recordGovernedSummary(receipt);
    if (!mutationResult)
    {
        if (error)
            *error = mutationResult.getErrorMessage();
        return false;
    }
    const pdf::PDFHistoryRetentionResult retention = history.enforceRetention({}, artifacts);
    if (!retention.success)
    {
        if (error)
            *error = QStringLiteral("Action List history was recorded, but retention could not be enforced: %1")
                         .arg(retention.errorMessage);
        return false;
    }
    return true;
}

}   // namespace

QString PDFToolActionList::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("action-list");
        case Name:
            return PDFToolTranslationContext::tr("Action List");
        case Description:
            return PDFToolTranslationContext::tr("Validate, plan, and execute reusable declarative Loop operations.");
    }
    return QString();
}

PDFToolExitCode PDFToolActionList::execute(const PDFToolOptions& options)
{
    const QString subcommand = options.actionListSubcommand;
    if (subcommand != QStringLiteral("validate") && subcommand != QStringLiteral("plan") &&
        subcommand != QStringLiteral("run") && subcommand != QStringLiteral("batch"))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         QStringLiteral("action-list requires validate, plan, run, or batch."));
        return PDFToolExitCode::InvalidInvocation;
    }
    if (options.actionListRecipe.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         QStringLiteral("An Action List recipe path is required."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject recipeObject;
    QString error;
    if (!readJsonFile(options.actionListRecipe, &recipeObject, &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.recipe-unreadable"), error);
        return PDFToolExitCode::InputError;
    }
    pdf::PDFActionList actionList;
    if (const pdf::PDFOperationResult parseResult = pdf::PDFActionList::fromJson(recipeObject, &actionList); !parseResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.recipe-invalid"), parseResult.getErrorMessage());
        return PDFToolExitCode::InvalidInvocation;
    }
    QJsonObject bindings;
    if (!parseBindings(options.actionListParameterAssignments, &bindings, &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), error);
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFActionListExecutor executor;
    pdf::PDFActionListExecutionOptions executionOptions;
    executionOptions.bindings = bindings;
    executionOptions.dryRun = options.destructiveDryRun;
    ActionListCancelControl cancelControl;
    executionOptions.operationControl = &cancelControl;
    const bool requiresPostflight = (subcommand == QStringLiteral("run") || subcommand == QStringLiteral("batch")) &&
                                    !options.destructiveDryRun;
    executionOptions.requirePostflight = requiresPostflight;
    QJsonObject governedProfile;
    if (requiresPostflight)
    {
        if (options.preflightProfilePath.isEmpty())
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.postflight-required"),
                             PDFToolTranslationContext::tr("A preflight --profile is required before an Action List output can be committed."));
            return PDFToolExitCode::PartialOutput;
        }
        executionOptions.preflightProfilePath = options.preflightProfilePath;
        if (!pdf::PreflightEngine::loadProfile(options.preflightProfilePath, governedProfile, error))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.profile-unreadable"), error);
            return PDFToolExitCode::InputError;
        }
        executionOptions.preflightProfile = governedProfile;
    }

    if (subcommand == QStringLiteral("validate"))
    {
        QStringList validationErrors;
        const pdf::PDFOperationResult validation = executor.validate(actionList, executionOptions, &validationErrors);
        const QJsonObject data{
            { QStringLiteral("schema"), QStringLiteral("loop-action-list-validation") },
            { QStringLiteral("recipe"), options.actionListRecipe },
            { QStringLiteral("action_list"), actionList.toJson() },
            { QStringLiteral("valid"), bool(validation) },
            { QStringLiteral("errors"), QJsonArray::fromStringList(validationErrors) }
        };
        if (options.executionContext)
            options.executionContext->setData(data);
        if (options.outputStyle != PDFOutputFormatter::Style::Json)
            PDFConsole::writeText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)), options.outputCodec);
        return validation ? PDFToolExitCode::Success : PDFToolExitCode::InvalidInvocation;
    }

    if (subcommand == QStringLiteral("batch"))
    {
        if (options.actionListFiles.isEmpty() || options.actionListOutputDirectory.isEmpty())
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                             QStringLiteral("action-list batch requires one or more input PDFs and --output-dir."));
            return PDFToolExitCode::InvalidInvocation;
        }
        if (!QDir().mkpath(options.actionListOutputDirectory))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.directory-create-failed"),
                             QStringLiteral("Unable to create batch output directory."));
            return PDFToolExitCode::ProcessingFailure;
        }
        QJsonArray items;
        PDFToolExitCode aggregateCode = PDFToolExitCode::Success;
        for (const QString& input : options.actionListFiles)
        {
            pdf::PDFDocument source;
            QByteArray sourceData;
            if (!readDocumentFromPath(options, input, &source, &sourceData, &error))
            {
                items.append(QJsonObject{ { QStringLiteral("input"), input }, { QStringLiteral("status"), QStringLiteral("failed") }, { QStringLiteral("error"), error } });
                aggregateCode = PDFToolExitCode::InputError;
                continue;
            }
            executionOptions = pdf::makeActionListExecutionOptions(source, bindings, &cancelControl);
            executionOptions.dryRun = options.destructiveDryRun;
            executionOptions.requirePostflight = requiresPostflight;
            executionOptions.preflightProfilePath = options.preflightProfilePath;
            executionOptions.preflightProfile = governedProfile;
            const QString output = QDir(options.actionListOutputDirectory).filePath(QFileInfo(input).completeBaseName() + QStringLiteral(".pdf"));
            pdf::PDFActionListExecutionResult executionResult;
            pdf::PDFDocument candidate;
            const pdf::PDFOperationResult executeResult = executor.execute(actionList, source, executionOptions, &candidate, &executionResult);
            QJsonObject item = resultWithInput(executionResult, input, sourceData);
            item.insert(QStringLiteral("output"), output);
            if (!executeResult)
            {
                aggregateCode = statusExitCode(executionResult);
            }
            else if (!options.destructiveDryRun)
            {
                const PDFToolExitCode outputCheck = validateDestructiveOutput(options, output);
                if (outputCheck != PDFToolExitCode::Success)
                {
                    aggregateCode = PDFToolExitCode::ProcessingFailure;
                }
                else if (const PDFToolExitCode refused = validateOperationSaveRequest(options,
                                                                                      input,
                                                                                      output,
                                                                                      executionResult.savePolicy,
                                                                                      /*appendInPlace=*/false);
                         refused != PDFToolExitCode::Success)
                {
                    aggregateCode = refused;
                    item.insert(QStringLiteral("status"), QStringLiteral("failed"));
                    item.insert(QStringLiteral("error"), QStringLiteral("Batch output violates the operation save policy."));
                }
                else
                {
                    QTemporaryDir stagingDirectory;
                    const QString stagedCandidatePath =
                        stagingDirectory.isValid() ? stagingDirectory.filePath(QStringLiteral("candidate.pdf")) : QString();
                    QByteArray candidateData;
                    pdf::PDFDocument reopened;
                    const pdf::PDFOperationResult serializeResult =
                        stagedCandidatePath.isEmpty()
                            ? pdf::PDFOperationResult(QStringLiteral("Unable to create a staging directory for the Action List output."))
                            : pdf::PDFStandardConversion::writeCandidate(candidate, stagedCandidatePath, executionResult.standardValidationRequirements, &reopened, &candidateData, &executionResult.independentValidation);
                    item.insert(QStringLiteral("independent_validation"), executionResult.independentValidation);
                    if (!serializeResult)
                    {
                        aggregateCode = PDFToolExitCode::ProcessingFailure;
                        item.insert(QStringLiteral("error"), serializeResult.getErrorMessage());
                    }
                    else if (!readBytesFile(stagedCandidatePath, &candidateData, &error))
                    {
                        aggregateCode = PDFToolExitCode::ProcessingFailure;
                        item.insert(QStringLiteral("error"), error);
                    }
                    else
                    {
                        QString historyError;
                        if (!recordActionListHistory(output,
                                                     stagedCandidatePath,
                                                     sourceData,
                                                     candidateData,
                                                     actionList.id,
                                                     QJsonObject{ { QStringLiteral("recipe"), options.actionListRecipe }, { QStringLiteral("bindings"), bindings } },
                                                     pdf::computeActionListPlanDigest(actionList,
                                                                                      bindings,
                                                                                      QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex()),
                                                                                      governedProfile,
                                                                                      executionResult.savePolicy),
                                                     governedProfile,
                                                     &item,
                                                     &historyError))
                        {
                            aggregateCode = PDFToolExitCode::ProcessingFailure;
                            item.insert(QStringLiteral("error"), historyError);
                        }
                    }
                }
            }
            items.append(std::move(item));
        }
        const QJsonObject data{ { QStringLiteral("schema"), QStringLiteral("loop-action-list-batch") }, { QStringLiteral("recipe"), actionList.id }, { QStringLiteral("items"), items } };
        if (options.executionContext)
            options.executionContext->setData(data);
        if (options.outputStyle != PDFOutputFormatter::Style::Json)
            PDFConsole::writeText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)), options.outputCodec);
        return aggregateCode;
    }

    if (options.actionListFiles.size() != 1)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         QStringLiteral("action-list plan/run requires exactly one input PDF."));
        return PDFToolExitCode::InvalidInvocation;
    }
    pdf::PDFDocument source;
    QByteArray sourceData;
    if (!readDocumentFromPath(options, options.actionListFiles.first(), &source, &sourceData, &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.document-unreadable"), error);
        return PDFToolExitCode::InputError;
    }
    executionOptions = pdf::makeActionListExecutionOptions(source, bindings, &cancelControl);
    executionOptions.dryRun = options.destructiveDryRun;
    executionOptions.requirePostflight = requiresPostflight;
    executionOptions.preflightProfilePath = options.preflightProfilePath;
    executionOptions.preflightProfile = governedProfile;

    pdf::PDFActionListExecutionResult executionResult;
    pdf::PDFDocument candidate;
    pdf::PDFOperationResult execution(false);
    if (subcommand == QStringLiteral("plan"))
    {
        execution = executor.plan(actionList, source, executionOptions, &executionResult);
    }
    else
    {
        execution = executor.execute(actionList, source, executionOptions, &candidate, &executionResult);
    }
    QJsonObject data = resultWithInput(executionResult, options.actionListFiles.first(), sourceData);
    if (execution && subcommand == QStringLiteral("run") && !options.destructiveDryRun)
    {
        if (options.actionListOutputDocument.isEmpty())
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                             QStringLiteral("action-list run requires --output unless --dry-run is used."));
            return PDFToolExitCode::InvalidInvocation;
        }
        // The operation-declared policy is the floor for the write: a candidate
        // resolving to the trusted input is refused by the save contract before
        // the destination is touched.
        if (const PDFToolExitCode refused = validateOperationSaveRequest(options,
                                                                         options.actionListFiles.first(),
                                                                         options.actionListOutputDocument,
                                                                         executionResult.savePolicy,
                                                                         /*appendInPlace=*/false);
            refused != PDFToolExitCode::Success)
        {
            return refused;
        }
        const PDFToolExitCode outputCheck = validateDestructiveOutput(options, options.actionListOutputDocument);
        if (outputCheck != PDFToolExitCode::Success)
            return outputCheck;
        QTemporaryDir stagingDirectory;
        const QString stagedCandidatePath =
            stagingDirectory.isValid() ? stagingDirectory.filePath(QStringLiteral("candidate.pdf")) : QString();
        QByteArray candidateData;
        pdf::PDFDocument reopened;
        if (stagedCandidatePath.isEmpty())
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.output-serialize-failed"),
                             QStringLiteral("Unable to create a staging directory for the Action List output."));
            return PDFToolExitCode::ProcessingFailure;
        }
        if (const pdf::PDFOperationResult serializeResult = pdf::PDFStandardConversion::writeCandidate(candidate, stagedCandidatePath, executionResult.standardValidationRequirements, &reopened, &candidateData, &executionResult.independentValidation);
            !serializeResult)
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.output-serialize-failed"), serializeResult.getErrorMessage());
            return PDFToolExitCode::ProcessingFailure;
        }
        data.insert(QStringLiteral("independent_validation"), executionResult.independentValidation);
        if (!readBytesFile(stagedCandidatePath, &candidateData, &error))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.output-read-failed"), error);
            return PDFToolExitCode::ProcessingFailure;
        }
        data.insert(QStringLiteral("output"), QJsonObject{ { QStringLiteral("path"), options.actionListOutputDocument }, { QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256).toHex()) } });
        QString historyError;
        if (!recordActionListHistory(options.actionListOutputDocument,
                                     stagedCandidatePath,
                                     sourceData,
                                     candidateData,
                                     actionList.id,
                                     QJsonObject{ { QStringLiteral("recipe"), options.actionListRecipe }, { QStringLiteral("bindings"), bindings } },
                                     pdf::computeActionListPlanDigest(actionList,
                                                                      bindings,
                                                                      QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex()),
                                                                      governedProfile,
                                                                      executionResult.savePolicy),
                                     governedProfile,
                                     &data,
                                     &historyError))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.write-failed"), historyError);
            return PDFToolExitCode::ProcessingFailure;
        }
        if (options.executionContext)
            options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("primary"), options.actionListOutputDocument, QStringLiteral("written") });
    }
    if (options.executionContext)
        options.executionContext->setData(data);
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
        PDFConsole::writeText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)), options.outputCodec);
    return execution ? PDFToolExitCode::Success : statusExitCode(executionResult);
}

PDFToolAbstractApplication::Options PDFToolActionList::getOptionsFlags() const
{
    return ConsoleFormat | ActionList | DestructiveWrite | PreflightProfile;
}

static PDFToolActionList s_actionListApplication;

}   // namespace pdftool
