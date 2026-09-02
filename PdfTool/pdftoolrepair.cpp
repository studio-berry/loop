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
#include "pdfoperationhistorystore.h"
#include "preflightengine.h"
#include "pdfpreflightverdict.h"
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

namespace
{

void appendRepairHistoryFailed(pdf::PDFOperationHistoryStore& history,
                               const QUuid& executionId,
                               const QString& errorCode,
                               const QString& message)
{
    pdf::PDFOperationHistoryEvent historyFailed;
    historyFailed.executionId = executionId;
    historyFailed.status = pdf::PDFOperationHistoryStatus::Failed;
    historyFailed.resultSummary = QJsonObject{
        { QStringLiteral("error_code"), errorCode },
        { QStringLiteral("error"), message }
    };
    history.appendEvent(historyFailed);
}

}   // namespace

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

    QJsonObject reportJson{
        { QStringLiteral("schema"), QStringLiteral("loop.repair-operation") },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("command"), QStringLiteral("repair") },
        { QStringLiteral("operation"), options.repairOperationId },
        { QStringLiteral("input"), QJsonObject{
                                       { QStringLiteral("path"), options.repairFiles.first() },
                                       { QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex()) } } },
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
    diffOptions.renderVisualDiff = !pdf::repairPlansMutatePageContent(transaction.plans());
    pdf::PDFRepairDiffReport diffReport;
    if (const pdf::PDFOperationResult diffResult = transaction.compareCandidate(candidatePath, diffOptions, &diffReport); !diffResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.preview-failed"), diffResult.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
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

    if (!options.preflightProfilePath.isEmpty())
    {
        QJsonObject profile;
        QString profileError;
        if (!pdf::PreflightEngine::loadProfile(options.preflightProfilePath, profile, profileError))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight.profile-invalid"), profileError);
            return PDFToolExitCode::InvalidInvocation;
        }
        pdf::PDFDocumentSession session(&candidateDocument);
        const pdf::PreflightResult preflight = pdf::PreflightEngine(&session).run(profile);
        const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(preflight);
        reportJson.insert(QStringLiteral("postflight"), preflight.toJson(candidatePath));
        if (verdict.state == pdf::PreflightVerdictState::Incomplete)
        {
            reportJson.insert(QStringLiteral("status"), QStringLiteral("incomplete"));
            reportJson.insert(QStringLiteral("incomplete_reasons"), QJsonArray{ QStringLiteral("postflight-incomplete") });
            if (!options.repairAllowIncomplete)
            {
                writeRepairReportIfRequested(options, reportJson);
                reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.postflight-incomplete"),
                                 PDFToolTranslationContext::tr("Postflight did not inspect the complete candidate."));
                return PDFToolExitCode::PartialOutput;
            }
        }
        if (verdict.state == pdf::PreflightVerdictState::Error)
        {
            reportJson.insert(QStringLiteral("status"), QStringLiteral("error"));
            writeRepairReportIfRequested(options, reportJson);
            return PDFToolExitCode::PreflightError;
        }
        if (verdict.state == pdf::PreflightVerdictState::Fail)
        {
            reportJson.insert(QStringLiteral("status"), QStringLiteral("failed"));
            writeRepairReportIfRequested(options, reportJson);
            return PDFToolExitCode::Findings;
        }
    }
    else
    {
        reportJson.insert(QStringLiteral("postflight"), QJsonObject{
                                                            { QStringLiteral("status"), QStringLiteral("not-run") },
                                                            { QStringLiteral("reason"), QStringLiteral("no-profile-supplied") } });
        reportJson.insert(QStringLiteral("status"), QStringLiteral("incomplete"));
        writeRepairReportIfRequested(options, reportJson);
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.postflight-required"),
                         PDFToolTranslationContext::tr("A preflight --profile is required before a repair output can be committed."));
        return PDFToolExitCode::PartialOutput;
    }

    reportJson.insert(QStringLiteral("diff"), diffReport.toJson());
    if (diffReport.status == pdf::PDFRepairDiffStatus::Incomplete)
    {
        reportJson.insert(QStringLiteral("status"), QStringLiteral("incomplete"));
        if (!options.repairAllowIncomplete)
        {
            writeRepairReportIfRequested(options, reportJson);
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.preview-incomplete"),
                             PDFToolTranslationContext::tr("The repair preview is incomplete; no output was committed."));
            return PDFToolExitCode::PartialOutput;
        }
    }
    if (pdf::unexpectedChangeCount(diffReport) > 0)
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
    if (pdf::PDFOperationControl::isOperationCancelled(&cancelControl))
    {
        return PDFToolExitCode::Cancelled;
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
    pdf::PDFOperationHistoryExecution historyExecution;
    historyExecution.operationId = options.repairOperationId;
    historyExecution.operationVersion = 1;
    historyExecution.input = historyInput.artifact;
    historyExecution.parameters = parameters;
    QUuid historyExecutionId;
    if (!operationHistory.beginExecution(historyExecution, &historyExecutionId))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.open-failed"),
                         QStringLiteral("Could not begin the repair operation history."));
        return PDFToolExitCode::ProcessingFailure;
    }
    pdf::PDFOperationHistoryEvent historyRunning;
    historyRunning.executionId = historyExecutionId;
    historyRunning.kind = pdf::PDFOperationHistoryEventKind::FixApplied;
    historyRunning.status = pdf::PDFOperationHistoryStatus::Running;
    historyRunning.documentRevisionDigest = QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex());
    historyRunning.operatorIdentity = QStringLiteral("PdfTool");
    if (!operationHistory.appendEvent(historyRunning))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.write-failed"),
                         QStringLiteral("Could not append the repair history start event."));
        return PDFToolExitCode::ProcessingFailure;
    }

    const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeData(
        options.repairOutputDocument, candidateData, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!writeResult)
    {
        appendRepairHistoryFailed(operationHistory,
                                  historyExecutionId,
                                  QStringLiteral("output.write-failed"),
                                  writeResult.getErrorMessage());
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), writeResult.getErrorMessage(),
                         QJsonObject{ { QStringLiteral("path"), options.repairOutputDocument } });
        return PDFToolExitCode::ProcessingFailure;
    }

    QFile finalFile(options.repairOutputDocument);
    if (!finalFile.open(QIODevice::ReadOnly) || finalFile.readAll() != candidateData)
    {
        appendRepairHistoryFailed(operationHistory,
                                  historyExecutionId,
                                  QStringLiteral("repair.output-mismatch"),
                                  PDFToolTranslationContext::tr("The committed output does not match the reviewed candidate."));
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.output-mismatch"),
                         PDFToolTranslationContext::tr("The committed output does not match the reviewed candidate."));
        return PDFToolExitCode::ProcessingFailure;
    }
    finalFile.close();
    pdf::PDFDocumentReader finalReader(nullptr, [](bool*)
                                       { return QString(); }, false, false);
    finalReader.readFromFile(options.repairOutputDocument);
    if (finalReader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        appendRepairHistoryFailed(operationHistory,
                                  historyExecutionId,
                                  QStringLiteral("repair.output-unreadable"),
                                  PDFToolTranslationContext::tr("The committed repair output could not be reopened."));
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair.output-unreadable"),
                         PDFToolTranslationContext::tr("The committed repair output could not be reopened."),
                         QJsonObject{ { QStringLiteral("path"), options.repairOutputDocument } });
        return PDFToolExitCode::ProcessingFailure;
    }
    const QString outputSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256).toHex());
    reportJson.insert(QStringLiteral("status"), QStringLiteral("passed"));
    reportJson.insert(QStringLiteral("output"), QJsonObject{
                                                    { QStringLiteral("path"), options.repairOutputDocument },
                                                    { QStringLiteral("sha256"), outputSha256 } });
    reportJson.insert(QStringLiteral("history"), QJsonObject{
                                                     { QStringLiteral("sidecar"), historyDirectory },
                                                     { QStringLiteral("database"), operationHistory.databasePath() } });

    pdf::PDFOperationHistoryEvent historyAccepted;
    historyAccepted.executionId = historyExecutionId;
    historyAccepted.kind = pdf::PDFOperationHistoryEventKind::FixApplied;
    historyAccepted.status = pdf::PDFOperationHistoryStatus::Accepted;
    historyAccepted.output = historyOutput.artifact;
    historyAccepted.resultSummary = reportJson;
    historyAccepted.approval.kind = pdf::PDFApprovalKind::Policy;
    historyAccepted.approval.actorId = QStringLiteral("PdfTool");
    historyAccepted.approval.decision = QStringLiteral("approve");
    historyAccepted.approval.policyId = QStringLiteral("preflight-profile");
    historyAccepted.approval.rationale = QStringLiteral("Repair candidate passed postflight and diff validation.");
    historyAccepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    if (!operationHistory.appendEvent(historyAccepted))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.write-failed"),
                         QStringLiteral("The repair output was written, but its accepted history event could not be persisted."));
        return PDFToolExitCode::ProcessingFailure;
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
    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolRepair::getOptionsFlags() const
{
    return ConsoleFormat | Repair | DestructiveWrite | PreflightProfile;
}

}   // namespace pdftool
