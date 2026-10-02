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

#include "documentfacade.h"
#include "pdfartifactidentity.h"

#include <QSet>

#include <utility>

namespace pdfinteraction
{

const char* getDocumentStateName(DocumentState state)
{
    switch (state)
    {
        case DocumentState::Empty:
            return "empty";
        case DocumentState::Opening:
            return "opening";
        case DocumentState::Ready:
            return "ready";
        case DocumentState::Closing:
            return "closing";
        case DocumentState::Error:
            return "error";
    }

    return "empty";
}

const char* getShellDocumentStatusName(ShellDocumentStatus status)
{
    switch (status)
    {
        case ShellDocumentStatus::NoDocument:
            return "NO_DOCUMENT";
        case ShellDocumentStatus::Open:
            return "OPEN";
        case ShellDocumentStatus::Modified:
            return "MODIFIED";
        case ShellDocumentStatus::OutputPending:
            return "OUTPUT_PENDING";
        case ShellDocumentStatus::OutputSaved:
            return "OUTPUT_SAVED";
    }

    return "NO_DOCUMENT";
}

ShellDocumentStatus DocumentFacade::shellDocumentStatus() const
{
    return projectShellStatus(m_state, m_facets, m_outputState);
}

ShellDocumentStatus DocumentFacade::projectShellStatus(DocumentState state,
                                                       DocumentFacets facets,
                                                       DocumentOutputState outputState)
{
    if (state != DocumentState::Ready)
    {
        return ShellDocumentStatus::NoDocument;
    }

    if (outputState == DocumentOutputState::Pending)
    {
        return ShellDocumentStatus::OutputPending;
    }

    if (facets.testFlag(DocumentFacet::Dirty))
    {
        return ShellDocumentStatus::Modified;
    }

    if (outputState == DocumentOutputState::Saved)
    {
        return ShellDocumentStatus::OutputSaved;
    }

    return ShellDocumentStatus::Open;
}

pdf::PDFRevisionIdentity DocumentFacade::currentRevision() const
{
    return m_revisionSource.currentRevision();
}

bool DocumentOperatorState::canActOnInspection() const
{
    return document == DocumentState::Ready && revision.isValid() &&
           output != DocumentOutputState::Pending &&
           inspection.state == DocumentInspectionState::Completed && inspection.receipt &&
           inspection.receipt->revision == revision &&
           inspection.receipt->verdict.state != pdf::PreflightVerdictState::Error;
}

DocumentOperatorState DocumentFacade::operatorState() const
{
    DocumentOperatorState snapshot;
    snapshot.document = m_state;
    snapshot.facets = m_facets;
    snapshot.output = m_outputState;
    snapshot.source = m_source;
    snapshot.generation = m_generation;
    snapshot.failureCode = m_typedError;
    snapshot.inspection = m_inspection;
    if (m_state == DocumentState::Ready)
    {
        snapshot.revision = currentRevision();
        snapshot.documentKey = m_revisionSource.documentKey();
        if (!snapshot.revision.isValid())
        {
            snapshot.document = DocumentState::Error;
            snapshot.failureCode = QStringLiteral("document/context-gone");
        }
    }
    if (!snapshot.revision.isValid())
    {
        snapshot.inspection.receipt.reset();
        snapshot.inspection.findingIds.clear();
        snapshot.inspection.selectedFindingId.clear();
        snapshot.inspection.planIntent.reset();
    }
    return snapshot;
}

std::optional<DocumentInspectionToken> DocumentFacade::beginInspection(
    const QString& inputDigest, const pdf::PreflightProfileData& profile, QString& error)
{
    error.clear();
    if (m_state != DocumentState::Ready || !currentRevision().isValid() ||
        m_pendingInvocation != InvalidCommandInvocation ||
        m_inspection.state == DocumentInspectionState::Running)
    {
        error = QStringLiteral("inspection/document-not-ready");
        return std::nullopt;
    }
    if (!pdf::isPDFSha256(inputDigest) || !pdf::isPDFSha256(profile.effectiveDigest))
    {
        error = QStringLiteral("inspection/invalid-provenance");
        return std::nullopt;
    }
    m_inspectionProfile = profile;
    m_inspectionInputDigest = inputDigest;
    const DocumentInspectionToken token{ ++m_inspectionRequest, m_generation, currentRevision() };
    m_inspectionToken = token;
    m_inspection = {};
    m_inspection.state = DocumentInspectionState::Running;
    Q_EMIT operatorStateChanged();
    return token;
}

bool DocumentFacade::admitInspectionTransition(const DocumentInspectionToken& token, QString& error) const
{
    error.clear();
    if (m_state != DocumentState::Ready || !currentRevision().isValid() ||
        m_inspection.state != DocumentInspectionState::Running ||
        token != m_inspectionToken || token.generation != m_generation ||
        token.revision != currentRevision())
    {
        error = QStringLiteral("inspection/not-current-or-terminal");
        return false;
    }
    return true;
}

bool DocumentFacade::updateInspectionProgress(const DocumentInspectionToken& token, int progress, QString& error)
{
    if (!admitInspectionTransition(token, error))
    {
        return false;
    }
    if (progress < m_inspection.progress || progress > 100)
    {
        error = QStringLiteral("inspection/invalid-progress");
        return false;
    }
    m_inspection.progress = progress;
    Q_EMIT operatorStateChanged();
    return true;
}

bool DocumentFacade::completeInspection(const DocumentInspectionToken& token,
                                        const pdf::PreflightResult& result,
                                        const pdf::PDFEvidenceGraph& evidence, QString& error)
{
    if (!admitInspectionTransition(token, error))
    {
        return false;
    }
    pdf::PreflightInspectionReceipt receipt;
    if (!pdf::buildPreflightInspectionReceipt(result, m_inspectionProfile, token.revision, evidence, receipt, error) ||
        !pdf::validatePreflightInspectionReceipt(receipt, m_inspectionInputDigest, token.revision, m_inspectionProfile, error))
    {
        return false;
    }
    QStringList findingIds;
    QSet<QString> seen;
    for (const auto& findings : { result.errors, result.warnings })
    {
        for (const auto& finding : findings)
        {
            const QString id = finding.stableId();
            if (id.isEmpty() || seen.contains(id))
            {
                error = QStringLiteral("inspection/invalid-finding-identity");
                return false;
            }
            seen.insert(id);
            findingIds.append(id);
        }
    }
    m_inspection.receipt = std::move(receipt);
    m_inspection.findingIds = std::move(findingIds);
    m_inspection.progress = 100;
    m_inspection.state = DocumentInspectionState::Completed;
    Q_EMIT operatorStateChanged();
    return true;
}

bool DocumentFacade::cancelInspection(const DocumentInspectionToken& token, QString& error)
{
    if (!admitInspectionTransition(token, error))
    {
        return false;
    }
    m_inspection.state = DocumentInspectionState::Cancelled;
    Q_EMIT operatorStateChanged();
    return true;
}

bool DocumentFacade::failInspection(const DocumentInspectionToken& token, const QString& code,
                                    const QString& message, QString& error)
{
    if (!admitInspectionTransition(token, error))
    {
        return false;
    }
    if (code.isEmpty() || message.isEmpty())
    {
        error = QStringLiteral("inspection/missing-failure");
        return false;
    }
    m_inspection.state = DocumentInspectionState::Failed;
    m_inspection.failureCode = code;
    m_inspection.failureMessage = message;
    Q_EMIT operatorStateChanged();
    return true;
}

bool DocumentFacade::selectFinding(const QString& findingId, QString& error)
{
    error.clear();
    if (!operatorState().canActOnInspection())
    {
        error = QStringLiteral("inspection/not-actionable");
        return false;
    }
    if (!findingId.isEmpty() && !m_inspection.findingIds.contains(findingId))
    {
        error = QStringLiteral("inspection/unknown-finding");
        return false;
    }
    m_inspection.selectedFindingId = findingId;
    m_inspection.planIntent.reset();
    Q_EMIT operatorStateChanged();
    return true;
}

bool DocumentFacade::requestPlan(const QString& operationId, QString& error)
{
    error.clear();
    if (!operatorState().canActOnInspection() || m_inspection.selectedFindingId.isEmpty())
    {
        error = QStringLiteral("inspection/no-actionable-selection");
        return false;
    }
    if (operationId.isEmpty())
    {
        error = QStringLiteral("inspection/missing-operation");
        return false;
    }
    m_inspection.planIntent = DocumentPlanIntent{ operationId, m_inspection.selectedFindingId,
                                                  m_inspection.receipt->identity, currentRevision() };
    Q_EMIT operatorStateChanged();
    return true;
}

void DocumentFacade::clearPlanIntent()
{
    if (m_inspection.planIntent)
    {
        m_inspection.planIntent.reset();
        Q_EMIT operatorStateChanged();
    }
}

void DocumentFacade::invalidateInspection()
{
    const bool inspected = m_inspection.state != DocumentInspectionState::NotChecked;
    m_inspection = {};
    if (inspected)
    {
        m_inspection.state = DocumentInspectionState::Stale;
    }
    Q_EMIT operatorStateChanged();
}

void DocumentFacade::setState(DocumentState state)
{
    if (m_state == state)
    {
        return;
    }

    m_state = state;
    Q_EMIT stateChanged(m_state);
    Q_EMIT operatorStateChanged();
}

void DocumentFacade::setFacets(DocumentFacets facets)
{
    if (m_facets == facets)
    {
        return;
    }

    m_facets = facets;
    Q_EMIT facetsChanged(m_facets);
    Q_EMIT operatorStateChanged();
}

void DocumentFacade::setOutputState(DocumentOutputState outputState)
{
    m_outputState = outputState;
    Q_EMIT operatorStateChanged();
}

void DocumentFacade::updateAvailability()
{
    Q_EMIT operatorStateChanged();
    if (!m_catalog || !m_handlersRegistered)
    {
        return;
    }

    const bool busy = m_pendingInvocation != InvalidCommandInvocation;
    const bool hasDocument = m_state == DocumentState::Ready && context() && context()->getDocument();

    // Open and Close are escape/supersede paths and stay available while work
    // is in flight. Save commands require a ready document and exclusive output
    // admission.
    m_catalog->setEnabledBatch({
        { OpenCommandId, true },
        { CloseCommandId, hasDocument || m_state == DocumentState::Opening || busy },
        { SaveCommandId, hasDocument && !busy && m_source.isValid() },
        { SaveAsCommandId, hasDocument && !busy },
    });
}

void DocumentFacade::finishPending(CommandInvocationId invocation,
                                   CommandTerminalState state,
                                   QString typedError)
{
    if (m_pendingInvocation == invocation)
    {
        m_pendingInvocation = InvalidCommandInvocation;
        m_pendingWorkStarted.reset();
    }

    if (m_catalog)
    {
        m_catalog->finishInvocation(invocation, state, std::move(typedError));
    }
}

}   // namespace pdfinteraction
