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

#ifndef PDFDOCUMENTTEXTSELECTIONSOURCE_H
#define PDFDOCUMENTTEXTSELECTIONSOURCE_H

#include "interactionglobal.h"
#include "textselectionsource.h"

#include "pdfdocumentcontext.h"
#include "pdftextlayout.h"

#include <QHash>
#include <QPointer>

namespace pdfinteraction
{

/// Answers text selection questions from the document the context holds.
///
/// Extraction is Core's: the same PDFTextLayoutGenerator and text-flow
/// machinery pdf::searchDocumentText uses, so a copied selection is the text
/// layer the document search already agreed on, including the docstrum reading
/// order. Layouts are cached per page and dropped when the context's revision
/// moves, because a layout describes one revision and nothing else.
///
/// The context is observed through a QPointer, so a destroyed context degrades
/// to "no document" rather than a dangling read -- the same rule
/// PDFDocumentContextSource and PDFDocumentPageGeometrySource follow.
class PDFDocumentTextSelectionSource final : public ITextSelectionSource
{
    Q_DECLARE_TR_FUNCTIONS(pdfinteraction::PDFDocumentTextSelectionSource)

public:
    explicit PDFDocumentTextSelectionSource(pdf::PDFDocumentContext* context);

    bool hasTextLayer(int pageIndex) const override;
    TextSelectionResult selectRange(int pageIndex, QPointF startPagePoint, QPointF endPagePoint) override;
    TextSelectionResult selectPage(int pageIndex) override;
    TextSelectionResult selectBlockAt(int pageIndex, QPointF pagePoint) override;
    int lineCount(int pageIndex) const override;
    TextSelectionResult selectLine(int pageIndex, int lineIndex) override;

private:
    /// The page's layout, built on first use and cached for the current
    /// revision. Returns nullptr with `unavailableReason` set when the page has
    /// no text layer or the document cannot be read.
    pdf::PDFTextLayout* mutableTextLayout(int pageIndex, QString* unavailableReason) const;

    /// One result from one selection, with page-space geometry per line span.
    TextSelectionResult resultFrom(const pdf::PDFTextLayout& layout, const pdf::PDFTextSelection& selection, int pageIndex) const;

    /// Reading-order ordinal of (blockIndex, lineIndex), or -1.
    int lineOrdinal(const pdf::PDFTextLayout& layout, size_t blockIndex, size_t lineIndex) const;

    void dropLayoutCacheIfStale() const;

    QPointer<pdf::PDFDocumentContext> m_context;
    mutable pdf::PDFRevisionIdentity m_layoutRevision;
    mutable QHash<int, pdf::PDFTextLayout> m_layouts;
};

}   // namespace pdfinteraction

#endif   // PDFDOCUMENTTEXTSELECTIONSOURCE_H
