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

#ifndef TEXTSELECTIONSOURCE_H
#define TEXTSELECTIONSOURCE_H

#include "interactionglobal.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace pdfinteraction
{

/// What one selection request produced. `available == false` means the page has
/// no text layer to select from; it is a reported state, never an empty string
/// presented as success.
struct TextSelectionResult
{
    bool available = false;
    QString unavailableReason;
    QString text;

    /// Page-space rectangles of the selected characters, one per line span.
    QList<QRectF> geometry;

    /// Reading-order line range of the result, or -1 when it does not address
    /// lines.
    int firstLine = -1;
    int lastLine = -1;
};

/// Text extraction for the interaction layer (issue #232).
///
/// The seam exists because a text selection needs the document's text layer and
/// the interaction layer holds no document. An implementation answers in page
/// space and in Core's own reading order; the interaction layer re-implements
/// no extraction and invents no order of its own.
class ITextSelectionSource
{
public:
    virtual ~ITextSelectionSource() = default;

    /// Whether the page has a text layer at all.
    virtual bool hasTextLayer(int pageIndex) const = 0;

    /// Text between two page-space points, in reading order.
    virtual TextSelectionResult selectRange(int pageIndex, QPointF startPagePoint, QPointF endPagePoint) = 0;

    /// The page's whole text layer, in reading order.
    virtual TextSelectionResult selectPage(int pageIndex) = 0;

    /// The one text block containing a page-space point -- a table or a
    /// paragraph to the text layer.
    virtual TextSelectionResult selectBlockAt(int pageIndex, QPointF pagePoint) = 0;

    /// Lines the page's text layer has in reading order, or -1 when the page
    /// has no text layer.
    virtual int lineCount(int pageIndex) const = 0;

    /// One line by reading-order ordinal.
    virtual TextSelectionResult selectLine(int pageIndex, int lineIndex) = 0;
};

}   // namespace pdfinteraction

#endif   // TEXTSELECTIONSOURCE_H
