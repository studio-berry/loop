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

#include "pdfannotationauthoring.h"

#include "pdfdocumentbuilder.h"

#include <cmath>

namespace pdf
{

namespace
{

/// A rectangle is usable only when it is valid and encloses a positive area.
/// An inverted drag is the caller's to normalize: silently repairing geometry
/// here would create an annotation the operator never drew.
bool isUsableRectangle(const QRectF& rectangle)
{
    return rectangle.isValid() && rectangle.width() > 0.0 && rectangle.height() > 0.0;
}

QColor defaultColorForSubtype(AnnotationAuthoringSubtype subtype)
{
    switch (subtype)
    {
        case AnnotationAuthoringSubtype::TextHighlight:
            return QColor(Qt::yellow);

        default:
            return QColor(Qt::red);
    }
}

}   // namespace

const char* getAnnotationAuthoringSubtypeName(AnnotationAuthoringSubtype subtype)
{
    switch (subtype)
    {
        case AnnotationAuthoringSubtype::TextHighlight:
            return "text-highlight";
        case AnnotationAuthoringSubtype::TextUnderline:
            return "text-underline";
        case AnnotationAuthoringSubtype::TextStrikeout:
            return "text-strikeout";
        case AnnotationAuthoringSubtype::TextSquiggly:
            return "text-squiggly";
        case AnnotationAuthoringSubtype::Rectangle:
            return "rectangle";
        case AnnotationAuthoringSubtype::Ellipse:
            return "ellipse";
        case AnnotationAuthoringSubtype::StraightLine:
            return "straight-line";
        case AnnotationAuthoringSubtype::Polyline:
            return "polyline";
        case AnnotationAuthoringSubtype::Polygon:
            return "polygon";
        case AnnotationAuthoringSubtype::FreehandCurve:
            return "freehand-curve";
        case AnnotationAuthoringSubtype::InlineText:
            return "inline-text";
        case AnnotationAuthoringSubtype::StickyNoteComment:
            return "sticky-note-comment";
        case AnnotationAuthoringSubtype::StickyNoteHelp:
            return "sticky-note-help";
        case AnnotationAuthoringSubtype::StickyNoteInsert:
            return "sticky-note-insert";
        case AnnotationAuthoringSubtype::StickyNoteKey:
            return "sticky-note-key";
        case AnnotationAuthoringSubtype::StickyNoteNewParagraph:
            return "sticky-note-new-paragraph";
        case AnnotationAuthoringSubtype::StickyNoteNote:
            return "sticky-note-note";
        case AnnotationAuthoringSubtype::StickyNoteParagraph:
            return "sticky-note-paragraph";
    }

    return "invalid";
}

bool parseAnnotationAuthoringSubtype(const QString& name, AnnotationAuthoringSubtype* subtype)
{
    for (const AnnotationAuthoringSubtype candidate :
         { AnnotationAuthoringSubtype::TextHighlight, AnnotationAuthoringSubtype::TextUnderline,
           AnnotationAuthoringSubtype::TextStrikeout, AnnotationAuthoringSubtype::TextSquiggly,
           AnnotationAuthoringSubtype::Rectangle, AnnotationAuthoringSubtype::Ellipse,
           AnnotationAuthoringSubtype::StraightLine, AnnotationAuthoringSubtype::Polyline,
           AnnotationAuthoringSubtype::Polygon, AnnotationAuthoringSubtype::FreehandCurve,
           AnnotationAuthoringSubtype::InlineText, AnnotationAuthoringSubtype::StickyNoteComment,
           AnnotationAuthoringSubtype::StickyNoteHelp, AnnotationAuthoringSubtype::StickyNoteInsert,
           AnnotationAuthoringSubtype::StickyNoteKey, AnnotationAuthoringSubtype::StickyNoteNewParagraph,
           AnnotationAuthoringSubtype::StickyNoteNote, AnnotationAuthoringSubtype::StickyNoteParagraph })
    {
        if (name == QLatin1String(getAnnotationAuthoringSubtypeName(candidate)))
        {
            if (subtype)
            {
                *subtype = candidate;
            }
            return true;
        }
    }

    return false;
}

namespace
{

/// Sticky-note subtypes share one builder primitive; the icon name is the only
/// difference, so the mapping lives here rather than at each call site.
bool isStickyNoteSubtype(AnnotationAuthoringSubtype subtype)
{
    switch (subtype)
    {
        case AnnotationAuthoringSubtype::StickyNoteComment:
        case AnnotationAuthoringSubtype::StickyNoteHelp:
        case AnnotationAuthoringSubtype::StickyNoteInsert:
        case AnnotationAuthoringSubtype::StickyNoteKey:
        case AnnotationAuthoringSubtype::StickyNoteNewParagraph:
        case AnnotationAuthoringSubtype::StickyNoteNote:
        case AnnotationAuthoringSubtype::StickyNoteParagraph:
            return true;

        default:
            return false;
    }
}

TextAnnotationIcon stickyNoteIcon(AnnotationAuthoringSubtype subtype)
{
    switch (subtype)
    {
        case AnnotationAuthoringSubtype::StickyNoteComment:
            return TextAnnotationIcon::Comment;
        case AnnotationAuthoringSubtype::StickyNoteHelp:
            return TextAnnotationIcon::Help;
        case AnnotationAuthoringSubtype::StickyNoteInsert:
            return TextAnnotationIcon::Insert;
        case AnnotationAuthoringSubtype::StickyNoteKey:
            return TextAnnotationIcon::Key;
        case AnnotationAuthoringSubtype::StickyNoteNewParagraph:
            return TextAnnotationIcon::NewParagraph;
        case AnnotationAuthoringSubtype::StickyNoteNote:
            return TextAnnotationIcon::Note;
        case AnnotationAuthoringSubtype::StickyNoteParagraph:
            return TextAnnotationIcon::Paragraph;

        default:
            return TextAnnotationIcon::Note;
    }
}

}   // namespace

PDFDocumentPointer createAuthoredAnnotation(const PDFDocument* document,
                                            const AnnotationAuthoringSpec& spec,
                                            AnnotationAuthoringResult* result)
{
    Q_ASSERT(result);
    *result = {};

    if (!document)
    {
        result->typedError = QStringLiteral("authoring/document-required");
        return nullptr;
    }

    const auto verticesAreDistinct = [&spec](qsizetype minimumCount)
    {
        if (qsizetype(spec.vertices.size()) < minimumCount)
        {
            return false;
        }
        for (const QPointF& point : spec.vertices)
        {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
            {
                return false;
            }
        }
        return true;
    };

    // Geometry is validated per subtype before the document is copied: a
    // refusal must cost nothing and must never leave a half-written annotation.
    switch (spec.subtype)
    {
        case AnnotationAuthoringSubtype::TextHighlight:
        case AnnotationAuthoringSubtype::TextUnderline:
        case AnnotationAuthoringSubtype::TextStrikeout:
        case AnnotationAuthoringSubtype::TextSquiggly:
        case AnnotationAuthoringSubtype::Rectangle:
        case AnnotationAuthoringSubtype::Ellipse:
            if (!isUsableRectangle(spec.rectangle))
            {
                result->typedError = QStringLiteral("authoring/invalid-geometry");
                return nullptr;
            }
            break;

        case AnnotationAuthoringSubtype::StraightLine:
            if (spec.vertices.size() != 2 || !verticesAreDistinct(2) ||
                spec.vertices[0] == spec.vertices[1])
            {
                result->typedError = QStringLiteral("authoring/invalid-geometry");
                return nullptr;
            }
            break;

        case AnnotationAuthoringSubtype::Polyline:
        case AnnotationAuthoringSubtype::FreehandCurve:
            if (!verticesAreDistinct(2))
            {
                result->typedError = QStringLiteral("authoring/invalid-geometry");
                return nullptr;
            }
            break;

        case AnnotationAuthoringSubtype::Polygon:
            if (!verticesAreDistinct(3))
            {
                result->typedError = QStringLiteral("authoring/invalid-geometry");
                return nullptr;
            }
            break;

        case AnnotationAuthoringSubtype::InlineText:
            if (!isUsableRectangle(spec.rectangle))
            {
                result->typedError = QStringLiteral("authoring/invalid-geometry");
                return nullptr;
            }
            if (spec.contents.trimmed().isEmpty())
            {
                result->typedError = QStringLiteral("authoring/missing-contents");
                return nullptr;
            }
            break;

        case AnnotationAuthoringSubtype::StickyNoteComment:
        case AnnotationAuthoringSubtype::StickyNoteHelp:
        case AnnotationAuthoringSubtype::StickyNoteInsert:
        case AnnotationAuthoringSubtype::StickyNoteKey:
        case AnnotationAuthoringSubtype::StickyNoteNewParagraph:
        case AnnotationAuthoringSubtype::StickyNoteNote:
        case AnnotationAuthoringSubtype::StickyNoteParagraph:
            if (!isUsableRectangle(spec.rectangle))
            {
                result->typedError = QStringLiteral("authoring/invalid-geometry");
                return nullptr;
            }
            if (spec.contents.trimmed().isEmpty())
            {
                result->typedError = QStringLiteral("authoring/missing-contents");
                return nullptr;
            }
            break;
    }

    // Page references come from the catalog's walked page tree rather than the
    // builder's direct Kids array, so a nested page tree addresses pages by the
    // same index the rest of Core uses.
    const PDFCatalog* catalog = document->getCatalog();
    if (!catalog || spec.pageIndex < 0 || std::size_t(spec.pageIndex) >= catalog->getPageCount())
    {
        result->typedError = QStringLiteral("authoring/invalid-page");
        return nullptr;
    }
    const PDFPage* page = catalog->getPage(std::size_t(spec.pageIndex));
    if (!page)
    {
        result->typedError = QStringLiteral("authoring/invalid-page");
        return nullptr;
    }

    PDFDocumentBuilder builder(document);
    const PDFObjectReference pageReference = page->getPageReference();
    const QColor color = spec.color.isValid() ? spec.color : defaultColorForSubtype(spec.subtype);
    const QString title = spec.title;
    const QString subject = spec.subject;
    const QString contents = spec.contents;
    const PDFReal borderWidth = spec.borderWidth > 0.0 ? spec.borderWidth : 1.0;

    PDFObjectReference annotation;
    switch (spec.subtype)
    {
        case AnnotationAuthoringSubtype::TextHighlight:
            annotation = builder.createAnnotationHighlight(pageReference, spec.rectangle, color, title, subject, contents);
            break;
        case AnnotationAuthoringSubtype::TextUnderline:
            annotation = builder.createAnnotationUnderline(pageReference, spec.rectangle, color, title, subject, contents);
            break;
        case AnnotationAuthoringSubtype::TextStrikeout:
            annotation = builder.createAnnotationStrikeout(pageReference, spec.rectangle, color, title, subject, contents);
            break;
        case AnnotationAuthoringSubtype::TextSquiggly:
            annotation = builder.createAnnotationSquiggly(pageReference, spec.rectangle, color, title, subject, contents);
            break;
        case AnnotationAuthoringSubtype::Rectangle:
            annotation = builder.createAnnotationSquare(pageReference, spec.rectangle, borderWidth, QColor(), color, title, subject, contents);
            break;
        case AnnotationAuthoringSubtype::Ellipse:
            annotation = builder.createAnnotationCircle(pageReference, spec.rectangle, borderWidth, QColor(), color, title, subject, contents);
            break;
        case AnnotationAuthoringSubtype::StraightLine:
        {
            const QRectF bounding = QRectF(spec.vertices[0], spec.vertices[1]).normalized();
            annotation = builder.createAnnotationLine(pageReference, bounding, spec.vertices[0], spec.vertices[1],
                                                      borderWidth, QColor(), color, title, subject, contents,
                                                      AnnotationLineEnding::None, AnnotationLineEnding::None);
            break;
        }
        case AnnotationAuthoringSubtype::Polyline:
        {
            QPolygonF polyline;
            for (const QPointF& point : spec.vertices)
            {
                polyline.append(point);
            }
            annotation = builder.createAnnotationPolyline(pageReference, polyline, borderWidth, QColor(), color,
                                                          title, subject, contents,
                                                          AnnotationLineEnding::None, AnnotationLineEnding::None);
            break;
        }
        case AnnotationAuthoringSubtype::Polygon:
        {
            QPolygonF polygon;
            for (const QPointF& point : spec.vertices)
            {
                polygon.append(point);
            }
            annotation = builder.createAnnotationPolygon(pageReference, polygon, borderWidth, QColor(), color,
                                                         title, subject, contents);
            break;
        }
        case AnnotationAuthoringSubtype::FreehandCurve:
        {
            QPolygonF ink;
            for (const QPointF& point : spec.vertices)
            {
                ink.append(point);
            }
            annotation = builder.createAnnotationInk(pageReference, ink, borderWidth, color, title, subject, contents);
            break;
        }
        case AnnotationAuthoringSubtype::InlineText:
            annotation = builder.createAnnotationFreeText(pageReference, spec.rectangle, title, subject, contents,
                                                          Qt::AlignLeft | Qt::AlignTop);
            break;

        case AnnotationAuthoringSubtype::StickyNoteComment:
        case AnnotationAuthoringSubtype::StickyNoteHelp:
        case AnnotationAuthoringSubtype::StickyNoteInsert:
        case AnnotationAuthoringSubtype::StickyNoteKey:
        case AnnotationAuthoringSubtype::StickyNoteNewParagraph:
        case AnnotationAuthoringSubtype::StickyNoteNote:
        case AnnotationAuthoringSubtype::StickyNoteParagraph:
            Q_ASSERT(isStickyNoteSubtype(spec.subtype));
            annotation = builder.createAnnotationText(pageReference, spec.rectangle, stickyNoteIcon(spec.subtype),
                                                      title, subject, contents, false);
            break;
    }

    if (!annotation.isValid())
    {
        result->typedError = QStringLiteral("authoring/creation-failed");
        return nullptr;
    }

    result->created = true;
    result->annotation = annotation;
    result->page = pageReference;
    return PDFDocumentPointer(new PDFDocument(builder.build()));
}

}   // namespace pdf
