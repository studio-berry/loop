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

#ifndef DOCUMENTFACADE_H
#define DOCUMENTFACADE_H

#include "commandcatalog.h"
#include "documentcontextsource.h"
#include "documentloader.h"
#include "jobrelay.h"
#include "jobsubmitter.h"
#include "pdfpreflightverdict.h"

#include <QFlags>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace pdfinteraction
{

/// Presentation-facing base state of the document.
enum class DocumentState
{
    Empty,
    Opening,
    Ready,
    Closing,
    Error
};

/// Independent facts about the document that are not mutually exclusive with
/// each other or with the base state.
enum class DocumentFacet
{
    Dirty = 0x01,   ///< Has unsaved changes.
    Stale = 0x02,   ///< Bound results were computed against an older revision.
    Incomplete = 0x04,   ///< Loaded, but not everything in the source was honoured.
    Cancelled = 0x08,   ///< The last lifecycle operation was cancelled by request.
    Unsupported = 0x10   ///< The source needs something this build does not support.
};

Q_DECLARE_FLAGS(DocumentFacets, DocumentFacet)
Q_DECLARE_OPERATORS_FOR_FLAGS(DocumentFacets)

/// The output axis, kept separate from the document axis exactly as
/// docs/LOOP_SHELL_CONTRACT.md requires.
enum class DocumentOutputState
{
    None,
    Pending,
    Saved
};

/// The five document states of the shell status contract
/// (docs/loop-shell.json, `status_contract.document`).
enum class ShellDocumentStatus
{
    NoDocument,
    Open,
    Modified,
    OutputPending,
    OutputSaved
};

const char* getDocumentStateName(DocumentState state);
const char* getShellDocumentStatusName(ShellDocumentStatus status);

/// Presentation identity for one lifecycle request, using catalog and Core identities.
struct DocumentOperation
{
    CommandInvocationId invocation = InvalidCommandInvocation;
    CommandId command;
    DocumentSource target;
    quint64 generation = 0;
    pdf::PDFRevisionIdentity revision;
    QString jobId;
    bool pending = false;
    CommandResult result;
    CommandResult retryCause;
};

enum class DocumentInspectionState
{
    NotChecked,
    Running,
    Completed,
    Cancelled,
    Failed,
    Stale
};

struct DocumentInspectionToken
{
    quint64 request = 0;
    quint64 generation = 0;
    pdf::PDFRevisionIdentity revision;

    bool operator==(const DocumentInspectionToken&) const = default;
};

struct DocumentPlanIntent
{
    QString operationId;
    QString findingId;
    QString receiptIdentity;
    pdf::PDFRevisionIdentity revision;
};

struct DocumentInspection
{
    DocumentInspectionState state = DocumentInspectionState::NotChecked;
    int progress = 0;
    std::optional<pdf::PreflightInspectionReceipt> receipt;
    QStringList findingIds;
    QString selectedFindingId;
    std::optional<DocumentPlanIntent> planIntent;
    QString failureCode;
    QString failureMessage;
};

/// A value snapshot for both interactive and unattended hosts. A plan intent
/// requests Core planning; it grants no mutation or publication authority.
struct DocumentOperatorState
{
    DocumentState document = DocumentState::Empty;
    DocumentFacets facets;
    DocumentOutputState output = DocumentOutputState::None;
    DocumentSource source;
    QString documentKey;
    pdf::PDFRevisionIdentity revision;
    quint64 generation = 0;
    DocumentInspection inspection;
    QString failureCode;

    bool canActOnInspection() const;
};

struct DocumentPermissions
{
    bool encrypted = false;
    bool print = false;
    bool modify = false;
    bool copy = false;
    bool assemble = false;

    bool allowsCorrection() const { return print && modify && copy && assemble; }
};

/// One presentation-facing document lifecycle.
///
/// The facade owns no PDF truth. pdf::PDFDocumentContext remains the document
/// identity and revision fence, pdf::PDFJobScheduler remains the only scheduler
/// (reached through IJobSubmitter), pdf::PDFDocumentReader and
/// pdf::PDFDocumentWriter remain the only reader and writer. What the facade
/// adds is the state machine a host can bind to, and admission rules that stop a
/// superseded completion from being presented as the current document.
///
/// Every lifecycle operation runs as a CommandCatalog invocation, so there is
/// one command path whether the operator used a menu, a shortcut, or C++.
class DocumentFacade final : public QObject
{
    Q_OBJECT

public:
    /// Command IDs this facade implements. They are Editor action IDs from
    /// docs/loop-shell-actions.json, reused verbatim.
    static const CommandId OpenCommandId;
    static const CommandId CloseCommandId;
    static const CommandId SaveCommandId;
    static const CommandId SaveAsCommandId;

    DocumentFacade(pdf::PDFDocumentContext& context,
                   IJobSubmitter& submitter,
                   IDocumentLoader& loader,
                   IDocumentWriter& writer,
                   CommandCatalog& catalog,
                   QObject* parent = nullptr);

    ~DocumentFacade() override;

    DocumentFacade(const DocumentFacade&) = delete;
    DocumentFacade& operator=(const DocumentFacade&) = delete;

    DocumentState state() const noexcept { return m_state; }
    DocumentFacets facets() const noexcept { return m_facets; }
    DocumentOutputState outputState() const noexcept { return m_outputState; }
    ShellDocumentStatus shellDocumentStatus() const;

    /// The projection onto the shell status contract. Pure, so the mapping can
    /// be pinned by a test rather than re-derived by every host.
    static ShellDocumentStatus projectShellStatus(DocumentState state,
                                                  DocumentFacets facets,
                                                  DocumentOutputState outputState);

    const DocumentSource& source() const noexcept { return m_source; }
    QString typedError() const { return m_typedError; }

    /// Read-only view of the one revision fence.
    const IDocumentRevisionSource& revisionSource() const noexcept { return m_revisionSource; }
    pdf::PDFRevisionIdentity currentRevision() const;

    DocumentOperatorState operatorState() const;
    DocumentPermissions permissions() const;
    std::optional<DocumentInspectionToken> beginInspection(const QString& inputDigest,
                                                           const pdf::PreflightProfileData& profile,
                                                           QString& error);
    bool updateInspectionProgress(const DocumentInspectionToken& token, int progress, QString& error);
    bool completeInspection(const DocumentInspectionToken& token,
                            const pdf::PreflightResult& result,
                            const pdf::PDFEvidenceGraph& evidence,
                            QString& error);
    /// Closes result admission; the submitting host owns cancellation of its job.
    bool cancelInspection(const DocumentInspectionToken& token, QString& error);
    bool failInspection(const DocumentInspectionToken& token, const QString& code,
                        const QString& message, QString& error);
    bool selectFinding(const QString& findingId, QString& error);
    bool requestPlan(const QString& operationId, QString& error);
    void clearPlanIntent();

    /// Advances on every document replacement and on every close. A completion
    /// carrying an older generation is rejected rather than admitted.
    quint64 documentGeneration() const noexcept { return m_generation; }

    /// Terminal job results dropped because their request or document generation
    /// was no longer current. A correct run of the lifecycle never admits one.
    int rejectedCompletionCount() const noexcept { return m_rejectedCompletions; }

    /// Marks the current document modified. Kept explicit rather than inferred:
    /// mutation belongs to the commands that mutate, and this is how they tell
    /// the lifecycle.
    void markModified();

    CommandInvocationId open(const QString& path);

    /// Re-opens the current source. Replacement, not a refresh: the old identity
    /// is dropped before the new one is established.
    CommandInvocationId reopen();

    CommandInvocationId close();
    CommandInvocationId save();
    CommandInvocationId saveAs(const QString& path);

    DocumentOperation operation() const { return m_operation; }

    /// Retries a failed or cancelled current request with a new invocation/job.
    /// The prior terminal result remains available as the retry's cause.
    CommandInvocationId retry();

    /// Requests cancellation of the lifecycle operation in flight.
    bool cancelPendingOperation();

signals:
    void operatorStateChanged();
    void stateChanged(pdfinteraction::DocumentState state);
    void facetsChanged(pdfinteraction::DocumentFacets facets);
    void operationChanged();

    /// Emitted after the new identity is established and the old one dropped.
    void documentReplaced(quint64 generation);

    void documentClosed(quint64 generation);

private:
    bool registerHandlers();

    void beginOpen(CommandInvocationId invocation, const DocumentSource& source);
    void beginSave(CommandInvocationId invocation, const CommandId& command, const DocumentSource& target);
    void beginOperation(CommandInvocationId invocation, const CommandId& command, const DocumentSource& target);
    void performClose(CommandInvocationId invocation);

    void admitLoadResult(CommandInvocationId invocation, quint64 generation, DocumentLoadResult result);
    void admitWriteResult(CommandInvocationId invocation,
                          quint64 generation,
                          DocumentSource target,
                          DocumentWriteResult result);

    void requestCancellation(CommandInvocationId invocation);

    void detachDocument();
    void supersedePending(CommandTerminalState state, QString typedError);
    void setState(DocumentState state);
    void setFacets(DocumentFacets facets);
    void setOutputState(DocumentOutputState outputState);
    void updateAvailability();
    void finishPending(CommandInvocationId invocation, CommandTerminalState state, QString typedError);
    bool admitInspectionTransition(const DocumentInspectionToken& token, QString& error) const;
    void invalidateInspection();

    /// The context is reached through the P4-S1 seam rather than held directly,
    /// so a destroyed context degrades to an invalid revision instead of a
    /// dangling read.
    pdf::PDFDocumentContext* context() const { return m_revisionSource.context(); }

    IJobSubmitter* m_submitter = nullptr;
    IDocumentLoader* m_loader = nullptr;
    IDocumentWriter* m_writer = nullptr;
    QPointer<CommandCatalog> m_catalog;

    PDFDocumentContextSource m_revisionSource;
    std::shared_ptr<JobRelay> m_relay;

    DocumentState m_state = DocumentState::Empty;
    DocumentFacets m_facets;
    DocumentOutputState m_outputState = DocumentOutputState::None;
    DocumentSource m_source;
    QString m_typedError;
    DocumentInspection m_inspection;
    DocumentInspectionToken m_inspectionToken;
    pdf::PreflightProfileData m_inspectionProfile;
    QString m_inspectionInputDigest;
    quint64 m_inspectionRequest = 0;

    quint64 m_generation = 0;
    int m_rejectedCompletions = 0;

    /// The key the current revision was published under. Captured so close()
    /// clears the entry it created, rather than whatever key the context reports
    /// after the document is already gone.
    QString m_publishedKey;

    CommandInvocationId m_pendingInvocation = InvalidCommandInvocation;
    QString m_pendingJobId;
    DocumentOperation m_operation;
    CommandResult m_retryCause;

    bool m_handlersRegistered = false;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::DocumentState)

#endif   // DOCUMENTFACADE_H
