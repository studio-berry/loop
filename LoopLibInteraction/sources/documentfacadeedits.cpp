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
#include "pdfauthoringedit.h"
#include "pdfauthoringhistory.h"
#include "pdfdirectedit.h"
#include "pdfdocumentwriter.h"
#include "pdfresourcebudget.h"
#include "pdfsecurityhandler.h"

#include <QBuffer>
#include <QJsonObject>
#include <QVariant>

namespace pdfinteraction
{

namespace
{

constexpr const char* FieldParameter = "field";
constexpr const char* ValueParameter = "value";
constexpr const char* CheckedParameter = "checked";
constexpr const char* NameParameter = "name";
constexpr const char* DescriptionParameter = "description";
constexpr const char* TitleParameter = "title";
constexpr const char* AuthorParameter = "author";
constexpr const char* SubjectParameter = "subject";
constexpr const char* KeywordsParameter = "keywords";
constexpr const char* CreatorParameter = "creator";

/// Reads one declared string parameter. An absent key is an absent value; a key
/// carrying another type is malformed, never coerced.
std::optional<QString> scalarString(const QVariantMap& scalars, const char* name, bool* malformed)
{
    const auto it = scalars.constFind(QLatin1String(name));
    if (it == scalars.constEnd())
    {
        return std::nullopt;
    }
    if (it->typeId() != QMetaType::QString)
    {
        *malformed = true;
        return std::nullopt;
    }
    return it->toString();
}

std::optional<bool> scalarBool(const QVariantMap& scalars, const char* name, bool* malformed)
{
    const auto it = scalars.constFind(QLatin1String(name));
    if (it == scalars.constEnd())
    {
        return std::nullopt;
    }
    if (it->typeId() != QMetaType::Bool)
    {
        *malformed = true;
        return std::nullopt;
    }
    return it->toBool();
}

bool requiredString(const QVariantMap& scalars, const char* name, QString* value, bool* malformed)
{
    const std::optional<QString> read = scalarString(scalars, name, malformed);
    if (!read || read->isEmpty())
    {
        *malformed = true;
        return false;
    }
    *value = *read;
    return true;
}

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

        case DirectEditKind::EditOutline:
            return "edit-outline";

        case DirectEditKind::SetFormFieldValue:
            return "set-form-field-value";

        case DirectEditKind::AddAttachment:
            return "add-attachment";

        case DirectEditKind::RemoveAttachment:
            return "remove-attachment";

        case DirectEditKind::EditMetadata:
            return "edit-metadata";
    }

    return "invalid";
}

bool parseDirectEditKind(const QString& name, DirectEditKind* kind)
{
    static const QList<QPair<QString, DirectEditKind>> names = {
        { QStringLiteral("create-annotation"), DirectEditKind::CreateAnnotation },
        { QStringLiteral("edit-outline"), DirectEditKind::EditOutline },
        { QStringLiteral("set-form-field-value"), DirectEditKind::SetFormFieldValue },
        { QStringLiteral("add-attachment"), DirectEditKind::AddAttachment },
        { QStringLiteral("remove-attachment"), DirectEditKind::RemoveAttachment },
        { QStringLiteral("edit-metadata"), DirectEditKind::EditMetadata },
    };
    for (const auto& entry : names)
    {
        if (entry.first == name)
        {
            if (kind)
            {
                *kind = entry.second;
            }
            return true;
        }
    }

    return false;
}

DirectEditOutcome DocumentFacade::applyDirectEdit(const DirectEditRequest& request)
{
    switch (request.kind)
    {
        case DirectEditKind::CreateAnnotation:
            return applyAnnotationEdit(request);

        case DirectEditKind::EditOutline:
            return applyOutlineEdit(request);

        case DirectEditKind::SetFormFieldValue:
        case DirectEditKind::AddAttachment:
        case DirectEditKind::RemoveAttachment:
        case DirectEditKind::EditMetadata:
            return applyDocumentEdit(request);
    }

    // A kind outside the vocabulary cannot be dispatched; it is refused rather
    // than handed to a family that would guess at the payload.
    DirectEditOutcome outcome;
    outcome.typedError = QStringLiteral("direct-edit/unsupported-kind");
    return outcome;
}

