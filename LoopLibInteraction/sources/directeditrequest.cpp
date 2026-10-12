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

#include "directeditrequest.h"

#include <QColor>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace pdfinteraction
{

namespace
{

using pdf::AnnotationAuthoringSubtype;

bool isRectangleSubtype(AnnotationAuthoringSubtype subtype)
{
    switch (subtype)
    {
        case AnnotationAuthoringSubtype::TextHighlight:
        case AnnotationAuthoringSubtype::TextUnderline:
        case AnnotationAuthoringSubtype::TextStrikeout:
        case AnnotationAuthoringSubtype::TextSquiggly:
        case AnnotationAuthoringSubtype::Rectangle:
        case AnnotationAuthoringSubtype::Ellipse:
        case AnnotationAuthoringSubtype::InlineText:
            return true;

        default:
            return false;
    }
}

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

bool readNumber(const QVariantMap& parameters, const QString& name, double* value)
{
    const QVariant parameter = parameters.value(name);
    if (!parameter.isValid())
    {
        return false;
    }

    bool ok = false;
    const double number = parameter.toDouble(&ok);
    if (!ok || !std::isfinite(number))
    {
        return false;
    }

    *value = number;
    return true;
}

/// Parses the `points` scalar: whitespace-separated numbers in x,y pairs, at
/// least `minimumPoints` pairs. A malformed list is refused, never truncated.
bool readPoints(const QString& value, qsizetype minimumPoints, std::vector<QPointF>* points)
{
    static const QRegularExpression separator(QStringLiteral("\\s+"));

    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
    {
        return false;
    }

    const QStringList tokens = trimmed.split(separator, Qt::SkipEmptyParts);
    if (tokens.size() % 2 != 0)
    {
        return false;
    }

    std::vector<QPointF> parsed;
    parsed.reserve(std::size_t(tokens.size() / 2));
    for (qsizetype index = 0; index < tokens.size(); index += 2)
    {
        bool xOk = false;
        bool yOk = false;
        const double x = tokens[index].toDouble(&xOk);
        const double y = tokens[index + 1].toDouble(&yOk);
        if (!xOk || !yOk || !std::isfinite(x) || !std::isfinite(y))
        {
            return false;
        }
        parsed.emplace_back(x, y);
    }

    if (qsizetype(parsed.size()) < minimumPoints)
    {
        return false;
    }

    *points = std::move(parsed);
    return true;
}

bool readOptionalColor(const QVariantMap& parameters, QColor* color)
{
    const QVariant parameter = parameters.value(QStringLiteral("color"));
    if (!parameter.isValid())
    {
        return true;
    }

    const QColor parsed(parameter.toString());
    if (!parsed.isValid())
    {
        return false;
    }

    *color = parsed;
    return true;
}

}   // namespace

const QList<QPair<QString, pdf::AnnotationAuthoringSubtype>>& annotationAuthoringCommandIds()
{
    static const QList<QPair<QString, pdf::AnnotationAuthoringSubtype>> ids = {
        { QStringLiteral("actionCreateTextHighlight"), pdf::AnnotationAuthoringSubtype::TextHighlight },
        { QStringLiteral("actionCreateTextUnderline"), pdf::AnnotationAuthoringSubtype::TextUnderline },
        { QStringLiteral("actionCreateTextStrikeout"), pdf::AnnotationAuthoringSubtype::TextStrikeout },
        { QStringLiteral("actionCreateTextSquiggly"), pdf::AnnotationAuthoringSubtype::TextSquiggly },
        { QStringLiteral("actionCreateRectangle"), pdf::AnnotationAuthoringSubtype::Rectangle },
        { QStringLiteral("actionCreateEllipse"), pdf::AnnotationAuthoringSubtype::Ellipse },
        { QStringLiteral("actionCreateStraightLine"), pdf::AnnotationAuthoringSubtype::StraightLine },
        { QStringLiteral("actionCreatePolyline"), pdf::AnnotationAuthoringSubtype::Polyline },
        { QStringLiteral("actionCreatePolygon"), pdf::AnnotationAuthoringSubtype::Polygon },
        { QStringLiteral("actionCreateFreehandCurve"), pdf::AnnotationAuthoringSubtype::FreehandCurve },
        { QStringLiteral("actionInlineText"), pdf::AnnotationAuthoringSubtype::InlineText },
        { QStringLiteral("actionStickyNoteComment"), pdf::AnnotationAuthoringSubtype::StickyNoteComment },
        { QStringLiteral("actionStickyNoteHelp"), pdf::AnnotationAuthoringSubtype::StickyNoteHelp },
        { QStringLiteral("actionStickyNoteInsert"), pdf::AnnotationAuthoringSubtype::StickyNoteInsert },
        { QStringLiteral("actionStickyNoteKey"), pdf::AnnotationAuthoringSubtype::StickyNoteKey },
        { QStringLiteral("actionStickyNoteNewParagraph"), pdf::AnnotationAuthoringSubtype::StickyNoteNewParagraph },
        { QStringLiteral("actionStickyNoteNote"), pdf::AnnotationAuthoringSubtype::StickyNoteNote },
        { QStringLiteral("actionStickyNoteParagraph"), pdf::AnnotationAuthoringSubtype::StickyNoteParagraph },
    };
    return ids;
}

bool annotationAuthoringSubtypeForCommand(const QString& commandId,
                                          pdf::AnnotationAuthoringSubtype* subtype)
{
    for (const auto& entry : annotationAuthoringCommandIds())
    {
        if (entry.first == commandId)
        {
            if (subtype)
            {
                *subtype = entry.second;
            }
            return true;
        }
    }

    return false;
}

std::optional<DirectEditRequest> directEditRequestFromParameters(
    pdf::AnnotationAuthoringSubtype subtype, const QVariantMap& parameters, QString* typedError)
{
    if (typedError)
    {
        typedError->clear();
    }

    DirectEditRequest request;
    request.kind = DirectEditKind::CreateAnnotation;
    request.annotation.subtype = subtype;

    if (!parameters.contains(QStringLiteral("page")))
    {
        if (typedError)
        {
            *typedError = QStringLiteral("annotation/missing-page");
        }
        return std::nullopt;
    }
    bool pageOk = false;
    const int pageIndex = parameters.value(QStringLiteral("page")).toInt(&pageOk);
    if (!pageOk || pageIndex < 0)
    {
        if (typedError)
        {
            *typedError = QStringLiteral("annotation/invalid-page");
        }
        return std::nullopt;
    }
    request.annotation.pageIndex = pageIndex;

    if (isRectangleSubtype(subtype))
    {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        if (!readNumber(parameters, QStringLiteral("x"), &x) ||
            !readNumber(parameters, QStringLiteral("y"), &y) ||
            !readNumber(parameters, QStringLiteral("width"), &width) ||
            !readNumber(parameters, QStringLiteral("height"), &height))
        {
            if (typedError)
            {
                *typedError = QStringLiteral("annotation/missing-geometry");
            }
            return std::nullopt;
        }
        if (width <= 0.0 || height <= 0.0)
        {
            if (typedError)
            {
                *typedError = QStringLiteral("annotation/invalid-geometry");
            }
            return std::nullopt;
        }
        request.annotation.rectangle = QRectF(x, y, width, height);
    }
    else if (subtype == pdf::AnnotationAuthoringSubtype::StraightLine)
    {
        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
        if (!readNumber(parameters, QStringLiteral("x1"), &x1) ||
            !readNumber(parameters, QStringLiteral("y1"), &y1) ||
            !readNumber(parameters, QStringLiteral("x2"), &x2) ||
            !readNumber(parameters, QStringLiteral("y2"), &y2))
        {
            if (typedError)
            {
                *typedError = QStringLiteral("annotation/missing-geometry");
            }
            return std::nullopt;
        }
        if (x1 == x2 && y1 == y2)
        {
            if (typedError)
            {
                *typedError = QStringLiteral("annotation/invalid-geometry");
            }
            return std::nullopt;
        }
        request.annotation.vertices = { QPointF(x1, y1), QPointF(x2, y2) };
    }
    else if (subtype == pdf::AnnotationAuthoringSubtype::Polyline ||
             subtype == pdf::AnnotationAuthoringSubtype::Polygon ||
             subtype == pdf::AnnotationAuthoringSubtype::FreehandCurve)
    {
        const qsizetype minimum = subtype == pdf::AnnotationAuthoringSubtype::Polygon ? 3 : 2;
        if (!parameters.contains(QStringLiteral("points")) ||
            !readPoints(parameters.value(QStringLiteral("points")).toString(), minimum,
                        &request.annotation.vertices))
        {
            if (typedError)
            {
                *typedError = QStringLiteral("annotation/invalid-points");
            }
            return std::nullopt;
        }
    }
    else if (isStickyNoteSubtype(subtype))
    {
        double x = 0.0;
        double y = 0.0;
        if (!readNumber(parameters, QStringLiteral("x"), &x) ||
            !readNumber(parameters, QStringLiteral("y"), &y))
        {
            if (typedError)
            {
                *typedError = QStringLiteral("annotation/missing-geometry");
            }
            return std::nullopt;
        }
        request.annotation.rectangle = QRectF(x, y, pdf::StickyNoteIconSize, pdf::StickyNoteIconSize);
    }

    if (subtype == pdf::AnnotationAuthoringSubtype::InlineText || isStickyNoteSubtype(subtype))
    {
        const QString text = parameters.value(QStringLiteral("text")).toString();
        if (text.trimmed().isEmpty())
        {
            if (typedError)
            {
                *typedError = QStringLiteral("annotation/missing-text");
            }
            return std::nullopt;
        }
        request.annotation.contents = text;
    }

    if (!readOptionalColor(parameters, &request.annotation.color))
    {
        if (typedError)
        {
            *typedError = QStringLiteral("annotation/invalid-color");
        }
        return std::nullopt;
    }

    return request;
}

}   // namespace pdfinteraction
