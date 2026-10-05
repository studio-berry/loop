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

#ifndef PDFOPERATIONHISTORYSTORE_H
#define PDFOPERATIONHISTORYSTORE_H

#include "pdfoperationhistory.h"
#include "pdfgovernedexecution.h"
#include "pdfutils.h"
#include "pdfschemaversion.h"

#include <QString>

#include <memory>
#include <optional>

namespace pdf
{

class PDFArtifactStore;

struct LOOPLIBCORESHARED_EXPORT PDFArtifactRegistrationOptions
{
    bool isOriginalInput = false;
};

struct LOOPLIBCORESHARED_EXPORT PDFOperationHistoryStoreOptions
{
    int busyTimeoutMs = 5000;
};

/// Reconstructed provenance for one governed publication, read back from the
/// canonical operation-history chain. Every link (plan, approval, execution,
/// artifacts, validation, sign-off) is read from the chain; nothing is
/// synthesized. `reconstructed` is false and `refusal` names the fail-closed
/// reason when the chain does not verify or no accepted publication binds the
/// requested output.
struct LOOPLIBCORESHARED_EXPORT PDFGovernedPublicationAudit
{
    bool reconstructed = false;
    /// Fail-closed reason (`chain-compromised`, `no-accepted-publication`, ...)
    /// when `reconstructed` is false.
    QString refusal;
    QString publishedSha256;
    /// Digest of the dry-run plan the approval authorized.
    QString planDigest;
    /// The approval that authorized this publication (actor/policy/decision/expiry).
    PDFApprovalRecord approval;
    /// Execution identity and source artifact.
    QUuid executionId;
    QString operationId;
    int operationVersion = 1;
    PDFArtifactIdentity inputArtifact;
    /// Artifact digests: source revision, reviewed/published candidate, and the
    /// revalidation report the sign-off binds.
    QString sourceSha256;
    QString candidateSha256;
    QString reportArtifactSha256;
    /// Validation link: the #38 revalidation state, its fail-closed reason code,
    /// the report digest, and the finding-delta summary.
    QString revalidationState;
    QString revalidationReasonCode;
    QString revalidationReportSha256;
    QString effectiveProfileDigest;
    QJsonObject validationDelta;
    /// Sign-off certificate, when the accepted event stored one.
    std::optional<PDFGovernedExecutionSignOff> signOff;
    /// Operator identity recorded on the accepted event.
    QString operatorIdentity;
    QDateTime publishedUtc;

    QJsonObject toJson() const;
};

class LOOPLIBCORESHARED_EXPORT PDFOperationHistoryStore
{
public:
    explicit PDFOperationHistoryStore(QString databasePath,
                                      PDFOperationHistoryStoreOptions options = {});
    ~PDFOperationHistoryStore();

    PDFOperationHistoryStore(const PDFOperationHistoryStore&) = delete;
    PDFOperationHistoryStore& operator=(const PDFOperationHistoryStore&) = delete;

    PDFOperationResult open(QString* errorMessage = nullptr);
    void close();
    bool isOpen() const;
    const QString& databasePath() const { return m_databasePath; }

    /// Registers immutable artifact metadata before an execution can reference it.
    PDFOperationResult registerArtifact(const PDFArtifactIdentity& artifact,
                                        PDFArtifactRegistrationOptions options = {});

    /// Records the as-received input as a protected rollback point. It has no
    /// producing audit event, because it predates the first operation.
    PDFOperationResult registerOriginalInput(const PDFArtifactIdentity& artifact);

    /// Begins an execution. If executionId is null, a fresh UUID is assigned.
    /// Parameters are redacted and canonically serialized before persistence.
    PDFOperationResult beginExecution(PDFOperationHistoryExecution execution,
                                      QUuid* executionId = nullptr);

    /// The only write API for history_events. There is deliberately no update or
    /// delete API; corrections and rollback are new events.
    PDFOperationResult appendEvent(PDFOperationHistoryEvent event,
                                   qint64* sequence = nullptr);

    PDFOperationResult appendSchemaMigratedEvent(const PDFArtifactIdentity& artifact,
                                                 PDFSchemaKind kind,
                                                 PDFSchemaVersion fromVersion,
                                                 PDFSchemaVersion toVersion,
                                                 const QString& documentRevisionDigest = QString());

    QList<PDFOperationHistoryEvent> events(QString* errorMessage = nullptr) const;
    /// Reads one execution's identity (operation, version, source artifact,
    /// parameters) back from the `executions` table. Absent when the id is
    /// unknown, so a reader can distinguish "no execution" from an empty one.
    std::optional<PDFOperationHistoryExecution> execution(const QUuid& executionId,
                                                          QString* errorMessage = nullptr) const;
    PDFOperationHistoryVerification verify() const;

    QList<PDFRollbackPoint> rollbackPoints(QString* errorMessage = nullptr) const;

    PDFHistoryRetentionResult enforceRetention(const PDFHistoryRetentionPolicy& policy,
                                               const PDFArtifactStore& artifacts,
                                               QDateTime nowUtc = QDateTime::currentDateTimeUtc());

    /// Verifies the target before using QSaveFile to replace the current
    /// document, then appends a new rolled-back event. Existing events remain.
    PDFOperationResult rollbackTo(const PDFRollbackRequest& request,
                                  const PDFArtifactStore& artifacts,
                                  const QString& destinationPath,
                                  qint64* sequence = nullptr);

    /// Resolves a rollback target only when it is the output of an accepted event.
    PDFOperationResult resolveRollbackTarget(const PDFRollbackRequest& request,
                                             PDFArtifactIdentity* targetArtifact) const;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
    QString m_databasePath;
    PDFOperationHistoryStoreOptions m_options;

    /// Records the schema upgrade that this open performed: the database file as
    /// it was before SQLite rewrote it, from the previous version to the current
    /// one. Joins the caller's transaction; open() rolls it back on failure.
    bool appendSchemaUpgradeProvenance(bool upgraded,
                                       int previousSchemaVersion,
                                       const QString& databaseDigest,
                                       qint64 databaseSize,
                                       QString* error);
};

/// Reconstructs the governed provenance of one published output from the
/// canonical chain. Refuses (returns false and leaves `reconstructed` false)
/// when the chain fails `verify()` or when no accepted publication binds the
/// exact `publishedSha256`. The reader answers "who approved what output": the
/// plan digest, the authorizing approval, the execution, every artifact digest,
/// the #38 validation state + delta summary, and the sign-off certificate are
/// all read from chain events, never synthesized.
LOOPLIBCORESHARED_EXPORT PDFOperationResult reconstructGovernedPublicationAudit(
    const PDFOperationHistoryStore& store,
    const QString& publishedSha256,
    PDFGovernedPublicationAudit* audit);

}   // namespace pdf

#endif   // PDFOPERATIONHISTORYSTORE_H
