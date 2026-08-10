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

#include "pdfoperationhistory.h"

#include <QJsonArray>
#include <QCryptographicHash>
#include <QJsonDocument>

namespace pdf
{

namespace
{

QString dateTimeString(const QDateTime& value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime dateTimeFromString(const QString& value)
{
    return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
}

} // namespace

QString pdfOperationHistoryStatusToString(PDFOperationHistoryStatus status)
{
    switch (status)
    {
        case PDFOperationHistoryStatus::Planned: return QStringLiteral("planned");
        case PDFOperationHistoryStatus::Running: return QStringLiteral("running");
        case PDFOperationHistoryStatus::Rejected: return QStringLiteral("rejected");
        case PDFOperationHistoryStatus::Failed: return QStringLiteral("failed");
        case PDFOperationHistoryStatus::Cancelled: return QStringLiteral("cancelled");
        case PDFOperationHistoryStatus::Interrupted: return QStringLiteral("interrupted");
        case PDFOperationHistoryStatus::Accepted: return QStringLiteral("accepted");
        case PDFOperationHistoryStatus::RolledBack: return QStringLiteral("rolled-back");
    }
    return QStringLiteral("failed");
}

PDFOperationHistoryStatus pdfOperationHistoryStatusFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("planned")) return PDFOperationHistoryStatus::Planned;
    if (normalized == QStringLiteral("running")) return PDFOperationHistoryStatus::Running;
    if (normalized == QStringLiteral("rejected")) return PDFOperationHistoryStatus::Rejected;
    if (normalized == QStringLiteral("cancelled") || normalized == QStringLiteral("canceled")) return PDFOperationHistoryStatus::Cancelled;
    if (normalized == QStringLiteral("interrupted")) return PDFOperationHistoryStatus::Interrupted;
    if (normalized == QStringLiteral("accepted")) return PDFOperationHistoryStatus::Accepted;
    if (normalized == QStringLiteral("rolled-back") || normalized == QStringLiteral("rolledback")) return PDFOperationHistoryStatus::RolledBack;
    return PDFOperationHistoryStatus::Failed;
}

QString pdfApprovalKindToString(PDFApprovalKind kind)
{
    switch (kind)
    {
        case PDFApprovalKind::None: return QStringLiteral("none");
        case PDFApprovalKind::Human: return QStringLiteral("human");
        case PDFApprovalKind::Policy: return QStringLiteral("policy");
        case PDFApprovalKind::System: return QStringLiteral("system");
    }
    return QStringLiteral("none");
}

PDFApprovalKind pdfApprovalKindFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("human")) return PDFApprovalKind::Human;
    if (normalized == QStringLiteral("policy")) return PDFApprovalKind::Policy;
    if (normalized == QStringLiteral("system")) return PDFApprovalKind::System;
    return PDFApprovalKind::None;
}

bool PDFApprovalRecord::isValid() const
{
    if (kind == PDFApprovalKind::None)
    {
        return actorId.isEmpty() && decision.isEmpty() && policyId.isEmpty() &&
               rationale.isEmpty() && evidenceSha256.isEmpty() && !decidedUtc.isValid();
    }
    if (actorId.trimmed().isEmpty() || decision.trimmed().isEmpty() || !decidedUtc.isValid())
    {
        return false;
    }
    if (kind == PDFApprovalKind::Policy && policyId.trimmed().isEmpty())
    {
        return false;
    }
    return evidenceSha256.isEmpty() || isPDFSha256(evidenceSha256);
}

QJsonObject PDFApprovalRecord::toJson() const
{
    return QJsonObject{
        { QStringLiteral("kind"), pdfApprovalKindToString(kind) },
        { QStringLiteral("actorId"), actorId },
        { QStringLiteral("decision"), decision },
        { QStringLiteral("policyId"), policyId },
        { QStringLiteral("rationale"), rationale },
        { QStringLiteral("evidenceSha256"), evidenceSha256 },
        { QStringLiteral("decidedUtc"), dateTimeString(decidedUtc) }
    };
}

PDFApprovalRecord PDFApprovalRecord::fromJson(const QJsonObject& object)
{
    PDFApprovalRecord approval;
    approval.kind = pdfApprovalKindFromString(object.value(QStringLiteral("kind")).toString());
    approval.actorId = object.value(QStringLiteral("actorId")).toString();
    approval.decision = object.value(QStringLiteral("decision")).toString();
    approval.policyId = object.value(QStringLiteral("policyId")).toString();
    approval.rationale = object.value(QStringLiteral("rationale")).toString();
    approval.evidenceSha256 = object.value(QStringLiteral("evidenceSha256")).toString().toLower();
    approval.decidedUtc = dateTimeFromString(object.value(QStringLiteral("decidedUtc")).toString());
    return approval;
}

