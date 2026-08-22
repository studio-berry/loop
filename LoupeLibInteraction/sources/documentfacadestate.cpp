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

#include <utility>

namespace pdfinteraction
{

const char* getDocumentStateName(DocumentState state)
{
    switch (state)
    {
        case DocumentState::Empty: return "empty";
        case DocumentState::Opening: return "opening";
        case DocumentState::Ready: return "ready";
        case DocumentState::Closing: return "closing";
        case DocumentState::Error: return "error";
    }

    return "empty";
}

const char* getShellDocumentStatusName(ShellDocumentStatus status)
{
    switch (status)
    {
        case ShellDocumentStatus::NoDocument: return "NO_DOCUMENT";
        case ShellDocumentStatus::Open: return "OPEN";
        case ShellDocumentStatus::Modified: return "MODIFIED";
        case ShellDocumentStatus::OutputPending: return "OUTPUT_PENDING";
        case ShellDocumentStatus::OutputSaved: return "OUTPUT_SAVED";
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

void DocumentFacade::setState(DocumentState state)
{
    if (m_state == state)
    {
        return;
    }

    m_state = state;
    Q_EMIT stateChanged(m_state);
}

void DocumentFacade::setFacets(DocumentFacets facets)
{
    if (m_facets == facets)
    {
        return;
    }

    m_facets = facets;
    Q_EMIT facetsChanged(m_facets);
}

void DocumentFacade::setOutputState(DocumentOutputState outputState)
{
    m_outputState = outputState;
}

void DocumentFacade::updateAvailability()
{
    if (!m_catalog || !m_handlersRegistered)
    {
        return;
    }

    const bool busy = m_pendingInvocation != InvalidCommandInvocation;
    const bool hasDocument = m_state == DocumentState::Ready && context() && context()->getDocument();

    // Open and Close are escape/supersede paths and stay available while work
    // is in flight. Save commands require a ready document and exclusive output
    // admission.
    m_catalog->setEnabled(OpenCommandId, true);
    m_catalog->setEnabled(CloseCommandId,
                          hasDocument || m_state == DocumentState::Opening || busy);
    m_catalog->setEnabled(SaveCommandId, hasDocument && !busy && m_source.isValid());
    m_catalog->setEnabled(SaveAsCommandId, hasDocument && !busy);
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
