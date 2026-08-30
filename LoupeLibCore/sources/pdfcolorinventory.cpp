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

#include "pdfcolorinventory.h"

#include "pdfcms.h"
#include "pdfcatalog.h"
#include "pdfconstants.h"
#include "pdfdocument.h"
#include "pdfdocumentsession.h"
#include "pdfpage.h"

#include <QTransform>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace pdf
{

bool isRichBlackPixel(PDFConstColorBuffer buffer,
                      const PDFPixelFormat& format,
                      PDFColorComponent kThreshold)
{
    if (format.getProcessColorChannelCount() != 4)
    {
        return false;
    }

    const uint8_t start = format.getProcessColorChannelIndexStart();
    const PDFColorComponent chromaticInk = buffer[start] + buffer[start + 1] + buffer[start + 2];
    const PDFColorComponent blackInk = buffer[start + 3];
    return blackInk > kThreshold && !qFuzzyIsNull(chromaticInk);
}

PDFColorInventory::PDFColorInventory(PDFDocumentSession* session) :
    m_session(session)
{
}

PDFColorInventoryResult PDFColorInventory::inspect(const PDFColorInventorySettings& settings) const
{
    PDFColorInventoryResult result;
    if (!m_session || !m_session->isValid() || settings.probeDpi <= 0)
    {
        return result;
    }

    PDFDocument* document = m_session->getDocument();
    if (!document)
    {
        return result;
    }

    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);

    PDFInkMapper inkMapper(&cmsManager, document);
    inkMapper.createSpotColors(true);

    const std::vector<PDFInkMapper::ColorInfo> colors = inkMapper.getSeparations(4, true);
    QList<PDFColorInventoryInk> processColors;
    QList<PDFColorInventoryInk> spotColors;
    for (const PDFInkMapper::ColorInfo& color : colors)
    {
        PDFColorInventoryInk ink;
        ink.name = color.name.isEmpty() ? color.textName : QString::fromLatin1(color.name);
        ink.displayColor = color.color;
        ink.isSpot = color.isSpot;
        if (ink.isSpot)
        {
            spotColors.append(ink);
        }
        else
        {
            processColors.append(ink);
        }
    }

    std::sort(spotColors.begin(), spotColors.end(), [](const PDFColorInventoryInk& lhs, const PDFColorInventoryInk& rhs)
              { return QString::compare(lhs.name, rhs.name, Qt::CaseInsensitive) < 0; });

    result.spotColors = spotColors;
    result.separations = processColors;
    result.separations.append(spotColors);

    const PDFCatalog* catalog = document->getCatalog();
    if (!catalog)
    {
        return result;
    }
    const PDFInteger pageCount = catalog->getPageCount();
    PDFTransparencyRendererSettings renderSettings;
    renderSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayImages, true);
    renderSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayText, true);
    renderSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayVectorGraphics, true);
    renderSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayShadings, true);
    renderSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayTilingPatterns, true);
    renderSettings.flags.setFlag(PDFTransparencyRendererSettings::SaveOriginalProcessImage, true);
    renderSettings.flags.setFlag(PDFTransparencyRendererSettings::SeparationSimulation,
                                 inkMapper.getActiveSpotColorCount() > 0);
    renderSettings.renderPolicy = PDFRenderPolicy::forPreflightAnalysis();

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        const QSizeF mediaSize = page->getRotatedMediaBox().size();
        const double widthPxReal = std::ceil(mediaSize.width() * PDF_POINT_TO_INCH * settings.probeDpi);
        const double heightPxReal = std::ceil(mediaSize.height() * PDF_POINT_TO_INCH * settings.probeDpi);
        if (!std::isfinite(widthPxReal) || !std::isfinite(heightPxReal) || widthPxReal <= 0.0 || heightPxReal <= 0.0 || widthPxReal > double(std::numeric_limits<int>::max()) || heightPxReal > double(std::numeric_limits<int>::max()))
        {
            continue;
        }

        const QSize imageSize(qMax(1, int(widthPxReal)), qMax(1, int(heightPxReal)));
        const QTransform pagePointToDevice = PDFRenderer::createPagePointToDevicePointMatrix(
            page, QRect(QPoint(0, 0), imageSize));
        PDFTransparencyRenderer renderer(page,
                                         document,
                                         m_session->getFontCache(),
                                         m_session->getCMS(),
                                         m_session->getOptionalContentActivity(),
                                         &inkMapper,
                                         renderSettings,
                                         pagePointToDevice);
        renderer.beginPaint(imageSize);
        renderer.processContents();
        renderer.endPaint();

        const PDFFloatBitmapWithColorSpace bitmap = renderer.getOriginalProcessBitmap();
        const PDFPixelFormat format = bitmap.getPixelFormat();
        if (format.getProcessColorChannelCount() != 4 || bitmap.getWidth() == 0 || bitmap.getHeight() == 0)
        {
            continue;
        }

        size_t richBlackPixelCount = 0;
        for (size_t y = 0; y < bitmap.getHeight(); ++y)
        {
            for (size_t x = 0; x < bitmap.getWidth(); ++x)
            {
                if (isRichBlackPixel(bitmap.getPixel(x, y), format, settings.richBlackKThreshold))
                {
                    ++richBlackPixelCount;
                }
            }
        }

        if (richBlackPixelCount == 0)
        {
            continue;
        }

        const QSizeF pageSizeMM = page->getRotatedMediaBoxMM().size();
        const qreal pixelArea = (pageSizeMM.width() * pageSizeMM.height()) / qreal(bitmap.getWidth() * bitmap.getHeight());
        result.richBlackPages.append(PDFRichBlackInventory{
            int(pageIndex + 1),
            qreal(richBlackPixelCount) * pixelArea });
    }

    return result;
}

}   // namespace pdf
