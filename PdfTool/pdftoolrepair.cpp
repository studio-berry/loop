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

#include "pdftoolrepair.h"

#include "pdftoolcancel.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentsession.h"
#include "pdfartifactstore.h"
#include "pdfgovernedexecution.h"
#include "pdfoperationhistorystore.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"
#include "pdfpreflightverdict.h"
#include "pdfpreflightcertificate.h"
#include "pdfsafefilewriter.h"

#include <algorithm>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTemporaryDir>

#include <optional>

namespace pdftool
{

namespace
{

class CancelControl final : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override
    {
        return isCancelRequested();
    }
};

static PDFToolRepair s_repairApplication;

bool readRepairDocument(const PDFToolOptions& options,
                        pdf::PDFDocument* document,
                        QByteArray* sourceData,
                        QString* error)
{
    pdf::PDFDocumentReader reader(nullptr, [&options](bool*)
                                  { return options.password; }, options.permissiveReading, false);
    *document = reader.readFromFile(options.repairFiles.first());
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        if (error)
        {
            *error = reader.getErrorMessage();
        }
        return false;
    }
    if (sourceData)
    {
        *sourceData = reader.getSource();
    }
    return true;
}

bool parseParameter(const QString& assignment, QString* key, QJsonValue* value, QString* error)
{
    const int separator = assignment.indexOf(QLatin1Char('='));
    if (separator <= 0)
    {
        if (error)
        {
            *error = PDFToolTranslationContext::tr("Repair parameter '%1' must use key=value.").arg(assignment);
        }
        return false;
    }

    const QString parameterKey = assignment.left(separator).trimmed();
    const QString parameterValue = assignment.mid(separator + 1).trimmed();
    if (parameterKey.isEmpty())
    {
        if (error)
        {
            *error = PDFToolTranslationContext::tr("Repair parameter keys may not be empty.");
        }
        return false;
    }

    if (parameterValue.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
    {
        *value = true;
    }
    else if (parameterValue.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
    {
        *value = false;
    }
    else if (parameterValue.compare(QStringLiteral("null"), Qt::CaseInsensitive) == 0)
    {
        *value = QJsonValue(QJsonValue::Null);
    }
    else
    {
        bool integerOk = false;
        const qlonglong integer = parameterValue.toLongLong(&integerOk);
        if (integerOk)
        {
            *value = integer;
        }
        else
        {
            bool realOk = false;
            const double real = parameterValue.toDouble(&realOk);
            *value = realOk ? QJsonValue(real) : QJsonValue(parameterValue);
        }
    }

    *key = parameterKey;
    return true;
}

bool parseParameters(const QStringList& assignments, QJsonObject* parameters, QString* error)
{
    for (const QString& assignment : assignments)
    {
        QString key;
        QJsonValue value;
        if (!parseParameter(assignment, &key, &value, error))
        {
            return false;
        }
        if (parameters->contains(key))
        {
            if (error)
            {
                *error = PDFToolTranslationContext::tr("Repair parameter '%1' was assigned more than once.").arg(key);
            }
            return false;
        }
        parameters->insert(key, value);
    }
    return true;
}

QJsonArray plansJson(const QList<pdf::PDFRepairPlan>& plans)
{
    QJsonArray result;
    for (const pdf::PDFRepairPlan& plan : plans)
    {
        result.append(plan.toJson());
    }
    return result;
}

QJsonArray resultsJson(const QList<pdf::PDFRepairResult>& results)
{
    QJsonArray result;
    for (const pdf::PDFRepairResult& repairResult : results)
    {
        result.append(repairResult.toJson());
    }
    return result;
}

bool writeJsonReport(const QString& path, const QJsonObject& report, QString* error)
{
    const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeData(
        path,
        QJsonDocument(report).toJson(QJsonDocument::Indented),
        pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!writeResult)
    {
        if (error)
        {
            *error = writeResult.getErrorMessage();
        }
        return false;
    }
    return true;
}

void writeRepairReportIfRequested(const PDFToolOptions& options, const QJsonObject& reportJson)
{
    if (!options.repairReportFile.isEmpty())
    {
        QString reportError;
        writeJsonReport(options.repairReportFile, reportJson, &reportError);
    }
}

}   // namespace

QString PDFToolRepair::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("repair");
        case Name:
            return PDFToolTranslationContext::tr("Repair");
        case Description:
            return PDFToolTranslationContext::tr("Plan, preview, validate, and atomically commit a registered PDF repair operation.");
    }
    return QString();
}

