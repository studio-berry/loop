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

#include "pdftoolpreflight.h"

#include "pdfdocumentsession.h"
#include "preflightprofileresolver.h"
#include "preflightengine.h"
#include "pdfpreflightverdict.h"
#include "pdfartifactstore.h"
#include "pdfjobscheduler.h"
#include "pdfoperationhistorystore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <optional>

namespace pdftool
{

namespace
{

static PDFToolPreflightApplication s_preflightApplication;

bool appendPreflightProvenance(const QString& documentPath,
                               const QByteArray& sourceData,
                               const QString& revisionDigest,
                               const QString& profileDigest,
                               pdf::PDFOperationHistoryStatus status,
                               const QJsonObject& summary,
                               QString* error)
{
    const QString historyDirectory = QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loupe-history");
    pdf::PDFArtifactStore artifacts(historyDirectory);
    const auto imported = artifacts.importBytes(sourceData, { QStringLiteral("application/pdf"), QStringLiteral("preflight-input.pdf") });
    if (!imported.success)
    {
        if (error)
        {
            *error = imported.errorMessage;
        }
        return false;
    }
    pdf::PDFOperationHistoryStore history(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString historyError;
    if (!history.open(&historyError) || !history.registerOriginalInput(imported.artifact))
    {
        if (error)
        {
            *error = historyError.isEmpty() ? QStringLiteral("Could not open preflight history.") : historyError;
        }
        return false;
    }
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("preflight");
    execution.operationVersion = 1;
    execution.input = imported.artifact;
    QUuid executionId;
    if (!history.beginExecution(execution, &executionId))
    {
        if (error)
        {
            *error = QStringLiteral("Could not begin preflight history.");
        }
        return false;
    }
    pdf::PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
    running.status = pdf::PDFOperationHistoryStatus::Running;
    running.documentRevisionDigest = revisionDigest;
    running.effectiveProfileDigest = profileDigest;
    running.operatorIdentity = QStringLiteral("PdfTool");
    if (!history.appendEvent(running))
    {
        if (error)
        {
            *error = QStringLiteral("Could not append preflight history start.");
        }
        return false;
    }
    pdf::PDFOperationHistoryEvent finished;
    finished.executionId = executionId;
    finished.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
    finished.status = status;
    finished.documentRevisionDigest = revisionDigest;
    finished.effectiveProfileDigest = profileDigest;
    finished.operatorIdentity = QStringLiteral("PdfTool");
    finished.resultSummary = summary;
    finished.createdUtc = QDateTime::currentDateTimeUtc();
    if (status == pdf::PDFOperationHistoryStatus::Accepted || status == pdf::PDFOperationHistoryStatus::RolledBack)
    {
        finished.output = imported.artifact;
    }
    if (!history.appendEvent(finished))
    {
        const QString appendError = QStringLiteral("Could not append preflight history result.");
        pdf::PDFOperationHistoryEvent failed;
        failed.executionId = executionId;
        failed.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
        failed.status = pdf::PDFOperationHistoryStatus::Failed;
        failed.documentRevisionDigest = revisionDigest;
        failed.effectiveProfileDigest = profileDigest;
        failed.operatorIdentity = QStringLiteral("PdfTool");
        failed.resultSummary = QJsonObject{ { QStringLiteral("error"), appendError } };
        history.appendEvent(failed);
        if (error)
        {
            *error = appendError;
        }
        return false;
    }
    return true;
}

bool loadProfileJson(const QString& profilePath, QJsonObject& profile, QString& errorMessage)
{
    return pdf::PreflightEngine::loadProfile(profilePath, profile, errorMessage);
}

bool loadJobContext(const QString& contextPath, pdf::PreflightJobContext& context, QString& errorMessage)
{
    QFile contextFile(contextPath);
    if (!contextFile.open(QIODevice::ReadOnly))
    {
        errorMessage = PDFToolTranslationContext::tr("Cannot open job context '%1'.").arg(contextPath);
        return false;
    }
    if (contextFile.size() > 1024 * 1024)
    {
        errorMessage = PDFToolTranslationContext::tr("Job context '%1' exceeds the maximum supported size.").arg(contextPath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contextFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = PDFToolTranslationContext::tr("Invalid job context JSON in '%1': %2").arg(contextPath, parseError.errorString());
        return false;
    }

    QJsonObject object = document.object();
    if (object.value(QStringLiteral("context")).isObject())
    {
        object = object.value(QStringLiteral("context")).toObject();
    }
    return pdf::PreflightJobContext::fromJson(object, context, errorMessage);
}

bool hasDirectContext(const PDFToolOptions& options)
{
    return !options.preflightClientId.isEmpty() || !options.preflightProductId.isEmpty() || !options.preflightJobType.isEmpty() || !options.preflightPressId.isEmpty() || !options.preflightStockId.isEmpty() || !options.preflightFinishingId.isEmpty();
}

QString defaultProfileStorePath()
{
    const QStringList candidates = {
        QDir::current().filePath(QStringLiteral("loupe-preflight/profiles")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../share/loupe/profiles")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles"))
    };
    for (const QString& candidate : candidates)
    {
        if (QFileInfo(candidate).isDir())
        {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
}

bool loadDecisions(const QString& decisionsPath,
                   QList<pdf::PreflightDecision>& decisions,
                   QString& errorMessage)
{
    decisions.clear();
    if (decisionsPath.isEmpty())
    {
        return true;
    }

    QFile file(decisionsPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = PDFToolTranslationContext::tr("Cannot open decisions file '%1'.").arg(decisionsPath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = PDFToolTranslationContext::tr("Invalid decisions JSON in '%1': %2")
                           .arg(decisionsPath, parseError.errorString());
        return false;
    }

    return pdf::preflightDecisionsFromJson(document.object(), decisions, errorMessage);
}

bool exportDecisions(const QString& decisionsPath,
                     const QList<pdf::PreflightDecision>& decisions,
                     QString& errorMessage)
{
    if (decisionsPath.isEmpty())
    {
        return true;
    }

    QFile file(decisionsPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        errorMessage = PDFToolTranslationContext::tr("Cannot write decisions file '%1'.").arg(decisionsPath);
        return false;
    }

    const QByteArray payload = QJsonDocument(pdf::preflightDecisionsToJson(decisions)).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size())
    {
        errorMessage = PDFToolTranslationContext::tr("Could not write decisions file '%1'.").arg(decisionsPath);
        return false;
    }

    return true;
}

bool hasActiveSignoffForFinding(const pdf::PreflightFinding& finding,
                                const QList<pdf::PreflightDecision>& decisions,
                                const QString& documentDigest,
                                const QString& profileDigest)
{
    const pdf::PreflightDecision* latest = nullptr;
    for (const pdf::PreflightDecision& decision : decisions)
    {
        if (decision.findingId != finding.stableId() || (latest && decision.timestampUtc < latest->timestampUtc))
        {
            continue;
        }
        latest = &decision;
    }

    return latest && latest->countsForSignoff(documentDigest, profileDigest);
}

}   // namespace

QString PDFToolPreflightApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("preflight");

        case Name:
            return PDFToolTranslationContext::tr("Preflight");

        case Description:
            return PDFToolTranslationContext::tr("Run Loupe preflight checks and emit a normalized JSON report.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}

PDFToolExitCode PDFToolPreflightApplication::execute(const PDFToolOptions& options)
{
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("The preflight command only supports JSON output."));
        return PDFToolExitCode::InvalidInvocation;
    }

    if (options.document.isEmpty())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("No document specified."));
        return PDFToolExitCode::InputError;
    }

