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

#include "pdfannotationauthoring.h"
#include "pdfauthoringhistory.h"
#include "pdfdocumentwriter.h"
#include "pdfresourcebudget.h"
#include "pdfsecurityhandler.h"

#include <QBuffer>
#include <QJsonObject>

namespace pdfinteraction
{

namespace
{

/// Serializes one revision for the operation-history artifact store. A
/// revision that cannot be serialized cannot be recorded, and an unrecordable
/// edit is refused before anything is mutated.
bool serializeRevision(const pdf::PDFDocument* document, QByteArray* bytes)
{
    if (!document || !bytes)
    {
        return false;
    }

    QBuffer buffer(bytes);
    if (!buffer.open(QIODevice::WriteOnly))
    {
        return false;
    }

    pdf::PDFDocumentWriter writer(nullptr);
    return bool(writer.write(&buffer, document));
}

QString operationIdForSubtype(pdf::AnnotationAuthoringSubtype subtype)
{
    return QStringLiteral("annotation.create.%1").arg(QLatin1String(pdf::getAnnotationAuthoringSubtypeName(subtype)));
}

}   // namespace

const char* getDirectEditKindName(DirectEditKind kind)
{
    switch (kind)
    {
        case DirectEditKind::CreateAnnotation:
            return "create-annotation";
    }

    return "invalid";
}

bool parseDirectEditKind(const QString& name, DirectEditKind* kind)
{
    if (name == QLatin1String("create-annotation"))
    {
        if (kind)
        {
            *kind = DirectEditKind::CreateAnnotation;
        }
        return true;
    }

    return false;
}

DirectEditOutcome DocumentFacade::applyDirectEdit(const DirectEditRequest& request)
{
    DirectEditOutcome outcome;
    outcome.revision = currentRevision();

    if (request.kind != DirectEditKind::CreateAnnotation)
    {
        outcome.typedError = QStringLiteral("direct-edit/unsupported-kind");
        return outcome;
    }
    if (request.operatorIdentity.trimmed().isEmpty())
    {
        outcome.typedError = QStringLiteral("direct-edit/missing-operator");
        return outcome;
    }

    pdf::PDFDocumentContext* documentContext = context();
    if (m_state != DocumentState::Ready || !documentContext || !documentContext->getDocument())
    {
        outcome.typedError = QStringLiteral("direct-edit/document-not-ready");
        return outcome;
    }

    // The fence: the edit carries the complete revision it was begun against,
    // and a mismatch fails closed before any mutation (invariant I07).
    if (!request.revision.isValid() || request.revision != currentRevision())
    {
        outcome.typedError = QStringLiteral("direct-edit/stale-revision");
        return outcome;
    }

    // Modify-annotations is the PDF permission bit this edit family needs.
    // The rule lives in permissions() — owner authorization satisfies it inside
    // the security handler, and a document without a security handler is
    // reported as restricted — so the gateway and the shell's availability gate
    // cannot disagree about what an editable document is.
    if (!permissions().modifyAnnotations)
    {
        outcome.typedError = QStringLiteral("direct-edit/permission-denied");
        return outcome;
    }

    // The pre-edit revision is the rollback target of this edit; it is captured
    // before the mutation so a failure to record refuses the edit instead of
    // leaving an unrecordable revision.
    QByteArray inputBytes;
    if (!serializeRevision(documentContext->getDocument(), &inputBytes))
    {
        outcome.typedError = QStringLiteral("direct-edit/revision-not-serializable");
        return outcome;
    }

    pdf::AnnotationAuthoringResult authoring;
    pdf::PDFDocumentPointer modified =
        pdf::createAuthoredAnnotation(documentContext->getDocument(), request.annotation, &authoring);
    if (!modified)
    {
        outcome.typedError = authoring.typedError;
        return outcome;
    }

    QByteArray outputBytes;
    if (!serializeRevision(modified.data(), &outputBytes))
    {
        outcome.typedError = QStringLiteral("direct-edit/revision-not-serializable");
        return outcome;
    }

    // One accepted edit is exactly one revision: the context is the revision
    // authority, so the document is replaced (not marked) and the revision
    // advances once.
    try
    {
        documentContext->setDocument(modified, pdf::PDFModifiedDocument::Annotation);
    }
    catch (const pdf::PDFResourceBudgetExceededException&)
    {
        outcome.typedError = QStringLiteral("direct-edit/admission-failed");
        return outcome;
    }
    catch (const pdf::PDFException&)
    {
        outcome.typedError = QStringLiteral("direct-edit/admission-failed");
        return outcome;
    }

    noteDirectMutation();

    outcome.accepted = true;
    outcome.annotation = authoring.annotation;
    outcome.page = authoring.page;
    outcome.revision = currentRevision();

    QJsonObject summary;
    summary.insert(QStringLiteral("edit"), QStringLiteral("annotation"));
    summary.insert(QStringLiteral("subtype"),
                   QLatin1String(pdf::getAnnotationAuthoringSubtypeName(request.annotation.subtype)));
    summary.insert(QStringLiteral("page"), int(request.annotation.pageIndex));
    summary.insert(QStringLiteral("annotation_object"), int(authoring.annotation.objectNumber));

    if (m_source.path.isEmpty())
    {
        // No document path means no sidecar to attach; the edit is still a real
        // revision, so the gap is reported rather than fabricated.
        outcome.historyError = QStringLiteral("direct-edit/history-sidecar-unavailable");
        return outcome;
    }

    quint64 sequence = 0;
    const pdf::PDFOperationResult recorded = pdf::appendAuthoringEditEvent(
        m_source.path, inputBytes, outputBytes, operationIdForSubtype(request.annotation.subtype),
        request.operatorIdentity, summary, &sequence);
    if (recorded)
    {
        outcome.historyAppended = true;
        outcome.historySequence = sequence;
    }
    else
    {
        // The store's message can name the sidecar directory, and a command
        // result is policy-safe (QUICK_SHELL_THREAT_MODEL.md): only the typed
        // code crosses this boundary.
        outcome.historyError = QStringLiteral("direct-edit/history-unrecorded");
    }
    return outcome;
}

void DocumentFacade::noteDirectMutation()
{
    // Same lifecycle bookkeeping as markModified(), minus the context revision
    // advance: setDocument() already produced the new revision. A queued save
    // was fenced against the revision this edit superseded, so it is superseded
    // rather than left to be silently discarded.
    if (m_outputState == DocumentOutputState::Pending)
    {
        supersedePending(CommandTerminalState::Cancelled, QStringLiteral("document/superseded"));
    }

    if (!m_publishedKey.isEmpty())
    {
        m_submitter->publishCurrentRevision(m_publishedKey, m_revisionSource.currentRevision());
    }

    setOutputState(DocumentOutputState::None);
    setFacets(m_facets | DocumentFacet::Dirty);
    updateAvailability();
}

}   // namespace pdfinteraction