DirectEditOutcome DocumentFacade::applyAnnotationEdit(const DirectEditRequest& request)
{
    DirectEditOutcome outcome;
    outcome.revision = currentRevision();

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

DirectEditOutcome DocumentFacade::applyOutlineEdit(const DirectEditRequest& request)
{
    DirectEditOutcome outcome;
    const auto refuse = [&outcome](const QString& typedError)
    {
        outcome.accepted = false;
        outcome.typedError = typedError;
        return outcome;
    };

    pdf::PDFDocumentContext* documentContext = context();
    if (m_state != DocumentState::Ready || !documentContext || !documentContext->getDocument())
    {
        return refuse(QStringLiteral("edit/document-not-ready"));
    }

    // The fence is checked before any mutation, so an edit begun against a
    // revision the operator is no longer looking at cannot land on this one.
    if (!m_revisionSource.isCurrent(request.revision))
    {
        return refuse(QStringLiteral("edit/stale-revision"));
    }

    if (!permissions().modify)
    {
        return refuse(QStringLiteral("edit/modify-permission-denied"));
    }

    pdf::PDFAuthoringEditResult result;
    try
    {
        result = pdf::PDFAuthoringEditWriter::apply(documentContext->getDocument(), request.authoring);
    }
    catch (const pdf::PDFResourceBudgetExceededException&)
    {
        return refuse(QStringLiteral("edit/resource-budget-exceeded"));
    }
    catch (const pdf::PDFException&)
    {
        return refuse(QStringLiteral("edit/refused-by-core"));
    }

    if (!result.applied())
    {
        return refuse(result.typedError.isEmpty() ? QStringLiteral("edit/refused") : result.typedError);
    }

    // The writer produced a new document; replacing it advances the document
    // revision and the cache generation, which is the revision this edit
    // produced. The old document stays owned by nothing once the context drops
    // it, exactly as a document replacement does.
    documentContext->setDocument(result.document, result.flags);
    noteAuthoringMutation();

    outcome.accepted = true;
    outcome.resolution = result.kind;
    outcome.affectedCount = result.affectedCount;
    outcome.revision = m_revisionSource.currentRevision();
    return outcome;
}

DirectEditOutcome DocumentFacade::applyDocumentEdit(const DirectEditRequest& request)
{
    DirectEditOutcome outcome;
    outcome.revision = currentRevision();

    pdf::PDFDocumentContext* documentContext = context();
    const pdf::PDFDocument* document = documentContext ? documentContext->getDocument() : nullptr;
    if (m_state != DocumentState::Ready || !document)
    {
        outcome.typedError = QStringLiteral("document/not-ready");
        return outcome;
    }

    // The fence first. An edit begun against a superseded revision is refused
    // before its payload is read and before any writer is reached.
    if (!request.revision.isValid() || request.revision != currentRevision())
    {
        outcome.typedError = QStringLiteral("document/stale-revision");
        return outcome;
    }

    // One mutation at a time: a lifecycle operation in flight is measured
    // against the revision this edit is about to supersede.
    if (m_pendingInvocation != InvalidCommandInvocation || m_outputState == DocumentOutputState::Pending)
    {
        outcome.typedError = QStringLiteral("document/busy");
        return outcome;
    }

    if (!directEditAllowed(request.kind))
    {
        outcome.typedError = QStringLiteral("document/modify-permission-denied");
        return outcome;
    }

    // Parse the scalar payload at this boundary, then hand it to the one Core
    // writer for the family. Nothing below here reads the raw map again.
    bool malformed = false;
    pdf::DirectEditOutcome coreOutcome;
    switch (request.kind)
    {
        case DirectEditKind::SetFormFieldValue:
        {
            pdf::FormFieldValueEdit edit;
            const bool hasField = requiredString(request.scalars, FieldParameter, &edit.field, &malformed);
            edit.text = scalarString(request.scalars, ValueParameter, &malformed);
            edit.checked = scalarBool(request.scalars, CheckedParameter, &malformed);
            if (!hasField || malformed || (!edit.text && !edit.checked))
            {
                outcome.typedError = QStringLiteral("direct-edit/invalid-request");
                return outcome;
            }
            coreOutcome = pdf::PDFFormValueWriter::setFieldValue(document, edit);
            break;
        }

        case DirectEditKind::AddAttachment:
        {
            pdf::AttachmentEdit edit;
            const bool hasName = requiredString(request.scalars, NameParameter, &edit.name, &malformed);
            edit.description = scalarString(request.scalars, DescriptionParameter, &malformed).value_or(QString());
            edit.contents = request.contents;
            if (!hasName || malformed || edit.contents.isEmpty())
            {
                outcome.typedError = QStringLiteral("direct-edit/invalid-request");
                return outcome;
            }
            coreOutcome = pdf::PDFAttachmentWriter::add(document, edit);
            break;
        }

        case DirectEditKind::RemoveAttachment:
        {
            QString name;
            const bool hasName = requiredString(request.scalars, NameParameter, &name, &malformed);
            if (!hasName || malformed)
            {
                outcome.typedError = QStringLiteral("direct-edit/invalid-request");
                return outcome;
            }
            coreOutcome = pdf::PDFAttachmentWriter::remove(document, name);
            break;
        }

        case DirectEditKind::EditMetadata:
        {
            pdf::DocumentMetadataEdit edit;
            edit.title = scalarString(request.scalars, TitleParameter, &malformed);
            edit.author = scalarString(request.scalars, AuthorParameter, &malformed);
            edit.subject = scalarString(request.scalars, SubjectParameter, &malformed);
            edit.keywords = scalarString(request.scalars, KeywordsParameter, &malformed);
            edit.creator = scalarString(request.scalars, CreatorParameter, &malformed);
            if (malformed || (!edit.title && !edit.author && !edit.subject && !edit.keywords && !edit.creator))
            {
                outcome.typedError = QStringLiteral("direct-edit/invalid-request");
                return outcome;
            }
            coreOutcome = pdf::PDFMetadataWriter::update(document, edit);
            break;
        }

        case DirectEditKind::CreateAnnotation:
        case DirectEditKind::EditOutline:
            // The dispatch routes these to their own helpers; a direct call is
            // refused rather than handed to a document writer that would not
            // understand the payload.
            outcome.typedError = QStringLiteral("direct-edit/unsupported-kind");
            return outcome;
    }

    if (!coreOutcome.ok())
    {
        outcome.typedError = QString::fromLatin1(pdf::getDirectEditRefusalCode(coreOutcome.refusal));
        return outcome;
    }

    // Accept the edit: the new document replaces the current one, which is what
    // advances the revision exactly once. markModified() is deliberately not
    // used here -- it would advance the revision a second time for one edit.
    if (m_outputState == DocumentOutputState::Pending)
    {
        supersedePending(CommandTerminalState::Cancelled, QStringLiteral("document/superseded"));
    }
    documentContext->setDocument(coreOutcome.document);
    if (!m_publishedKey.isEmpty())
    {
        m_submitter->publishCurrentRevision(m_publishedKey, m_revisionSource.currentRevision());
    }
    setOutputState(DocumentOutputState::None);
    setFacets(m_facets | DocumentFacet::Dirty);
    updateAvailability();

    outcome.accepted = true;
    outcome.revision = m_revisionSource.currentRevision();
    return outcome;
}

bool DocumentFacade::directEditAllowed(DirectEditKind kind) const
{
    const auto* documentContext = context();
    const auto* document = m_state == DocumentState::Ready && documentContext ? documentContext->getDocument() : nullptr;
    const auto* security = document ? document->getStorage().getSecurityHandler() : nullptr;
    if (!security)
    {
        return false;
    }

    using Permission = pdf::PDFSecurityHandler::Permission;
    switch (kind)
    {
        case DirectEditKind::CreateAnnotation:
            return security->isAllowed(Permission::ModifyInteractiveItems);

        case DirectEditKind::EditOutline:
            return security->isAllowed(Permission::Modify);

        case DirectEditKind::SetFormFieldValue:
            return security->isAllowed(Permission::ModifyFormFields);

        case DirectEditKind::AddAttachment:
        case DirectEditKind::RemoveAttachment:
        case DirectEditKind::EditMetadata:
            return security->isAllowed(Permission::Modify);
    }

    return false;
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
