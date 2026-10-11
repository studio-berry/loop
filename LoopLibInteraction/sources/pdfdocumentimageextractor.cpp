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

#include "pdfdocumentimageextractor.h"

#include "pdfcatalog.h"
#include "pdfdocumentsession.h"
#include "pdfmeshqualitysettings.h"
#include "pdfpage.h"
#include "pdfpagecontentprocessor.h"

namespace pdfinteraction
{

namespace
{

/// Walks one page's content stream and keeps every painted image, in paint
/// order. The walk is Core's PDFPageContentProcessor -- the same one PdfTool's
/// fetch-images drives -- so both surfaces extract the same images the same way.
class PageImageProcessor final : public pdf::PDFPageContentProcessor
{
    using BaseClass = pdf::PDFPageContentProcessor;

public:
    PageImageProcessor(const pdf::PDFPage* page,
                       const pdf::PDFDocument* document,
                       const pdf::PDFFontCache* fontCache,
                       const pdf::PDFCMS* cms,
                       const pdf::PDFOptionalContentActivity* optionalContentActivity,
                       const pdf::PDFMeshQualitySettings& meshQualitySettings,
                       int pageIndex,
                       QList<PageImage>* images) :
        BaseClass(page, document, fontCache, cms, optionalContentActivity, QTransform(), meshQualitySettings),
        m_pageIndex(pageIndex),
        m_images(images)
    {
    }

protected:
    bool isContentSuppressedByOC(pdf::PDFObjectReference ocgOrOcmd) override
    {
        Q_UNUSED(ocgOrOcmd);
        return false;
    }

    bool isContentKindSuppressed(ContentKind kind) const override
    {
        switch (kind)
        {
            case ContentKind::Shapes:
            case ContentKind::Text:
            case ContentKind::Shading:
                return true;

            case ContentKind::Tiling:
            case ContentKind::Images:
                return false;   // Tiling can have images

            default:
            {
                Q_ASSERT(false);
                break;
            }
        }

        return false;
    }

    void performImagePainting(const QImage& image) override
    {
        PageImage pageImage;
        pageImage.pageIndex = m_pageIndex;
        pageImage.order = m_images->size();
        pageImage.image = image;
        m_images->push_back(pageImage);
    }

private:
    int m_pageIndex = -1;
    QList<PageImage>* m_images = nullptr;
};

}   // namespace

PDFDocumentImageExtractor::PDFDocumentImageExtractor(pdf::PDFDocumentContext* context) :
    m_context(context)
{
}

QList<PageImage> PDFDocumentImageExtractor::extractPage(int pageIndex, QString* typedError) const
{
    const auto refuse = [typedError](const QString& reason)
    {
        if (typedError)
        {
            *typedError = reason;
        }
        return QList<PageImage>();
    };

    pdf::PDFDocumentContext* context = m_context;
    if (!context)
    {
        return refuse(tr("No document is open."));
    }

    pdf::PDFDocumentSession* session = context->getSession();
    const pdf::PDFDocument* document = context->getDocument();
    if (!session || !document)
    {
        return refuse(tr("No document is open."));
    }

    const pdf::PDFCatalog* catalog = document->getCatalog();
    const bool pageInRange = catalog && pageIndex >= 0 &&
                             static_cast<size_t>(pageIndex) < catalog->getPageCount();
    const pdf::PDFPage* page = pageInRange ? catalog->getPage(static_cast<size_t>(pageIndex)) : nullptr;
    if (!page)
    {
        return refuse(tr("The page does not exist."));
    }

    const pdf::PDFMeshQualitySettings meshQuality;
    QList<PageImage> images;
    PageImageProcessor processor(page, document, session->getFontCache(), session->getCMS(),
                                 session->getOptionalContentActivity(), meshQuality, pageIndex, &images);
    processor.processContents();

    return images;
}

}   // namespace pdfinteraction