PDFToolExitCode PDFToolRepair::execute(const PDFToolOptions& options)
{
    if (options.repairListOperations)
    {
        const QJsonObject data{
            { QStringLiteral("ok"), true },
            { QStringLiteral("command"), QStringLiteral("repair") },
            { QStringLiteral("operations"), pdf::PDFRepairRegistry::instance().descriptors() }
        };
        if (options.executionContext)
        {
            options.executionContext->setData(data);
        }
        if (options.outputStyle != PDFOutputFormatter::Style::Json)
        {
            PDFConsole::writeText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)), options.outputCodec);
        }
        return PDFToolExitCode::Success;
    }

    if (options.repairFiles.size() != 1 || options.repairOperationId.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("repair requires one input PDF and --operation <id>."));
        return PDFToolExitCode::InvalidInvocation;
    }

    const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(options.repairOperationId);
    if (!operation)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.operation-unsupported"),
                         PDFToolTranslationContext::tr("Repair operation '%1' is not registered.").arg(options.repairOperationId),
                         QJsonObject{ { QStringLiteral("operation"), options.repairOperationId } });
        return PDFToolExitCode::InputError;
    }

    if (options.repairOutputDocument.isEmpty() && !options.destructiveDryRun)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("A repair output path is required unless --dry-run is used."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject parameters;
    QString parameterError;
    if (!parseParameters(options.repairParameterAssignments, &parameters, &parameterError))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), parameterError);
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFDocument source;
    QByteArray sourceData;
    QString readError;
    if (!readRepairDocument(options, &source, &sourceData, &readError))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.document-unreadable"), readError,
                         QJsonObject{ { QStringLiteral("path"), options.repairFiles.first() } });
        return PDFToolExitCode::InputError;
    }

    CancelControl cancelControl;
    pdf::PDFRepairTransactionOptions transactionOptions;
    transactionOptions.operationControl = &cancelControl;
    // The trusted input the caller received: a candidate write resolving to
    // this path is refused by the save contract.
    transactionOptions.sourcePath = options.repairFiles.first();
    pdf::PDFRepairTransaction transaction(source, transactionOptions);
    if (const pdf::PDFOperationResult addResult = transaction.add(operation, parameters); !addResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.plan-failed"), addResult.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }
    if (const pdf::PDFOperationResult analyzeResult = transaction.analyze(); !analyzeResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.plan-failed"), analyzeResult.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }
    if (transaction.status() == pdf::PDFRepairStatus::Unsupported)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.operation-unsupported"),
                         PDFToolTranslationContext::tr("The requested repair is unsupported for this document."));
        return PDFToolExitCode::Findings;
    }

    const QString sourceSha256 = QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex());
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), sourceSha256, transaction.savePolicy());

    QJsonObject reportJson{
        { QStringLiteral("schema"), QStringLiteral("loop.repair-operation") },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("command"), QStringLiteral("repair") },
        { QStringLiteral("operation"), options.repairOperationId },
        { QStringLiteral("input"), QJsonObject{
                                       { QStringLiteral("path"), options.repairFiles.first() },
                                       { QStringLiteral("sha256"), sourceSha256 } } },
        { QStringLiteral("plan_digest"), planDigest },
        { QStringLiteral("plans"), plansJson(transaction.plans()) },
        { QStringLiteral("results"), resultsJson(transaction.results()) }
    };

    if (options.destructiveDryRun)
    {
        reportJson.insert(QStringLiteral("status"), QStringLiteral("planned"));
        if (!options.repairReportFile.isEmpty())
        {
            if (const PDFToolExitCode blocked = validateDestructiveOutput(options, options.repairReportFile); blocked != PDFToolExitCode::Success)
            {
                return blocked;
            }
            QString reportError;
            if (!writeJsonReport(options.repairReportFile, reportJson, &reportError))
            {
                reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), reportError);
                return PDFToolExitCode::ProcessingFailure;
            }
        }
        if (options.executionContext)
        {
            options.executionContext->setData(reportJson);
            if (!options.repairReportFile.isEmpty())
            {
                options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("report"), options.repairReportFile, QStringLiteral("written") });
            }
        }
        if (options.outputStyle != PDFOutputFormatter::Style::Json)
        {
            PDFConsole::writeText(QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented)), options.outputCodec);
        }
        return PDFToolExitCode::Success;
    }

    if (const pdf::PDFOperationResult applyResult = transaction.apply(); !applyResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.apply-failed"), applyResult.getErrorMessage());
        return transaction.status() == pdf::PDFRepairStatus::Cancelled ? PDFToolExitCode::Cancelled : PDFToolExitCode::ProcessingFailure;
    }
    reportJson.insert(QStringLiteral("results"), resultsJson(transaction.results()));
    reportJson.insert(QStringLiteral("independent_validation"), transaction.artifactValidation());

    QTemporaryDir candidateDirectory;
    if (!candidateDirectory.isValid())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.candidate-directory-failed"),
                         PDFToolTranslationContext::tr("Unable to create an isolated repair candidate directory."));
        return PDFToolExitCode::ProcessingFailure;
    }
    if (!options.repairRenderDirectory.isEmpty() && !QDir().mkpath(options.repairRenderDirectory))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.artifact-directory-failed"),
                         PDFToolTranslationContext::tr("Unable to create the repair artifact directory."));
        return PDFToolExitCode::ProcessingFailure;
    }

    const QString candidatePath = QDir(candidateDirectory.path()).filePath(QStringLiteral("candidate.pdf"));
    pdf::PDFRepairDiffOptions diffOptions;
    diffOptions.renderDirectory = options.repairRenderDirectory;
    diffOptions.operationControl = &cancelControl;
    pdf::PDFTechnicalPreview technicalPreview;
    if (const pdf::PDFOperationResult technicalResult = pdf::buildTechnicalPreview(transaction, candidatePath, planDigest, &technicalPreview); !technicalResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.preview-failed"), technicalResult.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }
    pdf::PDFVisualPreview visualPreview;
    if (pdf::repairPlansMutatePageContent(transaction.plans()))
    {
        if (const pdf::PDFOperationResult visualResult = pdf::buildVisualPreview(transaction, candidatePath, planDigest, diffOptions, &visualPreview); !visualResult)
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.preview-failed"), visualResult.getErrorMessage());
            return PDFToolExitCode::ProcessingFailure;
        }
    }

    pdf::PDFDocument candidateDocument;
    QByteArray candidateData;
    {
        pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                      { return QString(); }, false, false);
        candidateDocument = reader.readFromFile(candidatePath);
        candidateData = reader.getSource();
        if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.candidate-unreadable"), reader.getErrorMessage());
            return PDFToolExitCode::ProcessingFailure;
        }
    }

    QJsonObject governedProfile;
    if (!options.preflightProfilePath.isEmpty())
    {
        QString profileError;
        if (!pdf::PreflightEngine::loadProfile(options.preflightProfilePath, governedProfile, profileError))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight.profile-invalid"), profileError);
            return PDFToolExitCode::InvalidInvocation;
        }
        if (!transaction.postflightRequired())
        {
            reportJson.insert(QStringLiteral("status"), QStringLiteral("incomplete"));
            pdf::PDFRepairFindingDelta missingPostflight;
            missingPostflight.incompleteFindingIds.append(QStringLiteral("postflight-not-required-by-plan"));
            reportJson.insert(QStringLiteral("finding_delta"), missingPostflight.toJson());
            writeRepairReportIfRequested(options, reportJson);
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.postflight-required"),
                             PDFToolTranslationContext::tr("Repair publication requires a plan that declares postflight validation."));
            return PDFToolExitCode::PartialOutput;
        }
    }

    pdf::PreflightResult postflight;
    const pdf::PDFOperationResult verified = transaction.validateCandidate(options.preflightProfilePath,
                                                                           &postflight,
                                                                           governedProfile);
    reportJson.insert(QStringLiteral("results"), resultsJson(transaction.results()));
    reportJson.insert(QStringLiteral("independent_validation"), transaction.artifactValidation());
    if (options.preflightProfilePath.isEmpty())
    {
        reportJson.insert(QStringLiteral("postflight"), QJsonObject{
                                                            { QStringLiteral("status"), QStringLiteral("not-run") },
                                                            { QStringLiteral("reason"), QStringLiteral("no-profile-supplied") } });
    }
    else
    {
        reportJson.insert(QStringLiteral("postflight"), postflight.toJson(candidatePath));
    }
    const pdf::PreflightVerdict canonicalVerdict = pdf::reducePreflightVerdict(postflight);
    // The canonical transaction validation is the only repair inspection; its
    // per-result finding delta is reported here rather than from a second run.
    pdf::PDFRepairFindingDelta automaticFindingDelta;
    if (!transaction.results().isEmpty())
    {
        automaticFindingDelta = transaction.results().first().findingDelta;
    }
    reportJson.insert(QStringLiteral("finding_delta"), automaticFindingDelta.toJson());
    reportJson.insert(QStringLiteral("postflight_verdict"), canonicalVerdict.toJson());
    if (!verified || transaction.status() != pdf::PDFRepairStatus::Passed || !canonicalVerdict.isPass())
    {
        const pdf::PDFRepairStatus status = transaction.status();
        const bool incomplete = status == pdf::PDFRepairStatus::Incomplete ||
                                canonicalVerdict.state == pdf::PreflightVerdictState::Incomplete;
        reportJson.insert(QStringLiteral("status"), incomplete ? QStringLiteral("incomplete") : QStringLiteral("failed"));
        writeRepairReportIfRequested(options, reportJson);
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.declared-validation-failed"),
                         verified.getErrorMessage().isEmpty()
                             ? PDFToolTranslationContext::tr("A declared repair validator did not complete; no output was committed.")
                             : verified.getErrorMessage());
        return incomplete ? PDFToolExitCode::PartialOutput : PDFToolExitCode::Findings;
    }

    reportJson.insert(QStringLiteral("technical_preview"), technicalPreview.toJson());
    reportJson.insert(QStringLiteral("visual_preview"), visualPreview.toJson());
    const bool previewIncomplete = technicalPreview.status == pdf::PDFRepairDiffStatus::Incomplete ||
                                   visualPreview.status == pdf::PDFRepairDiffStatus::Incomplete;
    if (previewIncomplete)
    {
        reportJson.insert(QStringLiteral("status"), QStringLiteral("incomplete"));
        writeRepairReportIfRequested(options, reportJson);
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.preview-incomplete"),
                         PDFToolTranslationContext::tr("The repair preview is incomplete; no output was committed."));
        return PDFToolExitCode::PartialOutput;
    }
    const int unexpectedStructuralChanges = std::count_if(technicalPreview.structuralChanges.cbegin(),
                                                          technicalPreview.structuralChanges.cend(),
                                                          [](const pdf::PDFRepairStructuralChange& change)
                                                          { return change.classification == pdf::PDFRepairChangeClass::Unexpected; });
    const int unexpectedVisualPages = std::count_if(visualPreview.pages.cbegin(),
                                                    visualPreview.pages.cend(),
                                                    [](const pdf::PDFRepairPageVisualDiff& page)
                                                    { return page.unexpectedChangedPixelCount > 0; });
    if (unexpectedStructuralChanges > 0 || unexpectedVisualPages > 0)
    {
        reportJson.insert(QStringLiteral("status"), QStringLiteral("failed"));
        writeRepairReportIfRequested(options, reportJson);
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.unexpected-change"),
                         PDFToolTranslationContext::tr("The repair candidate contains unexpected changes; no output was committed."));
        return PDFToolExitCode::Findings;
    }

    QStringList plannedOutputs{ options.repairOutputDocument };
    if (!options.repairReportFile.isEmpty())
    {
        plannedOutputs.append(options.repairReportFile);
    }
    if (const PDFToolExitCode blocked = validateDestructiveOutputs(options, plannedOutputs); blocked != PDFToolExitCode::Success)
    {
        return blocked;
    }
    // Repair never appends in place, so the publish destination is held to the
    // transaction's merged save policy before any publication side effect.
    if (const PDFToolExitCode refused = validateOperationSaveRequest(options,
                                                                     options.repairFiles.first(),
                                                                     options.repairOutputDocument,
                                                                     transaction.savePolicy(),
                                                                     /*appendInPlace=*/false);
        refused != PDFToolExitCode::Success)
    {
        return refused;
    }
    if (pdf::PDFOperationControl::isOperationCancelled(&cancelControl))
    {
        return PDFToolExitCode::Cancelled;
    }

    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256).toHex());
    // Bind the approval to the effective profile in scope for this run. The
    // revalidation recomputes the same digest from the same profile object.
    const QString expectedProfileDigest = governedProfile.isEmpty() ? QString() : pdf::computeProfileDigest(governedProfile);
    pdf::PDFApprovalAuthorizationContext authorizationContext;
    authorizationContext.evaluatedUtc = QDateTime::currentDateTimeUtc();
    authorizationContext.expectedProfileDigest = expectedProfileDigest;
    pdf::PDFGovernedExecutionApproval governedApproval;
    if (!options.repairApprovalFile.isEmpty())
    {
        QFile approvalFile(options.repairApprovalFile);
        if (!approvalFile.open(QIODevice::ReadOnly))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.approval-unreadable"),
                             PDFToolTranslationContext::tr("Could not read the governed approval file."));
            return PDFToolExitCode::InvalidInvocation;
        }
        const QJsonObject approvalObject = QJsonDocument::fromJson(approvalFile.readAll()).object();
        QString approvalError;
        governedApproval = pdf::PDFGovernedExecutionApproval::fromJson(approvalObject, &approvalError);
        if (!governedApproval.isValid())
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.approval-invalid"), approvalError);
            return PDFToolExitCode::InvalidInvocation;
        }
        // The identity and authorization of a supplied approval are decided by the
        // gateway, so the same rule holds on every surface.
    }
    else
    {
        governedApproval.planDigest = planDigest;
        governedApproval.sourceSha256 = sourceSha256;
        governedApproval.candidateSha256 = candidateSha256;
        governedApproval.effectiveProfileDigest = expectedProfileDigest;
        governedApproval.approval.kind = pdf::PDFApprovalKind::Policy;
        governedApproval.approval.actorId = QStringLiteral("PdfTool");
        governedApproval.approval.decision = QStringLiteral("approve");
        governedApproval.approval.policyId = QStringLiteral("preflight-profile");
        governedApproval.approval.rationale = QStringLiteral("Repair candidate passed governed previews and postflight.");
        governedApproval.approval.evidenceSha256 = planDigest;
        governedApproval.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    }

    const QString historyDirectory = QFileInfo(options.repairOutputDocument).absoluteFilePath() + QStringLiteral(".loop-history");
    pdf::PDFArtifactStore historyArtifacts(historyDirectory);
    const auto historyInput = historyArtifacts.importBytes(sourceData,
                                                           { QStringLiteral("application/pdf"), QStringLiteral("original-input.pdf") });
    const auto historyOutput = historyArtifacts.importBytes(candidateData,
                                                            { QStringLiteral("application/pdf"), QStringLiteral("candidate-output.pdf") });
    if (!historyInput.success || !historyOutput.success)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.artifact-failed"),
                         historyInput.success ? historyOutput.errorMessage : historyInput.errorMessage);
        return PDFToolExitCode::ProcessingFailure;
    }
    pdf::PDFOperationHistoryStore operationHistory(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString historyError;
    if (!operationHistory.open(&historyError) ||
        !operationHistory.registerOriginalInput(historyInput.artifact) ||
        !operationHistory.registerArtifact(historyOutput.artifact))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.open-failed"),
                         historyError.isEmpty() ? QStringLiteral("Could not register the repair history artifacts.") : historyError);
        return PDFToolExitCode::ProcessingFailure;
    }
    // The output chain resolves revocation of the approval that authorized it.
    authorizationContext.history = &operationHistory;
    // A certificate lives in the certified document's own chain - the same chain
    // verify-certificate reads - so the retained certificate is read from the input
    // document's history and its invalidation is recorded there. The repair's own
    // events belong to the output document's history, which never holds the
    // certificate that the fix is superseding.
    const QString certifiedHistoryDirectory =
        QFileInfo(options.repairFiles.first()).absoluteFilePath() + QStringLiteral(".loop-history");
    const bool certifiedHistoryIsOutput = certifiedHistoryDirectory == historyDirectory;
    pdf::PDFOperationHistoryStore certifiedHistory(QDir(certifiedHistoryDirectory).filePath(QStringLiteral("history.sqlite3")));
    pdf::PDFOperationHistoryStore& retainedHistory = certifiedHistoryIsOutput ? operationHistory : certifiedHistory;

    QString retainedCertificateError;
    const bool hasRetainedHistory = certifiedHistoryIsOutput || QFileInfo(retainedHistory.databasePath()).exists();
    if (hasRetainedHistory && !certifiedHistoryIsOutput && !certifiedHistory.open(&retainedCertificateError))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.open-failed"), retainedCertificateError);
        return PDFToolExitCode::ProcessingFailure;
    }
    const QList<pdf::PDFOperationHistoryEvent> priorHistory =
        hasRetainedHistory ? retainedHistory.events(&retainedCertificateError)
                           : QList<pdf::PDFOperationHistoryEvent>();
    const std::optional<pdf::PreflightCertificate> priorCertificate =
        retainedCertificateError.isEmpty()
            ? pdf::latestPreflightCertificate(priorHistory, &retainedCertificateError)
            : std::nullopt;
    if (!retainedCertificateError.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.read-failed"), retainedCertificateError);
        return PDFToolExitCode::ProcessingFailure;
    }

    // One cancellation-safe gateway call owns staging, revalidation, sign-off, the
    // atomic commit, read-back, and the canonical chain append. Every refusal or
    // failure before the commit leaves the destination untouched.
    pdf::PDFGovernedMutationRequest mutation;
    mutation.approval = governedApproval;
    mutation.authorization = authorizationContext;
    mutation.planDigest = planDigest;
    mutation.sourceSha256 = sourceSha256;
    mutation.candidateBytes = candidateData;
    mutation.destinationPath = options.repairOutputDocument;
    mutation.overwritePolicy = pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite;
    mutation.profile = governedProfile;
    mutation.profileDigest = expectedProfileDigest;
    mutation.signOffActor = QStringLiteral("PdfTool");
    mutation.signOffPolicy = QStringLiteral("repair-postflight");
    mutation.operationControl = &cancelControl;
    mutation.history = &operationHistory;
    mutation.operationId = options.repairOperationId;
    mutation.inputArtifact = historyInput.artifact;
    mutation.parameters = parameters;
    mutation.outputArtifact = historyOutput.artifact;
    mutation.resultSummary = [&reportJson, &governedApproval, &options, &historyDirectory, &operationHistory, candidateSha256](const pdf::PDFGovernedMutationReceipt& receipt)
    {
        reportJson.insert(QStringLiteral("status"), QStringLiteral("passed"));
        reportJson.insert(QStringLiteral("approval"), governedApproval.toJson());
        reportJson.insert(QStringLiteral("revalidation"), receipt.revalidation.toJson());
        reportJson.insert(QStringLiteral("sign_off"), receipt.signOff.toJson());
        reportJson.insert(QStringLiteral("output"), QJsonObject{
                                                        { QStringLiteral("path"), options.repairOutputDocument },
                                                        { QStringLiteral("sha256"), candidateSha256 } });
        reportJson.insert(QStringLiteral("history"), QJsonObject{
                                                         { QStringLiteral("sidecar"), historyDirectory },
                                                         { QStringLiteral("database"), operationHistory.databasePath() } });
        reportJson.insert(QStringLiteral("receipt"), receipt.toJson());
        return reportJson;
    };

    pdf::PDFGovernedMutationReceipt mutationReceipt;
    const pdf::PDFOperationResult mutationResult = pdf::executeGovernedMutation(mutation, &mutationReceipt);
    if (!mutationResult)
    {
        reportJson.insert(QStringLiteral("receipt"), mutationReceipt.toJson());
        reportJson.insert(QStringLiteral("revalidation"), mutationReceipt.revalidation.toJson());
        reportJson.insert(QStringLiteral("status"), QStringLiteral("failed"));
        writeRepairReportIfRequested(options, reportJson);
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         mutationReceipt.status == QStringLiteral("refused") ? QStringLiteral("repair.approval-invalid")
                                                                             : QStringLiteral("repair.output-write-failed"),
                         mutationResult.getErrorMessage(),
                         QJsonObject{ { QStringLiteral("path"), options.repairOutputDocument },
                                      { QStringLiteral("reason_code"), mutationReceipt.reasonCode } });
        if (mutationReceipt.status == QStringLiteral("refused"))
        {
            return PDFToolExitCode::InvalidInvocation;
        }
        if (mutationReceipt.status == QStringLiteral("cancelled"))
        {
            return PDFToolExitCode::Cancelled;
        }
        return PDFToolExitCode::ProcessingFailure;
    }

    if (priorCertificate.has_value() &&
        priorCertificate->documentRevisionDigest.compare(sourceSha256, Qt::CaseInsensitive) == 0 &&
        candidateSha256.compare(sourceSha256, Qt::CaseInsensitive) != 0)
    {
        // The invalidation is evidence in the certified document's chain, so it needs
        // its own execution there; the repair's execution belongs to the output chain.
        pdf::PDFOperationHistoryExecution invalidationExecution;
        invalidationExecution.operationId = options.repairOperationId;
        invalidationExecution.operationVersion = 1;
        invalidationExecution.input = historyInput.artifact;
        invalidationExecution.parameters = QJsonObject{
            { QStringLiteral("certificate_id"), priorCertificate->certificateId },
            { QStringLiteral("superseded_revision_digest"), sourceSha256 },
            { QStringLiteral("replacement_revision_digest"), candidateSha256 }
        };
        invalidationExecution.startedUtc = QDateTime::currentDateTimeUtc();
        QUuid invalidationExecutionId;

        pdf::PDFOperationHistoryEvent invalidated;
        invalidated.kind = pdf::PDFOperationHistoryEventKind::CertificateInvalidated;
        invalidated.status = pdf::PDFOperationHistoryStatus::Running;
        invalidated.operatorIdentity = QStringLiteral("PdfTool");
        invalidated.documentRevisionDigest = candidateSha256;
        invalidated.effectiveProfileDigest = priorCertificate->effectiveProfileDigest;
        invalidated.approval.decisionReference = priorCertificate->certificateId;
        invalidated.resultSummary = QJsonObject{
            { QStringLiteral("reason"), QStringLiteral("The certified document revision changed after a fix was applied.") },
            { QStringLiteral("previous_document_revision_digest"), sourceSha256 },
            { QStringLiteral("current_document_revision_digest"), candidateSha256 }
        };

        pdf::PDFOperationResult invalidationRecorded = retainedHistory.registerArtifact(historyInput.artifact);
        if (invalidationRecorded)
        {
            invalidationRecorded = retainedHistory.beginExecution(invalidationExecution, &invalidationExecutionId);
        }
        if (invalidationRecorded)
        {
            invalidated.executionId = invalidationExecutionId;
            invalidationRecorded = retainedHistory.appendEvent(invalidated);
        }
        if (!invalidationRecorded)
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.write-failed"),
                             QStringLiteral("The repair output was written, but certificate invalidation could not be persisted: ") + invalidationRecorded.getErrorMessage());
            return PDFToolExitCode::ProcessingFailure;
        }
    }

    if (!options.repairReportFile.isEmpty())
    {
        QString reportError;
        if (!writeJsonReport(options.repairReportFile, reportJson, &reportError))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), reportError);
            return PDFToolExitCode::ProcessingFailure;
        }
    }
    if (options.executionContext)
    {
        options.executionContext->setData(reportJson);
        options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("primary"), options.repairOutputDocument, QStringLiteral("written") });
        if (!options.repairReportFile.isEmpty())
        {
            options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("report"), options.repairReportFile, QStringLiteral("written") });
        }
        if (!options.repairRenderDirectory.isEmpty())
        {
            options.executionContext->addOutput({ QStringLiteral("directory"), QStringLiteral("artifacts"), options.repairRenderDirectory, QStringLiteral("written") });
        }
    }
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        PDFConsole::writeText(QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented)), options.outputCodec);
    }

    // Retention runs after the accepted output is fully reported so a retention failure
    // cannot suppress the repair report or output registration.
    const pdf::PDFHistoryRetentionResult retention = operationHistory.enforceRetention({}, historyArtifacts);
    if (!retention.success)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.retention-failed"),
                         QStringLiteral("The repair history was recorded, but retention could not be enforced: %1")
                             .arg(retention.errorMessage));
        return PDFToolExitCode::ProcessingFailure;
    }
    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolRepair::getOptionsFlags() const
{
    return ConsoleFormat | Repair | DestructiveWrite | PreflightProfile;
}

}   // namespace pdftool