QJsonObject PDFOperationHistoryEvent::toJson() const
{
    QJsonObject object{
        { QStringLiteral("sequence"), sequence },
        { QStringLiteral("entryId"), entryId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("executionId"), executionId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("status"), pdfOperationHistoryStatusToString(status) },
        { QStringLiteral("result"), resultSummary },
        { QStringLiteral("findingIds"), QJsonArray::fromStringList(findingIds) },
        { QStringLiteral("reportArtifactSha256"), reportArtifactSha256 },
        { QStringLiteral("diffArtifactSha256"), diffArtifactSha256 },
        { QStringLiteral("approval"), approval.toJson() },
        { QStringLiteral("previousEventHash"), QString::fromLatin1(previousEventHash.toHex()) },
        { QStringLiteral("eventHash"), QString::fromLatin1(eventHash.toHex()) },
        { QStringLiteral("createdUtc"), dateTimeString(createdUtc) }
    };
    if (output)
    {
        object.insert(QStringLiteral("output"), output->toJson());
    }
    return object;
}

QJsonObject PDFOperationHistoryVerification::toJson() const
{
    return QJsonObject{
        { QStringLiteral("integrity"), integrity },
        { QStringLiteral("verified"), verified },
        { QStringLiteral("eventsChecked"), eventsChecked },
        { QStringLiteral("firstSequence"), firstSequence },
        { QStringLiteral("lastSequence"), lastSequence },
        { QStringLiteral("error"), errorMessage }
    };
}

QJsonObject PDFRollbackRequest::toJson() const
{
    return QJsonObject{
        { QStringLiteral("operation"), QStringLiteral("rollback") },
        { QStringLiteral("currentArtifactSha256"), currentArtifactSha256 },
        { QStringLiteral("targetArtifactSha256"), targetArtifactSha256 },
        { QStringLiteral("targetExecutionId"), targetExecutionId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("approval"), approval.toJson() }
    };
}

bool PDFRollbackPoint::isValid() const
{
    return !rollbackId.trimmed().isEmpty() && isPDFSha256(documentRevisionDigest) &&
           createdAtUtc.isValid() && artifactBytes >= 0 && !artifactPath.contains(QStringLiteral("..")) &&
           (!isOriginalInput || auditEventId.isNull());
}

QJsonObject PDFRollbackPoint::toJson() const
{
    return QJsonObject{
        { QStringLiteral("rollbackId"), rollbackId },
        { QStringLiteral("auditEventId"), auditEventId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("documentRevisionDigest"), documentRevisionDigest },
        { QStringLiteral("createdAtUtc"), dateTimeString(createdAtUtc) },
        { QStringLiteral("artifactPath"), artifactPath },
        { QStringLiteral("artifactBytes"), artifactBytes },
        { QStringLiteral("operationId"), operationId },
        { QStringLiteral("planSummary"), planSummary },
        { QStringLiteral("isOriginalInput"), isOriginalInput },
        { QStringLiteral("approvedOutput"), approvedOutput },
        { QStringLiteral("artifactEvicted"), artifactEvicted },
        { QStringLiteral("evictedAtUtc"), dateTimeString(evictedAtUtc) }
    };
}

QByteArray computeOperationHistoryEventHash(const PDFOperationHistoryEvent& event,
                                            const QByteArray& previousHash)
{
    const QJsonObject canonical{
        { QStringLiteral("entryId"), event.entryId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("executionId"), event.executionId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("status"), pdfOperationHistoryStatusToString(event.status) },
        { QStringLiteral("result"), canonicalizeJson(event.resultSummary) },
        { QStringLiteral("output"), event.output ? canonicalizeJson(event.output->toJson()) : QJsonObject() },
        { QStringLiteral("findingIds"), QJsonArray::fromStringList(event.findingIds) },
        { QStringLiteral("reportArtifactSha256"), event.reportArtifactSha256 },
        { QStringLiteral("diffArtifactSha256"), event.diffArtifactSha256 },
        { QStringLiteral("approval"), canonicalizeJson(event.approval.toJson()) },
        { QStringLiteral("createdUtc"), dateTimeString(event.createdUtc) }
    };
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(previousHash);
    hash.addData(canonicalJson(canonical));
    return hash.result();
}

} // namespace pdf
