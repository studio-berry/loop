// MIT License

#include "pdfpreflightaudit.h"

#include "pdfartifactstore.h"
#include "pdfoperationhistorystore.h"
#include "pdfpreflightcertificate.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace pdf
{

QJsonObject preflightAuditReportSummary(const PreflightResult& result, const QString& documentPath)
{
    QJsonObject summary = result.toJson(documentPath);
    if (!result.effectiveProfileDigest.isEmpty())
    {
        summary.insert(QStringLiteral("effective_profile_digest"), result.effectiveProfileDigest);
    }
    if (!result.profileIdentity.isEmpty())
    {
        summary.insert(QStringLiteral("profile_identity"), result.profileIdentity);
    }
    if (!result.coverageScope.isEmpty())
    {
        summary.insert(QStringLiteral("coverage_scope"), result.coverageScope);
    }
    return summary;
}

PDFOperationResult appendPreflightAuditRun(const QString& documentPath,
                                           const QByteArray& documentBytes,
                                           const PreflightResult& result,
                                           PDFOperationHistoryStatus status,
                                           const QString& operatorIdentity,
                                           const QJsonObject& summary)
{
    if (documentPath.trimmed().isEmpty() || documentBytes.isEmpty())
        return PDFOperationResult(QStringLiteral("Preflight audit requires a document path and revision bytes."));
    if (operatorIdentity.trimmed().isEmpty())
        return PDFOperationResult(QStringLiteral("Preflight audit requires an operator identity."));

    const QString historyDirectory = QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history");
    PDFArtifactStore artifacts(historyDirectory);
    const PDFArtifactStoreResult imported =
        artifacts.importBytes(documentBytes,
                              { QStringLiteral("application/pdf"), QStringLiteral("preflight-input.pdf") });
    if (!imported.success)
        return PDFOperationResult(imported.errorMessage);

    PDFOperationHistoryStore history(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString historyError;
    if (const PDFOperationResult openResult = history.open(&historyError); !openResult)
        return PDFOperationResult(historyError.isEmpty() ? openResult.getErrorMessage() : historyError);
    if (const PDFOperationResult artifactResult = history.registerArtifact(imported.artifact); !artifactResult)
        return artifactResult;

    PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("preflight");
    execution.operationVersion = 1;
    execution.input = imported.artifact;
    QUuid executionId;
    if (const PDFOperationResult beginResult = history.beginExecution(execution, &executionId); !beginResult)
        return beginResult;

    PDFOperationHistoryEvent opened;
    opened.executionId = executionId;
    opened.kind = PDFOperationHistoryEventKind::DocumentOpened;
    opened.status = PDFOperationHistoryStatus::Running;
    opened.operatorIdentity = operatorIdentity.trimmed();
    opened.documentRevisionDigest = result.documentRevisionDigest;
    opened.effectiveProfileDigest = result.effectiveProfileDigest;
    if (const PDFOperationResult openedResult = history.appendEvent(opened); !openedResult)
        return openedResult;

    PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.kind = PDFOperationHistoryEventKind::PreflightRun;
    running.status = PDFOperationHistoryStatus::Running;
    running.operatorIdentity = operatorIdentity.trimmed();
    running.documentRevisionDigest = result.documentRevisionDigest;
    running.effectiveProfileDigest = result.effectiveProfileDigest;
    if (const PDFOperationResult runningResult = history.appendEvent(running); !runningResult)
        return runningResult;

    PDFOperationHistoryEvent finished;
    finished.executionId = executionId;
    finished.kind = PDFOperationHistoryEventKind::PreflightRun;
    finished.status = status;
    finished.operatorIdentity = operatorIdentity.trimmed();
    finished.documentRevisionDigest = result.documentRevisionDigest;
    finished.effectiveProfileDigest = result.effectiveProfileDigest;
    finished.resultSummary = summary.isEmpty() ? preflightAuditReportSummary(result, documentPath) : summary;
    if (status == PDFOperationHistoryStatus::Accepted || status == PDFOperationHistoryStatus::RolledBack)
        finished.output = imported.artifact;
    if (const PDFOperationResult finishedResult = history.appendEvent(finished); !finishedResult)
        return finishedResult;

    for (const PreflightDecision& decision : result.decisions)
    {
        const QString decisionId = preflightDecisionIdentity(decision);
        PDFOperationHistoryEvent decisionEvent;
        decisionEvent.executionId = executionId;
        decisionEvent.kind = decision.countsForSignoff(result.documentRevisionDigest, result.effectiveProfileDigest)
                                 ? PDFOperationHistoryEventKind::DecisionRecorded
                                 : PDFOperationHistoryEventKind::DecisionInvalidated;
        decisionEvent.status = PDFOperationHistoryStatus::Running;
        decisionEvent.operatorIdentity = decision.operatorIdentity.trimmed().isEmpty()
                                             ? operatorIdentity.trimmed()
                                             : decision.operatorIdentity.trimmed();
        decisionEvent.documentRevisionDigest = result.documentRevisionDigest;
        decisionEvent.effectiveProfileDigest = result.effectiveProfileDigest;
        decisionEvent.approval.decisionReference = decisionId;
        decisionEvent.resultSummary = QJsonObject{
            { QStringLiteral("decision_id"), decisionId },
            { QStringLiteral("finding_id"), decision.findingId },
            { QStringLiteral("kind"), preflightDecisionKindToString(decision.kind) }
        };
        if (const PDFOperationResult decisionResult = history.appendEvent(decisionEvent); !decisionResult)
            return decisionResult;
    }

    return PDFOperationResult(true);
}

}   // namespace pdf
