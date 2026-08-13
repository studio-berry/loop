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

#ifndef PDFOPERATIONHISTORY_H
#define PDFOPERATIONHISTORY_H

#include "pdfartifactidentity.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QUuid>

#include <optional>

namespace pdf
{

enum class PDFOperationHistoryStatus
{
    Planned,
    Running,
    Rejected,
    Failed,
    Cancelled,
    Interrupted,
    Accepted,
    RolledBack
};

/// Canonical provenance kinds. Preflight and decision records are events in
/// the operation-history chain, not a second audit ledger.
enum class PDFOperationHistoryEventKind
{
    Operation,
    DocumentOpened,
    PreflightRun,
    FixApplied,
    DecisionRecorded,
    DecisionInvalidated,
    CertificateIssued,
    CertificateInvalidated
};

enum class PDFApprovalKind
{
    None,
    Human,
    Policy,
    System
};

PDF4QTLIBCORESHARED_EXPORT QString pdfOperationHistoryStatusToString(PDFOperationHistoryStatus status);
PDF4QTLIBCORESHARED_EXPORT PDFOperationHistoryStatus pdfOperationHistoryStatusFromString(const QString& value);
PDF4QTLIBCORESHARED_EXPORT QString pdfOperationHistoryEventKindToString(PDFOperationHistoryEventKind kind);
PDF4QTLIBCORESHARED_EXPORT PDFOperationHistoryEventKind pdfOperationHistoryEventKindFromString(const QString& value);
PDF4QTLIBCORESHARED_EXPORT QString pdfApprovalKindToString(PDFApprovalKind kind);
PDF4QTLIBCORESHARED_EXPORT PDFApprovalKind pdfApprovalKindFromString(const QString& value);

struct PDF4QTLIBCORESHARED_EXPORT PDFApprovalRecord
{
    PDFApprovalKind kind = PDFApprovalKind::None;
    QString actorId;
    QString decision;
    QString policyId;
    QString rationale;
    QString evidenceSha256;
    QString decisionReference;
    QDateTime decidedUtc;

    bool isValid() const;
    QJsonObject toJson() const;
    static PDFApprovalRecord fromJson(const QJsonObject& object);
};

struct PDF4QTLIBCORESHARED_EXPORT PDFOperationHistoryExecution
{
    QUuid executionId;
    std::optional<QUuid> parentExecutionId;
    QString operationId;
    int operationVersion = 1;
    PDFArtifactIdentity input;
    quint64 sourceDocumentRevision = 0;
    QJsonObject parameters;
    QDateTime startedUtc;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFOperationHistoryEvent
{
    qint64 sequence = 0;
    QUuid entryId;
    QUuid executionId;
    PDFOperationHistoryEventKind kind = PDFOperationHistoryEventKind::Operation;
    PDFOperationHistoryStatus status = PDFOperationHistoryStatus::Planned;
    QString operatorIdentity;
    QString documentRevisionDigest;
    QString effectiveProfileDigest;
    QJsonObject resultSummary;
    std::optional<PDFArtifactIdentity> output;
    QStringList findingIds;
    QString reportArtifactSha256;
    QString diffArtifactSha256;
    PDFApprovalRecord approval;
    QByteArray previousEventHash;
    QByteArray eventHash;
    QDateTime createdUtc;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFOperationHistoryVerification
{
    bool verified = false;
    QString integrity = QStringLiteral("unverified");
    qint64 eventsChecked = 0;
    qint64 firstSequence = 0;
    qint64 lastSequence = 0;
    QString errorMessage;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRollbackRequest
{
    QString currentArtifactSha256;
    QString targetArtifactSha256;
    QUuid targetExecutionId;
    QString reason;
    PDFApprovalRecord approval;

    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRollbackPoint
{
    QString rollbackId;
    QUuid auditEventId;
    QString documentRevisionDigest;
    QDateTime createdAtUtc;
    QString artifactPath;
    qint64 artifactBytes = 0;
    QString operationId;
    QString planSummary;
    bool isOriginalInput = false;
    bool approvedOutput = false;
    bool artifactEvicted = false;
    QDateTime evictedAtUtc;

    bool isValid() const;
    QJsonObject toJson() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFHistoryRetentionPolicy
{
    int maxPointsPerJob = 20;
    qint64 maxBytesPerJob = 2LL * 1024LL * 1024LL * 1024LL;
    int maxAgeDays = 90;
    bool keepOriginalInput = true;
    bool keepApprovedOutputs = true;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFHistoryRetentionResult
{
    bool success = false;
    int pointsEvicted = 0;
    qint64 bytesEvicted = 0;
    QString errorMessage;
};

PDF4QTLIBCORESHARED_EXPORT QByteArray computeOperationHistoryEventHash(
        const PDFOperationHistoryEvent& event,
        const QByteArray& previousHash);

} // namespace pdf

#endif // PDFOPERATIONHISTORY_H
