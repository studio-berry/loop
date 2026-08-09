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

#include "pdfinkcoverageprobe.h"

#include "pdfcatalog.h"
#include "pdfpage.h"
#include "pdfrenderer.h"
#include "pdftransparencyrenderer.h"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pdf
{

namespace
{

bool isUsableBox(const QRectF& box)
{
    return box.isValid()
        && std::isfinite(box.left())
        && std::isfinite(box.top())
        && std::isfinite(box.width())
        && std::isfinite(box.height())
        && box.width() > 0.0
        && box.height() > 0.0;
}

QRectF resolveAnalysisBox(const PDFPage* page, PDFInkCoverageAnalysisBox requested)
{
    if (!page)
    {
        return QRectF();
    }

    const QRectF media = page->getMediaBox().normalized();
    const QRectF crop = page->getCropBox().normalized();
    const QRectF trim = page->getTrimBox().normalized();
    const QRectF bleed = page->getBleedBox().normalized();

    switch (requested)
    {
        case PDFInkCoverageAnalysisBox::Media:
            return isUsableBox(media) ? media : QRectF();
        case PDFInkCoverageAnalysisBox::Crop:
            return isUsableBox(crop) ? crop : media;
        case PDFInkCoverageAnalysisBox::Trim:
            if (page->hasTrimBox() && isUsableBox(trim))
            {
                return trim;
            }
            return isUsableBox(crop) ? crop : media;
        case PDFInkCoverageAnalysisBox::Bleed:
            if (page->hasBleedBox() && isUsableBox(bleed))
            {
                return bleed;
            }
            if (page->hasTrimBox() && isUsableBox(trim))
            {
                return trim;
            }
            return isUsableBox(crop) ? crop : media;
    }

    return media;
}

} // namespace

PDFInkCoverageProbe::PDFInkCoverageProbe(PDFDocumentSession* session) :
    m_session(session)
{
}

PDFInkCoverageProbeResult PDFInkCoverageProbe::probe(const PDFPage* page,
                                                      size_t pageIndex,
                                                      const PDFInkCoverageProbeSettings& settings)
{
    PDFInkCoverageProbeResult result;
    Q_UNUSED(pageIndex);

    if (!page || !m_session)
    {
        return result;
    }

    PDFDocument* document = m_session->getDocument();
    if (!document)
    {
        return result;
    }

    const QRectF analysisBox = resolveAnalysisBox(page, settings.analysisBox);
    if (!isUsableBox(analysisBox))
    {
        return result;
    }

    const PageRotation pageRotation = page->getPageRotation();
    const QRectF rotatedAnalysisBox = PDFPage::getRotatedBox(analysisBox, pageRotation).normalized();
    const QSizeF mediaSize = rotatedAnalysisBox.size();
    const qreal pointToPixel = static_cast<qreal>(settings.dpi) / 72.0;
    const double widthReal = std::ceil(mediaSize.width() * pointToPixel);
    const double heightReal = std::ceil(mediaSize.height() * pointToPixel);

    if (!std::isfinite(widthReal) || !std::isfinite(heightReal)
        || widthReal <= 0.0 || heightReal <= 0.0
        || widthReal > static_cast<double>(std::numeric_limits<int>::max())
        || heightReal > static_cast<double>(std::numeric_limits<int>::max()))
    {
        return result;
    }

    const int width = qMax(1, static_cast<int>(widthReal));
    const int height = qMax(1, static_cast<int>(heightReal));
    const qint64 rasterPixels = static_cast<qint64>(width) * static_cast<qint64>(height);
    if (settings.maxRasterPixels > 0 && rasterPixels > settings.maxRasterPixels)
    {
        result.budgetExceeded = true;
        return result;
    }

    PDFTransparencyRendererSettings rendererSettings;
    rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::SaveOriginalProcessImage, true);
    rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::ActiveColorMask, false);
    rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::SeparationSimulation, true);
    rendererSettings.activeColorMask = PDFPixelFormat::getAllColorsMask();

    const QSize imageSize(width, height);
    const QTransform pagePointToDevice = PDFRenderer::createMediaBoxToDevicePointMatrix(
        rotatedAnalysisBox,
        QRect(QPoint(0, 0), imageSize),
        pageRotation);
    PDFInkMapper inkMapper(nullptr, document);
    inkMapper.createSpotColors(true);

    PDFTransparencyRenderer renderer(page,
                                     document,
                                     m_session->getFontCache(),
                                     m_session->getCMS(),
                                     m_session->getOptionalContentActivity(),
                                     &inkMapper,
                                     rendererSettings,
                                     pagePointToDevice);
    renderer.beginPaint(imageSize);
    renderer.processContents();
    renderer.endPaint();

    const PDFFloatBitmapWithColorSpace bitmap = renderer.getOriginalProcessBitmap();
    const size_t bitmapWidth = bitmap.getWidth();
    const size_t bitmapHeight = bitmap.getHeight();
    if (bitmapWidth == 0 || bitmapHeight == 0)
    {
        return result;
    }

    result.rasterized = true;
    const size_t totalPixels = bitmapWidth * bitmapHeight;
    std::vector<bool> overLimit(totalPixels, false);

    size_t overLimitPixels = 0;
    for (size_t y = 0; y < bitmapHeight; ++y)
    {
        for (size_t x = 0; x < bitmapWidth; ++x)
        {
            const qreal inkCoverage = bitmap.getPixelInkCoverage(x, y);
            result.peakInkCoverage = qMax(result.peakInkCoverage, inkCoverage);
            if (inkCoverage > settings.maxInkCoverage)
            {
                overLimit[y * bitmapWidth + x] = true;
                ++overLimitPixels;
            }
        }
    }

    const QSizeF pageSizeMM = page->getRectMM(analysisBox).size();
    const qreal pixelAreaMM2 = (pageSizeMM.width() * pageSizeMM.height()) / static_cast<qreal>(totalPixels);
    result.overLimitAreaMM2 = static_cast<qreal>(overLimitPixels) * pixelAreaMM2;

    struct DeviceRegion
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        size_t pixelCount = 0;
        qreal peakInkCoverage = 0.0;
    };

    std::vector<DeviceRegion> deviceRegions;
    std::vector<QPoint> pending;
    pending.reserve(256);
    const int directions[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

    for (size_t y = 0; y < bitmapHeight; ++y)
    {
        for (size_t x = 0; x < bitmapWidth; ++x)
        {
            const size_t startIndex = y * bitmapWidth + x;
            if (!overLimit[startIndex])
            {
                continue;
            }

            DeviceRegion region;
            region.left = region.right = static_cast<int>(x);
            region.top = region.bottom = static_cast<int>(y);
            pending.clear();
            pending.emplace_back(static_cast<int>(x), static_cast<int>(y));
            overLimit[startIndex] = false;

            while (!pending.empty())
            {
                const QPoint point = pending.back();
                pending.pop_back();

                ++region.pixelCount;
                region.left = qMin(region.left, point.x());
                region.right = qMax(region.right, point.x());
                region.top = qMin(region.top, point.y());
                region.bottom = qMax(region.bottom, point.y());
                region.peakInkCoverage = qMax(region.peakInkCoverage, bitmap.getPixelInkCoverage(static_cast<size_t>(point.x()), static_cast<size_t>(point.y())));

                for (const auto& direction : directions)
                {
                    const int neighborX = point.x() + direction[0];
                    const int neighborY = point.y() + direction[1];
                    if (neighborX < 0 || neighborY < 0
                        || neighborX >= static_cast<int>(bitmapWidth)
                        || neighborY >= static_cast<int>(bitmapHeight))
                    {
                        continue;
                    }

                    const size_t neighborIndex = static_cast<size_t>(neighborY) * bitmapWidth + static_cast<size_t>(neighborX);
                    if (overLimit[neighborIndex])
                    {
                        overLimit[neighborIndex] = false;
                        pending.emplace_back(neighborX, neighborY);
                    }
                }
            }

            const qreal areaRatio = static_cast<qreal>(region.pixelCount) / static_cast<qreal>(totalPixels);
            if (areaRatio < settings.minRegionAreaRatio)
            {
                continue;
            }

            deviceRegions.push_back(qMove(region));
        }
    }

    bool invertible = false;
    const QTransform deviceToPage = pagePointToDevice.inverted(&invertible);
    if (!invertible)
    {
        result.regions.clear();
        return result;
    }

    std::sort(deviceRegions.begin(), deviceRegions.end(), [](const DeviceRegion& lhs, const DeviceRegion& rhs)
    {
        if (lhs.pixelCount != rhs.pixelCount)
        {
            return lhs.pixelCount > rhs.pixelCount;
        }
        if (lhs.top != rhs.top)
        {
            return lhs.top < rhs.top;
        }
        return lhs.left < rhs.left;
    });

    if (settings.maxRegionsPerPage >= 0 && static_cast<int>(deviceRegions.size()) > settings.maxRegionsPerPage)
    {
        deviceRegions.resize(static_cast<size_t>(settings.maxRegionsPerPage));
    }

    result.regions.reserve(deviceRegions.size());
    for (const DeviceRegion& region : deviceRegions)
    {
        PDFInkCoverageRegion coverageRegion;
        coverageRegion.bbox = deviceToPage.mapRect(QRectF(region.left,
                                                          region.top,
                                                          region.right - region.left + 1,
                                                          region.bottom - region.top + 1)).normalized();
        coverageRegion.areaMM2 = static_cast<qreal>(region.pixelCount) * pixelAreaMM2;
        coverageRegion.peakInkCoverage = region.peakInkCoverage;
        result.regions.push_back(qMove(coverageRegion));
    }

    return result;
}

} // namespace pdf