    QList<pdf::PreflightDecision> decisions;
    QString decisionsError;
    if (!loadDecisions(options.preflightDecisionsPath, decisions, decisionsError))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         decisionsError);
        return PDFToolExitCode::InvalidInvocation;
    }

    const bool hasContextInput = !options.preflightJobContextPath.isEmpty() || hasDirectContext(options) || !options.preflightProfileStorePath.isEmpty();
    if (!options.preflightProfilePath.isEmpty() && hasContextInput)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("--profile cannot be combined with contextual profile selection. Use --profile alone or provide context and a profile store."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject profileJson;
    pdf::PreflightResolvedProfile resolved;
    pdf::PreflightProfileResolver resolver;
    QString profileError;
    if (!options.preflightProfilePath.isEmpty())
    {
        if (!loadProfileJson(options.preflightProfilePath, profileJson, profileError))
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("cli.invalid-arguments"),
                             profileError);
            return PDFToolExitCode::InvalidInvocation;
        }
        resolved = resolver.resolveExplicitProfile(profileJson,
                                                   QFileInfo(options.preflightProfilePath).completeBaseName(),
                                                   QStringLiteral("explicit"));
    }
    else
    {
        pdf::PreflightJobContext context;
        if (!options.preflightJobContextPath.isEmpty() && !loadJobContext(options.preflightJobContextPath, context, profileError))
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("cli.invalid-arguments"),
                             profileError);
            return PDFToolExitCode::InvalidInvocation;
        }

        auto overrideContext = [](const QString& value, QString& target)
        {
            if (!value.isEmpty())
                target = value;
        };
        overrideContext(options.preflightClientId, context.clientId);
        overrideContext(options.preflightProductId, context.productId);
        overrideContext(options.preflightJobType, context.jobType);
        overrideContext(options.preflightPressId, context.pressId);
        overrideContext(options.preflightStockId, context.stockId);
        overrideContext(options.preflightFinishingId, context.finishingId);

        const QString storePath = options.preflightProfileStorePath.isEmpty()
                                      ? defaultProfileStorePath()
                                      : options.preflightProfileStorePath;
        if (storePath.isEmpty())
        {
            profileError = PDFToolTranslationContext::tr("No profile store found. Use --profile-store <directory> or --profile <file.json>.");
            resolved.normalizedContext = context.toJson();
            resolved.errorCode = QStringLiteral("profile-store-missing");
            resolved.errorMessage = profileError;
        }
        else
        {
            pdf::PreflightProfileSnapshot snapshot;
            if (!pdf::PreflightProfileStore::loadDirectory(storePath, snapshot, profileError))
            {
                // Keep the store error as a configuration result below.
                resolved.normalizedContext = context.toJson();
                resolved.errorCode = QStringLiteral("profile-store-invalid");
                resolved.errorMessage = profileError;
            }
            else
            {
                resolved = resolver.resolve(context, snapshot);
            }
        }
    }

    if (!resolved.ok)
    {
        pdf::PreflightResult result;
        result.inspectionComplete = false;
        result.errorCode = resolved.errorCode.isEmpty() ? QStringLiteral("profile-resolution") : resolved.errorCode;
        result.errorMessage = profileError.isEmpty() ? resolved.errorMessage : profileError;
        result.profileName = QStringLiteral("Unresolved profile");
        pdf::PreflightFinding finding;
        finding.scope = QString::fromLatin1(pdf::PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.type = QStringLiteral("profile-resolution");
        finding.severity = QStringLiteral("error");
        finding.message = profileError.isEmpty() ? resolved.errorMessage : profileError;
        finding.checkId = QStringLiteral("profile-resolution");
        finding.evidence = QJsonObject{
            { QStringLiteral("code"), profileError.isEmpty() ? resolved.errorCode : QStringLiteral("profile-store") }
        };
        result.errors.append(finding);
        result.profileResolution = resolved.provenance();
        if (options.executionContext)
        {
            options.executionContext->setData(QJsonObject{
                { QStringLiteral("report"), result.toJson(options.document) } });
        }
        const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
        return verdict.state == pdf::PreflightVerdictState::Error
                   ? PDFToolExitCode::PreflightError
                   : PDFToolExitCode::PreflightIncomplete;
    }

    profileJson = resolved.effectiveProfile;

    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const QString revisionDigest = QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex());
    const QString profileDigest = QString::fromLatin1(resolved.effectiveHash);

    struct PreflightJobOutcome
    {
        std::optional<pdf::PreflightResult> result;
    };
    PreflightJobOutcome outcome;

    pdf::PDFJobScheduler scheduler(1);
    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentRevision = revisionDigest;
    spec.operationId = QStringLiteral("preflight");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;
    const QString jobId = scheduler.submit(spec, [&engine, profileJson, &outcome](pdf::PDFJobContext& context)
                                           {
        if (context.isCancellationRequested())
        {
            return;
        }
        engine.setOperationControl(context.operationControl());
        pdf::PreflightResult runResult = engine.run(profileJson);
        if (context.isCancellationRequested())
        {
            return;
        }
        outcome.result = std::move(runResult); });

    constexpr int preflightTimeoutMs = 300000;
    if (!scheduler.waitForFinished(jobId, preflightTimeoutMs))
    {
        scheduler.cancel(jobId);
        scheduler.waitForFinished(jobId, 30000);
    }
    const pdf::PDFJobSnapshot snapshot = scheduler.snapshot(jobId);

    pdf::PreflightResult result;
    if (snapshot.status == pdf::PDFJobStatus::Succeeded && outcome.result.has_value())
    {
        result = std::move(*outcome.result);
    }
    else
    {
        if (snapshot.status == pdf::PDFJobStatus::Cancelled || snapshot.status == pdf::PDFJobStatus::Stale)
        {
            result.inspectionComplete = false;
            result.errorCode = QStringLiteral("cancelled");
            result.errorMessage = PDFToolTranslationContext::tr("Preflight was cancelled.");
        }
        else if (snapshot.status != pdf::PDFJobStatus::Succeeded)
        {
            result.inspectionComplete = false;
            result.errorCode = QStringLiteral("preflight-job-failed");
            result.errorMessage = snapshot.errorMessage.isEmpty()
                                      ? PDFToolTranslationContext::tr("Preflight job did not succeed.")
                                      : snapshot.errorMessage;
        }
    }

    result.profileResolution = resolved.provenance();
    result.documentRevisionDigest = QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex());
    result.effectiveProfileDigest = QString::fromLatin1(resolved.effectiveHash);
    result.decisions = decisions;

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    result.pass = verdict.isPass();
    PDFToolExitCode resultExitCode = PDFToolExitCode::PreflightError;
    switch (verdict.state)
    {
        case pdf::PreflightVerdictState::Pass:
            resultExitCode = PDFToolExitCode::Success;
            break;
        case pdf::PreflightVerdictState::Fail:
            resultExitCode = PDFToolExitCode::Findings;
            break;
        case pdf::PreflightVerdictState::Incomplete:
            resultExitCode = PDFToolExitCode::PreflightIncomplete;
            break;
        case pdf::PreflightVerdictState::Error:
            resultExitCode = PDFToolExitCode::PreflightError;
            break;
    }
    if (options.preflightRequireSignoff && verdict.state == pdf::PreflightVerdictState::Fail)
    {
        for (const pdf::PreflightFinding& finding : result.errors)
        {
            if (!hasActiveSignoffForFinding(finding,
                                            result.decisions,
                                            result.documentRevisionDigest,
                                            result.effectiveProfileDigest))
            {
                resultExitCode = PDFToolExitCode::Findings;
                break;
            }
            resultExitCode = PDFToolExitCode::Success;
        }
    }
    if (snapshot.status == pdf::PDFJobStatus::Cancelled || snapshot.status == pdf::PDFJobStatus::Stale)
    {
        resultExitCode = PDFToolExitCode::Cancelled;
    }

    if (!exportDecisions(options.preflightDecisionsExportPath, result.decisions, decisionsError))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.output-failed"),
                         decisionsError);
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.executionContext)
    {
        options.executionContext->setData(QJsonObject{
            { QStringLiteral("report"), result.toJson(options.document) } });
    }

    pdf::PDFOperationHistoryStatus historyStatus = pdf::PDFOperationHistoryStatus::Accepted;
    if (snapshot.status == pdf::PDFJobStatus::Cancelled || snapshot.status == pdf::PDFJobStatus::Stale)
    {
        historyStatus = pdf::PDFOperationHistoryStatus::Cancelled;
    }
    else if (verdict.state == pdf::PreflightVerdictState::Error || snapshot.status != pdf::PDFJobStatus::Succeeded)
    {
        historyStatus = pdf::PDFOperationHistoryStatus::Failed;
    }
    QString historyError;
    if (!appendPreflightProvenance(options.document,
                                   sourceData,
                                   revisionDigest,
                                   profileDigest,
                                   historyStatus,
                                   result.toJson(options.document),
                                   &historyError))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("history.write-failed"),
                         historyError);
        return PDFToolExitCode::ProcessingFailure;
    }

    return resultExitCode;
}

PDFToolAbstractApplication::Options PDFToolPreflightApplication::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PreflightProfile;
}

}   // namespace pdftool
