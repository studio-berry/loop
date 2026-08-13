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
#include "pdfoperationhistorystore.h"
#include "pdfsafefilewriter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDateTime>

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
        if (error) *error = QStringLiteral("Unable to read Action List recipe '%1'.").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error) *error = QStringLiteral("Action List recipe '%1' is not a JSON object: %2.").arg(path, parseError.errorString());
        return false;
    }
    *object = document.object();
    return true;
}

bool parseBinding(const QString& assignment, QString* key, QJsonValue* value, QString* error)
{
    const int separator = assignment.indexOf(QLatin1Char('='));
    if (separator <= 0)
    {
        if (error) *error = QStringLiteral("Action List parameter '%1' must use key=value.").arg(assignment);
        return false;
    }
    *key = assignment.left(separator).trimmed();
    const QString text = assignment.mid(separator + 1).trimmed();
    if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) *value = true;
    else if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) *value = false;
    else if (text.compare(QStringLiteral("null"), Qt::CaseInsensitive) == 0) *value = QJsonValue(QJsonValue::Null);
    else
    {
        bool integerOk = false;
        const qlonglong integer = text.toLongLong(&integerOk);
        if (integerOk) *value = integer;
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
        if (!parseBinding(assignment, &key, &value, error)) return false;
        bindings->insert(key, value);
    }
    return true;
}

bool readDocumentFromPath(const PDFToolOptions& options, const QString& path, pdf::PDFDocument* document, QByteArray* sourceData, QString* error)
{
    pdf::PDFDocumentReader reader(nullptr,
                                  [&options](bool*) { return options.password; },
                                  options.permissiveReading,
                                  false);
    *document = reader.readFromFile(path);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        if (error) *error = reader.getErrorMessage();
        return false;
    }
    if (sourceData) *sourceData = reader.getSource();
    return true;
}

QJsonObject resultWithInput(const pdf::PDFActionListExecutionResult& result,
                            const QString& input,
                            const QByteArray& inputData)
{
    QJsonObject object = result.toJson();
    object.insert(QStringLiteral("input"), QJsonObject{
        { QStringLiteral("path"), input },
        { QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(inputData, QCryptographicHash::Sha256).toHex()) }
    });
    return object;
}

PDFToolExitCode statusExitCode(const pdf::PDFActionListExecutionResult& result)
{
    if (result.status == QStringLiteral("cancelled")) return PDFToolExitCode::Cancelled;
    if (result.status == QStringLiteral("failed")) return PDFToolExitCode::ProcessingFailure;
    return PDFToolExitCode::Success;
}

bool recordActionListHistory(const QString& outputPath,
                             const QByteArray& sourceData,
                             const QByteArray& candidateData,
                             const QString& operationId,
                             const QJsonObject& parameters,
                             const QJsonObject& summary,
                             QString* error)
{
    const QString historyDirectory = QFileInfo(outputPath).absoluteFilePath() + QStringLiteral(".loupe-history");
    pdf::PDFArtifactStore artifacts(historyDirectory);
    const auto input = artifacts.importBytes(sourceData, { QStringLiteral("application/pdf"), QStringLiteral("original-input.pdf") });
    const auto output = artifacts.importBytes(candidateData, { QStringLiteral("application/pdf"), QStringLiteral("candidate-output.pdf") });
    if (!input.success || !output.success)
    {
        if (error) *error = input.success ? output.errorMessage : input.errorMessage;
        return false;
    }
    pdf::PDFOperationHistoryStore history(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString historyError;
    if (!history.open(&historyError) || !history.registerOriginalInput(input.artifact) || !history.registerArtifact(output.artifact))
    {
        if (error) *error = historyError.isEmpty() ? QStringLiteral("Could not register Action List history artifacts.") : historyError;
        return false;
    }
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = operationId;
    execution.input = input.artifact;
    execution.parameters = parameters;
    QUuid executionId;
    if (!history.beginExecution(execution, &executionId))
    {
        if (error) *error = QStringLiteral("Could not begin Action List history.");
        return false;
    }
    pdf::PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.status = pdf::PDFOperationHistoryStatus::Running;
    if (!history.appendEvent(running))
    {
        if (error) *error = QStringLiteral("Could not append Action List history start.");
        return false;
    }
    pdf::PDFOperationHistoryEvent accepted;
    accepted.executionId = executionId;
    accepted.status = pdf::PDFOperationHistoryStatus::Accepted;
    accepted.output = output.artifact;
    accepted.resultSummary = summary;
    accepted.approval.kind = pdf::PDFApprovalKind::System;
    accepted.approval.actorId = QStringLiteral("PdfTool");
    accepted.approval.decision = QStringLiteral("approve");
    accepted.approval.rationale = QStringLiteral("Action List execution completed successfully.");
    accepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    if (!history.appendEvent(accepted))
    {
        if (error) *error = QStringLiteral("Could not append Action List accepted history.");
        return false;
    }
    return true;
}

} // namespace

