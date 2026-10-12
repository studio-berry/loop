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

#ifndef PDFDOCUMENTIMAGEEXTRACTOR_H
#define PDFDOCUMENTIMAGEEXTRACTOR_H

#include "interactionglobal.h"

#include "pdfdocumentcontext.h"

#include <QImage>
#include <QList>
#include <QPointer>
#include <QString>

namespace pdfinteraction
{

/// One image a page's content stream paints.
struct PageImage
{
    int pageIndex = -1;

    /// Paint order within the page, from zero. The same ordinal PdfTool's
    /// fetch-images reports, so the two surfaces name an image identically.
    int order = 0;

    QImage image;
};

/// Extracts the images a page paints, in paint order.
///
/// The extraction is Core's: the same PDFPageContentProcessor walk PdfTool's
/// `fetch-images` uses, so a Quick extraction and a CLI extraction of the same
/// page produce the same images in the same order. The context is observed
/// through a QPointer, so a destroyed context degrades to "no document" rather
/// than a dangling read.
class PDFDocumentImageExtractor final
{
    Q_DECLARE_TR_FUNCTIONS(pdfinteraction::PDFDocumentImageExtractor)

public:
    explicit PDFDocumentImageExtractor(pdf::PDFDocumentContext* context);

    /// Images `pageIndex` paints, in paint order. Returns an empty list with
    /// `typedError` set when the page cannot be read.
    QList<PageImage> extractPage(int pageIndex, QString* typedError) const;

private:
    QPointer<pdf::PDFDocumentContext> m_context;
};

}   // namespace pdfinteraction

#endif   // PDFDOCUMENTIMAGEEXTRACTOR_H
