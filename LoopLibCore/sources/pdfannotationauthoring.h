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

#ifndef PDFANNOTATIONAUTHORING_H
#define PDFANNOTATIONAUTHORING_H

#include "pdfdocument.h"
#include "pdfannotation.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <vector>

namespace pdf
{

/// The closed set of annotation subtypes the Quick authoring commands create
/// (issue #234). Every value maps onto an existing pdf::AnnotationType through
/// PDFDocumentBuilder's createAnnotation* primitives; no parallel annotation
/// model is introduced. The vocabulary is closed on purpose: an id outside it
/// is refused at the boundary rather than coerced into a default.
enum class AnnotationAuthoringSubtype
{
    TextHighlight,
    TextUnderline,
    TextStrikeout,
    TextSquiggly,
    Rectangle,
    Ellipse,
    StraightLine,
    Polyline,
    Polygon,
    FreehandCurve,
    InlineText,
    StickyNoteComment,
    StickyNoteHelp,
    StickyNoteInsert,
    StickyNoteKey,
    StickyNoteNewParagraph,
    StickyNoteNote,
    StickyNoteParagraph
};

/// Stable name of a subtype, as used by summaries and tests. Never localized.
LOOPLIBCORESHARED_EXPORT const char* getAnnotationAuthoringSubtypeName(AnnotationAuthoringSubtype subtype);

/// Parses a subtype name. Returns false rather than guessing, so an unknown
/// name is a typed refusal and not a silently defaulted subtype.
LOOPLIBCORESHARED_EXPORT bool parseAnnotationAuthoringSubtype(const QString& name,
                                                              AnnotationAuthoringSubtype* subtype);

/// One typed authoring edit for a single annotation. Geometry is in page space,
/// the coordinate system PDFAnnotation::getRectangle() reads back in.
struct LOOPLIBCORESHARED_EXPORT AnnotationAuthoringSpec
{
    AnnotationAuthoringSubtype subtype = AnnotationAuthoringSubtype::TextHighlight;
    PDFInteger pageIndex = 0;

    /// Bounding area for markup, rectangle, ellipse, inline text and the sticky
    /// note icon. A sticky note uses this as its icon position and size.
    QRectF rectangle;

    /// Vertices for StraightLine (exactly two), Polyline/Polygon (at least
    /// three for a polygon, two for a polyline) and FreehandCurve (at least two).
    std::vector<QPointF> vertices;

    /// Stroke (and, for closed shapes, interior) colour. An invalid colour
    /// selects the subtype's documented default.
    QColor color;

    /// Note or inline text. Required for InlineText and the sticky notes.
    QString contents;
    QString title;
    QString subject;
    PDFReal borderWidth = 1.0;
};

/// Icon rectangle edge length a sticky note is created with. Fixed at the size
/// PDFTextAnnotation draws its icon at, so the rectangle the operator asked for
/// and the rectangle the annotation reads back with agree.
inline constexpr PDFReal StickyNoteIconSize = 32.0;

/// Outcome of one creation attempt. `created` is false exactly when
/// `typedError` names why nothing was written; there is no partial success.
struct LOOPLIBCORESHARED_EXPORT AnnotationAuthoringResult
{
    bool created = false;
    QString typedError;
    PDFObjectReference annotation;
    PDFObjectReference page;
};

/// Creates one annotation of the spec's subtype on a copy of `document`, using
/// the PDFDocumentBuilder machinery that also produces the annotation's normal
/// appearance stream. The input document is never touched: on success the
/// returned pointer owns a complete new revision, and on refusal nullptr is
/// returned with `result->typedError` set. Validation is fail-closed: an
/// unsupported subtype, an out-of-range page or degenerate geometry is refused
/// before anything is built.
///
/// `result` must not be null.
LOOPLIBCORESHARED_EXPORT PDFDocumentPointer createAuthoredAnnotation(const PDFDocument* document,
                                                                     const AnnotationAuthoringSpec& spec,
                                                                     AnnotationAuthoringResult* result);

}   // namespace pdf

#endif   // PDFANNOTATIONAUTHORING_H