QString PDFToolActionList::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command: return QStringLiteral("action-list");
        case Name: return PDFToolTranslationContext::tr("Action List");
        case Description: return PDFToolTranslationContext::tr("Validate, plan, and execute reusable declarative Loupe operations.");
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

    if (subcommand == QStringLiteral("validate"))
    {
        QStringList validationErrors;
        const pdf::PDFOperationResult validation = executor.validate(actionList, executionOptions, &validationErrors);
        const QJsonObject data{
            { QStringLiteral("schema"), QStringLiteral("loupe-action-list-validation") },
            { QStringLiteral("recipe"), options.actionListRecipe },
            { QStringLiteral("action_list"), actionList.toJson() },
            { QStringLiteral("valid"), bool(validation) },
            { QStringLiteral("errors"), QJsonArray::fromStringList(validationErrors) }
        };
        if (options.executionContext) options.executionContext->setData(data);
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
                items.append(QJsonObject{{QStringLiteral("input"), input}, {QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("error"), error}});
                aggregateCode = PDFToolExitCode::InputError;
                continue;
            }
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
                if (outputCheck != PDFToolExitCode::Success || QFileInfo(output).absoluteFilePath() == QFileInfo(input).absoluteFilePath())
                {
                    aggregateCode = PDFToolExitCode::ProcessingFailure;
                }
                else
                {
                    QByteArray candidateData;
                    pdf::PDFDocument reopened;
                    const pdf::PDFOperationResult serializeResult = pdf::PDFRepairDiffEngine::buildSerializedCandidate(
                        candidate, [](pdf::PDFDocument*) { return pdf::PDFOperationResult(true); }, output, &reopened, &candidateData);
                    if (!serializeResult)
                    {
                        aggregateCode = PDFToolExitCode::ProcessingFailure;
                        item.insert(QStringLiteral("error"), serializeResult.getErrorMessage());
                    }
                    else
                    {
                        QString historyError;
                        if (!recordActionListHistory(output, sourceData, candidateData, actionList.id,
                                                     QJsonObject{{QStringLiteral("recipe"), options.actionListRecipe}, {QStringLiteral("bindings"), bindings}},
                                                     item, &historyError))
                        {
                            aggregateCode = PDFToolExitCode::ProcessingFailure;
                            item.insert(QStringLiteral("error"), historyError);
                        }
                    }
                }
            }
            items.append(std::move(item));
        }
        const QJsonObject data{{QStringLiteral("schema"), QStringLiteral("loupe-action-list-batch")}, {QStringLiteral("recipe"), actionList.id}, {QStringLiteral("items"), items}};
        if (options.executionContext) options.executionContext->setData(data);
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
        if (QFileInfo(options.actionListOutputDocument).absoluteFilePath() == QFileInfo(options.actionListFiles.first()).absoluteFilePath())
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.source-collision"),
                             QStringLiteral("Action List output must be a new path; the source PDF is never overwritten implicitly."));
            return PDFToolExitCode::InvalidInvocation;
        }
        const PDFToolExitCode outputCheck = validateDestructiveOutput(options, options.actionListOutputDocument);
        if (outputCheck != PDFToolExitCode::Success) return outputCheck;
        QByteArray candidateData;
        pdf::PDFDocument reopened;
        if (const pdf::PDFOperationResult serializeResult = pdf::PDFRepairDiffEngine::buildSerializedCandidate(candidate, [](pdf::PDFDocument*) { return pdf::PDFOperationResult(true); }, options.actionListOutputDocument, &reopened, &candidateData); !serializeResult)
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("action-list.output-serialize-failed"), serializeResult.getErrorMessage());
            return PDFToolExitCode::ProcessingFailure;
        }
        data.insert(QStringLiteral("output"), QJsonObject{{QStringLiteral("path"), options.actionListOutputDocument}, {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256).toHex())}});
        QString historyError;
        if (!recordActionListHistory(options.actionListOutputDocument, sourceData, candidateData, actionList.id,
                                     QJsonObject{{QStringLiteral("recipe"), options.actionListRecipe}, {QStringLiteral("bindings"), bindings}},
                                     data, &historyError))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.write-failed"), historyError);
            return PDFToolExitCode::ProcessingFailure;
        }
        if (options.executionContext) options.executionContext->addOutput({QStringLiteral("file"), QStringLiteral("primary"), options.actionListOutputDocument, QStringLiteral("written")});
    }
    if (options.executionContext) options.executionContext->setData(data);
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
        PDFConsole::writeText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)), options.outputCodec);
    return execution ? PDFToolExitCode::Success : statusExitCode(executionResult);
}

PDFToolAbstractApplication::Options PDFToolActionList::getOptionsFlags() const
{
    return ConsoleFormat | ActionList | DestructiveWrite;
}

static PDFToolActionList s_actionListApplication;

} // namespace pdftool
