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

#ifndef DIRECTEDITREQUEST_H
#define DIRECTEDITREQUEST_H

#include "interactionglobal.h"

#include "pdfannotationauthoring.h"
#include "pdfdocumentcontext.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QVariantMap>

#include <optional>

namespace pdfinteraction
{

/// The typed authoring-edit families the direct-edit gateway accepts. One kind
/// per edit family; a value outside this vocabulary is a typed refusal, so a
/// handler cannot smuggle an unmodelled mutation through the gateway.
enum class DirectEditKind
{
    CreateAnnotation
};

const char* getDirectEditKindName(DirectEditKind kind);

/// Parses a direct-edit kind name. Returns false rather than guessing.
bool parseDirectEditKind(const QString& name, DirectEditKind* kind);

/// One authoring edit submitted to `DocumentFacade::applyDirectEdit` — the
/// mutation-gateway shape ADR-014 requires: the edit kind, its typed payload,
/// and the `PDFRevisionIdentity` the edit was begun against.
///
/// `revision` is the fence, not a hint. A request whose revision no longer
/// matches the context's current revision is refused with nothing written.
struct DirectEditRequest
{
    DirectEditKind kind = DirectEditKind::CreateAnnotation;
    pdf::AnnotationAuthoringSpec annotation;

    /// The revision the edit was begun against (complete PDFRevisionIdentity).
    pdf::PDFRevisionIdentity revision;

    /// The acting identity recorded on the appended operation-history event.
    /// Required; an empty identity is refused rather than attributed to nobody.
    QString operatorIdentity;
};

/// Result of one gateway submission. `accepted` is true exactly when the edit
/// was applied to the document as a new revision; a refusal leaves the document
/// untouched and carries a typed reason in `typedError`.
///
/// `historyAppended` reports the #124 provenance write separately: the document
/// mutation is authoritative and is never rolled back when the sidecar append
/// fails, so the failure is reported in `historyError` and surfaced by the
/// command result instead of being hidden.
struct DirectEditOutcome
{
    bool accepted = false;
    QString typedError;

    /// The revision the outcome describes: the new revision when accepted,
    /// the still-current revision when refused.
    pdf::PDFRevisionIdentity revision;

    pdf::PDFObjectReference annotation;
    pdf::PDFObjectReference page;

    bool historyAppended = false;
    quint64 historySequence = 0;
    QString historyError;
};

/// The annotation-authoring command ids of docs/loop-shell-actions.json, each
/// with the subtype its handler creates. One table, so the registered handlers,
/// the coverage check, and a test cannot disagree about what an id means.
const QList<QPair<QString, pdf::AnnotationAuthoringSubtype>>& annotationAuthoringCommandIds();

/// Looks one command id up in the table above. Returns false for an id outside
/// the family rather than defaulting to a subtype.
bool annotationAuthoringSubtypeForCommand(const QString& commandId,
                                          pdf::AnnotationAuthoringSubtype* subtype);

/// Parses one command invocation's scalar parameters into a typed request for
/// the facade gateway. This is the boundary: an invocation without usable
/// geometry, an unparseable point list, or a malformed colour is refused here
/// with a typed reason, so no free-form value reaches the document.
///
/// The returned request carries no revision and no operator identity; the
/// caller supplies both (the handler takes the revision at invoke time, a
/// gesture flow the revision the gesture was begun against).
std::optional<DirectEditRequest> directEditRequestFromParameters(
    pdf::AnnotationAuthoringSubtype subtype, const QVariantMap& parameters, QString* typedError);

}   // namespace pdfinteraction

#endif   // DIRECTEDITREQUEST_H
