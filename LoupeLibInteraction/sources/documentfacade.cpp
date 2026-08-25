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

#include "documentfacade.h"

#include <QVariant>

#include <utility>

namespace pdfinteraction
{

const CommandId DocumentFacade::OpenCommandId = QStringLiteral("actionOpen");
const CommandId DocumentFacade::CloseCommandId = QStringLiteral("actionClose");
const CommandId DocumentFacade::SaveCommandId = QStringLiteral("actionSave");
const CommandId DocumentFacade::SaveAsCommandId = QStringLiteral("actionSave_As");

namespace
{

constexpr const char* PathParameter = "path";

}   // namespace

DocumentFacade::DocumentFacade(pdf::PDFDocumentContext& context,
                               IJobSubmitter& submitter,
                               IDocumentLoader& loader,
                               IDocumentWriter& writer,
                               CommandCatalog& catalog,
                               QObject* parent) :
    QObject(parent),
    m_submitter(&submitter),
    m_loader(&loader),
    m_writer(&writer),
    m_catalog(&catalog),
    m_revisionSource(&context),
    m_relay(new JobRelay, [](JobRelay* relay)
            { relay->deleteLater(); })
{
    m_handlersRegistered = registerHandlers();
    updateAvailability();
}

DocumentFacade::~DocumentFacade()
{
    // Order matters. Detaching first guarantees no queued completion can reach
    // this object while it is being torn down; the relay itself stays alive
    // until the last worker lambda releases its shared_ptr.
    m_relay->detach();

    if (!m_pendingJobId.isEmpty())
    {
        m_submitter->cancel(m_pendingJobId);
        m_pendingJobId.clear();
    }

    if (!m_publishedKey.isEmpty())
    {
        m_submitter->clearCurrentRevision(m_publishedKey);
        m_publishedKey.clear();
    }

    if (m_catalog)
    {
        m_catalog->clearHandler(OpenCommandId);
        m_catalog->clearHandler(CloseCommandId);
        m_catalog->clearHandler(SaveCommandId);
        m_catalog->clearHandler(SaveAsCommandId);

        if (m_pendingInvocation != InvalidCommandInvocation)
        {
            m_catalog->finishInvocation(m_pendingInvocation,
                                        CommandTerminalState::Cancelled,
                                        QStringLiteral("document/facade-destroyed"));
            m_pendingInvocation = InvalidCommandInvocation;
        }
    }
}

bool DocumentFacade::registerHandlers()
{
    if (!m_catalog)
    {
        return false;
    }

    bool registered = true;
    CommandCatalog::Handler open;
    open.invoke = [this](CommandInvocationId invocation, const QVariantMap& parameters)
    {
        DocumentSource source;
        source.path = parameters.value(QLatin1String(PathParameter)).toString();
        beginOpen(invocation, source);
    };
    open.cancel = [this](CommandInvocationId invocation)
    { requestCancellation(invocation); };
    registered = m_catalog->setHandler(OpenCommandId, std::move(open)) && registered;

    CommandCatalog::Handler close;
    close.invoke = [this](CommandInvocationId invocation, const QVariantMap&)
    { performClose(invocation); };
    registered = m_catalog->setHandler(CloseCommandId, std::move(close)) && registered;

    CommandCatalog::Handler save;
    save.invoke = [this](CommandInvocationId invocation, const QVariantMap&)
    { beginSave(invocation, m_source); };
    save.cancel = [this](CommandInvocationId invocation)
    { requestCancellation(invocation); };
    registered = m_catalog->setHandler(SaveCommandId, std::move(save)) && registered;

    CommandCatalog::Handler saveAs;
    saveAs.invoke = [this](CommandInvocationId invocation, const QVariantMap& parameters)
    {
        DocumentSource target;
        target.path = parameters.value(QLatin1String(PathParameter)).toString();
        beginSave(invocation, target);
    };
    saveAs.cancel = [this](CommandInvocationId invocation)
    { requestCancellation(invocation); };
    registered = m_catalog->setHandler(SaveAsCommandId, std::move(saveAs)) && registered;

    if (!registered)
    {
        m_catalog->clearHandler(OpenCommandId);
        m_catalog->clearHandler(CloseCommandId);
        m_catalog->clearHandler(SaveCommandId);
        m_catalog->clearHandler(SaveAsCommandId);
    }

    return registered;
}

void DocumentFacade::requestCancellation(CommandInvocationId invocation)
{
    if (invocation != m_pendingInvocation || m_pendingJobId.isEmpty())
    {
        return;
    }

    const QString jobId = m_pendingJobId;
    const std::shared_ptr<std::atomic_bool> workStarted = m_pendingWorkStarted;
    m_submitter->cancel(jobId);

    // Queued rather than resolved inline: work that is already running still
    // reports its own terminal state, and that report must be allowed to arrive
    // first.
    m_relay->post([this, invocation, jobId, workStarted]()
                  { resolveCancellation(invocation, jobId, workStarted); });
}

void DocumentFacade::resolveCancellation(CommandInvocationId invocation,
                                         QString jobId,
                                         std::shared_ptr<std::atomic_bool> workStarted)
{
    if (invocation != m_pendingInvocation)
    {
        // The work already reported. Cancellation was still terminal, just not
        // through this path.
        return;
    }

    if (workStarted && workStarted->load(std::memory_order_acquire))
    {
        return;
    }

    const pdf::PDFJobSnapshot snapshot = m_submitter->snapshot(jobId);
    if (snapshot.status == pdf::PDFJobStatus::Queued || snapshot.status == pdf::PDFJobStatus::Running)
    {
        m_relay->post([this, invocation, jobId, workStarted]()
                      { resolveCancellation(invocation, jobId, workStarted); });
        return;
    }

    // Terminal without the work ever starting: the scheduler dropped it from the
    // queue, so no completion is coming and the invocation would otherwise never
    // reach a terminal state.
    m_pendingJobId.clear();
    m_typedError = QStringLiteral("document/cancelled");

    if (m_state == DocumentState::Opening)
    {
        m_source = DocumentSource();
        setFacets(DocumentFacets(DocumentFacet::Cancelled));
        setState(DocumentState::Empty);
    }
    else
    {
        setOutputState(DocumentOutputState::None);
        setFacets(m_facets | DocumentFacet::Cancelled);
    }

    finishPending(invocation, CommandTerminalState::Cancelled, m_typedError);
    updateAvailability();
}

void DocumentFacade::markModified()
{
    pdf::PDFDocumentContext* documentContext = context();
    if (m_state != DocumentState::Ready || !documentContext)
    {
        return;
    }

    // A queued save was fenced against the revision that is about to be
    // superseded. The scheduler would discard it silently, leaving the
    // invocation without a terminal state, so it is superseded here instead.
    if (m_outputState == DocumentOutputState::Pending)
    {
        supersedePending(CommandTerminalState::Cancelled, QStringLiteral("document/superseded"));
    }

    documentContext->markModified();

    if (!m_publishedKey.isEmpty())
    {
        m_submitter->publishCurrentRevision(m_publishedKey, m_revisionSource.currentRevision());
    }

    setOutputState(DocumentOutputState::None);
    setFacets(m_facets | DocumentFacet::Dirty);
    updateAvailability();
}

CommandInvocationId DocumentFacade::open(const QString& path)
{
    QVariantMap parameters;
    parameters.insert(QLatin1String(PathParameter), path);
    return m_catalog ? m_catalog->invoke(OpenCommandId, parameters) : InvalidCommandInvocation;
}

CommandInvocationId DocumentFacade::reopen()
{
    if (!m_source.isValid())
    {
        return InvalidCommandInvocation;
    }

    return open(m_source.path);
}

CommandInvocationId DocumentFacade::close()
{
    return m_catalog ? m_catalog->invoke(CloseCommandId) : InvalidCommandInvocation;
}

CommandInvocationId DocumentFacade::save()
{
    return m_catalog ? m_catalog->invoke(SaveCommandId) : InvalidCommandInvocation;
}

CommandInvocationId DocumentFacade::saveAs(const QString& path)
{
    QVariantMap parameters;
    parameters.insert(QLatin1String(PathParameter), path);
    return m_catalog ? m_catalog->invoke(SaveAsCommandId, parameters) : InvalidCommandInvocation;
}

bool DocumentFacade::cancelPendingOperation()
{
    if (m_pendingInvocation == InvalidCommandInvocation || !m_catalog)
    {
        return false;
    }

    return m_catalog->cancelInvocation(m_pendingInvocation);
}

void DocumentFacade::beginOpen(CommandInvocationId invocation, const DocumentSource& source)
{
    if (!source.isValid())
    {
        finishPending(invocation,
                      CommandTerminalState::Failed,
                      QStringLiteral("document/invalid-source"));
        return;
    }

    supersedePending(CommandTerminalState::Cancelled, QStringLiteral("document/superseded"));

    // Replacement drops the previous identity before any new work is issued, so
    // nothing computed against it can be admitted into the new one.
    detachDocument();

    m_source = source;
    m_typedError.clear();
    setFacets({});
    setOutputState(DocumentOutputState::None);
    setState(DocumentState::Opening);

    m_pendingInvocation = invocation;

    const quint64 generation = m_generation;
    const std::shared_ptr<JobRelay> relay = m_relay;
    IDocumentLoader* loader = m_loader;
    const DocumentSource captured = source;

    m_pendingWorkStarted = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> workStarted = m_pendingWorkStarted;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;
    // Deliberately no documentKey: there is no document yet, so there is no
    // revision for the scheduler to fence this job against. Supersession is
    // fenced by `generation` at admission instead.

    m_pendingJobId = m_submitter->submit(
        spec,
        [this, relay, loader, captured, generation, invocation, workStarted](pdf::PDFJobContext& jobContext)
        {
            workStarted->store(true, std::memory_order_release);
            DocumentLoadResult result = loader->load(captured, jobContext);
            relay->post([this, invocation, generation, result = std::move(result)]() mutable
                        { admitLoadResult(invocation, generation, std::move(result)); });
        });

    updateAvailability();
}

void DocumentFacade::beginSave(CommandInvocationId invocation, const DocumentSource& target)
{
    pdf::PDFDocumentContext* documentContext = context();
    if (m_state != DocumentState::Ready || !documentContext || !documentContext->getDocument())
    {
        finishPending(invocation,
                      CommandTerminalState::Unavailable,
                      QStringLiteral("document/not-ready"));
        return;
    }

    if (!target.isValid())
    {
        finishPending(invocation,
                      CommandTerminalState::Failed,
                      QStringLiteral("document/invalid-target"));
        return;
    }

    supersedePending(CommandTerminalState::Cancelled, QStringLiteral("document/superseded"));

    m_pendingInvocation = invocation;
    setOutputState(DocumentOutputState::Pending);

    const quint64 generation = m_generation;
    const std::shared_ptr<JobRelay> relay = m_relay;
    IDocumentWriter* writer = m_writer;
    const DocumentSource captured = target;

    // The shared pointer keeps the document alive for the worker even if the
    // context replaces it mid-write. It is not a second document truth: nothing
    // reads it back into the context.
    const pdf::PDFDocumentPointer document = documentContext->getDocumentPointer();
    const pdf::PDFRevisionIdentity revision = m_revisionSource.currentRevision();

    m_pendingWorkStarted = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> workStarted = m_pendingWorkStarted;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Export;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = m_publishedKey;
    spec.documentRevision = revision.toString();
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    m_pendingJobId = m_submitter->submit(
        spec,
        [this, relay, writer, captured, document, generation, invocation, workStarted](pdf::PDFJobContext& jobContext)
        {
            workStarted->store(true, std::memory_order_release);
            DocumentWriteResult result = writer->write(captured, document.data(), jobContext);
            relay->post([this, invocation, generation, captured, result = std::move(result)]() mutable
                        { admitWriteResult(invocation, generation, captured, std::move(result)); });
        });

    updateAvailability();
}

void DocumentFacade::performClose(CommandInvocationId invocation)
{
    supersedePending(CommandTerminalState::Cancelled, QStringLiteral("document/closed"));

    setState(DocumentState::Closing);
    detachDocument();

    m_source = DocumentSource();
    m_typedError.clear();
    setFacets({});
    setOutputState(DocumentOutputState::None);
    setState(DocumentState::Empty);

    finishPending(invocation, CommandTerminalState::Completed, QString());
    updateAvailability();
}

void DocumentFacade::admitLoadResult(CommandInvocationId invocation,
                                     quint64 generation,
                                     DocumentLoadResult result)
{
    if (generation != m_generation)
    {
        // The document this result was requested for is gone. It is counted and
        // dropped, never patched into the current document.
        ++m_rejectedCompletions;
        return;
    }

    if (invocation != m_pendingInvocation)
    {
        // Same document, but the invocation already reached a terminal state —
        // a cancellation the scheduler resolved first. Not a stale admission.
        return;
    }

    m_pendingJobId.clear();

    pdf::PDFDocumentContext* documentContext = context();
    if (!documentContext)
    {
        ++m_rejectedCompletions;
        m_typedError = QStringLiteral("document/context-gone");
        setState(DocumentState::Error);
        finishPending(invocation, CommandTerminalState::Failed, m_typedError);
        updateAvailability();
        return;
    }

    switch (result.outcome)
    {
        case DocumentLoadOutcome::Loaded:
        {
            documentContext->setDocument(result.document, pdf::PDFModifiedDocument::Reset);

            m_publishedKey = m_revisionSource.documentKey();
            if (!m_publishedKey.isEmpty())
            {
                m_submitter->publishCurrentRevision(m_publishedKey,
                                                    m_revisionSource.currentRevision());
            }

            DocumentFacets facets;
            if (result.incomplete)
            {
                facets |= DocumentFacet::Incomplete;
            }
            if (result.unsupported)
            {
                facets |= DocumentFacet::Unsupported;
            }

            m_typedError.clear();
            setFacets(facets);
            setOutputState(DocumentOutputState::None);
            setState(DocumentState::Ready);

            Q_EMIT documentReplaced(m_generation);
            finishPending(invocation, CommandTerminalState::Completed, QString());
            break;
        }

        case DocumentLoadOutcome::Cancelled:
        {
            m_typedError = result.typedError;
            m_source = DocumentSource();
            setFacets(DocumentFacets(DocumentFacet::Cancelled));
            setState(DocumentState::Empty);
            finishPending(invocation, CommandTerminalState::Cancelled, result.typedError);
            break;
        }

        case DocumentLoadOutcome::Failed:
        {
            m_typedError = result.typedError;
            DocumentFacets facets;
            if (result.unsupported)
            {
                facets |= DocumentFacet::Unsupported;
            }
            setFacets(facets);
            setState(DocumentState::Error);
            finishPending(invocation, CommandTerminalState::Failed, result.typedError);
            break;
        }
    }

    updateAvailability();
}

void DocumentFacade::admitWriteResult(CommandInvocationId invocation,
                                      quint64 generation,
                                      DocumentSource target,
                                      DocumentWriteResult result)
{
    if (generation != m_generation)
    {
        ++m_rejectedCompletions;
        return;
    }

    if (invocation != m_pendingInvocation)
    {
        return;
    }

    m_pendingJobId.clear();

    switch (result.outcome)
    {
        case DocumentWriteOutcome::Written:
            m_source = target;
            m_typedError.clear();
            setFacets(m_facets & ~DocumentFacets(DocumentFacet::Dirty));
            setOutputState(DocumentOutputState::Saved);
            finishPending(invocation, CommandTerminalState::Completed, QString());
            break;

        case DocumentWriteOutcome::Cancelled:
            m_typedError = result.typedError;
            setOutputState(DocumentOutputState::None);
            setFacets(m_facets | DocumentFacet::Cancelled);
            finishPending(invocation, CommandTerminalState::Cancelled, result.typedError);
            break;

        case DocumentWriteOutcome::Failed:
            // The document is untouched and still open; only the output axis
            // failed, so the base state stays Ready.
            m_typedError = result.typedError;
            setOutputState(DocumentOutputState::None);
            finishPending(invocation, CommandTerminalState::Failed, result.typedError);
            break;
    }

    updateAvailability();
}

void DocumentFacade::detachDocument()
{
    if (!m_publishedKey.isEmpty())
    {
        // A key with no entry is never stale, so this belongs at close and at
        // replacement, not between submissions.
        m_submitter->clearCurrentRevision(m_publishedKey);
        m_publishedKey.clear();
    }

    pdf::PDFDocumentContext* documentContext = context();
    const bool hadDocument = documentContext && documentContext->getDocument();
    if (hadDocument)
    {
        documentContext->setDocument(pdf::PDFDocumentPointer(), pdf::PDFModifiedDocument::Reset);
    }

    ++m_generation;

    // Availability computed against the previous document is not evidence about
    // the next one.
    if (m_catalog)
    {
        m_catalog->invalidateAvailability();
    }

    if (hadDocument)
    {
        Q_EMIT documentClosed(m_generation);
    }
}

void DocumentFacade::supersedePending(CommandTerminalState state, QString typedError)
{
    if (m_pendingInvocation == InvalidCommandInvocation)
    {
        return;
    }

    if (!m_pendingJobId.isEmpty())
    {
        m_submitter->cancel(m_pendingJobId);
        m_pendingJobId.clear();
    }

    const CommandInvocationId invocation = m_pendingInvocation;
    m_pendingInvocation = InvalidCommandInvocation;
    m_pendingWorkStarted.reset();

    if (m_outputState == DocumentOutputState::Pending)
    {
        setOutputState(DocumentOutputState::None);
    }

    if (m_catalog)
    {
        m_catalog->finishInvocation(invocation, state, std::move(typedError));
    }
}

}   // namespace pdfinteraction
