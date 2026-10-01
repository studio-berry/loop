// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit purposes only
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

#include "pdfbleedmarginprobe.h"

#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocument.h"
#include "pdfoptionalcontent.h"
#include "pdfpage.h"
#include "pdfpainter.h"
#include "pdfrenderer.h"
#include "pdffont.h"
#include "pdfprocessingbudget.h"

#include <QImage>
#include <QPainter>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace pdf
{

namespace
{

QRectF referenceRectFromPage(const PDFPage* page, PDFBleedFixupSettings::ReferenceBox referenceBox)
{
    if (!page)
    {
        return QRectF();
    }

    switch (referenceBox)
    {
        case PDFBleedFixupSettings::ReferenceBox::MediaBox:
            return page->getMediaBox();
        case PDFBleedFixupSettings::ReferenceBox::CropBox:
            return page->getCropBox();
        case PDFBleedFixupSettings::ReferenceBox::TrimBox:
        default:
            return page->getTrimBox().isValid() ? page->getTrimBox() : page->getCropBox();
    }
}

QRectF targetBleedRect(const QRectF& reference, const QMarginsF& bleedMM)
{
    const PDFReal left = bleedMM.left() * PDF_MM_TO_POINT;
    const PDFReal right = bleedMM.right() * PDF_MM_TO_POINT;
    const PDFReal top = bleedMM.top() * PDF_MM_TO_POINT;
    const PDFReal bottom = bleedMM.bottom() * PDF_MM_TO_POINT;
    return QRectF(reference.left() - left,
                  reference.top() - bottom,
                  reference.width() + left + right,
                  reference.height() + top + bottom)
        .normalized();
}

QRect mapPageRectToImage(const QRectF& pageRect, const QTransform& pageToDevice, const QSize& imageSize)
{
    const QRectF mapped = pageToDevice.mapRect(pageRect).normalized();
    return mapped.toAlignedRect().intersected(QRect(QPoint(0, 0), imageSize));
}

QRectF sideStripRect(const QRectF& reference, PDFBleedFixupSide side, PDFReal depthPt)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left:
            return QRectF(reference.left() - depthPt, reference.top(), depthPt, reference.height());
        case PDFBleedFixupSide::Right:
            return QRectF(reference.right(), reference.top(), depthPt, reference.height());
        case PDFBleedFixupSide::Bottom:
            return QRectF(reference.left(), reference.top() - depthPt, reference.width(), depthPt);
        case PDFBleedFixupSide::Top:
            return QRectF(reference.left(), reference.bottom(), reference.width(), depthPt);
    }
    return QRectF();
}

PDFReal sideBleedPt(const QMarginsF& bleedMM, PDFBleedFixupSide side)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left:
            return bleedMM.left() * PDF_MM_TO_POINT;
        case PDFBleedFixupSide::Right:
            return bleedMM.right() * PDF_MM_TO_POINT;
        case PDFBleedFixupSide::Top:
            return bleedMM.top() * PDF_MM_TO_POINT;
        case PDFBleedFixupSide::Bottom:
            return bleedMM.bottom() * PDF_MM_TO_POINT;
    }
    return 0.0;
}

bool isAxisAlignedRectangle(const QPainterPath& path, const QRectF& bounds)
{
    constexpr qreal Tolerance = 0.01;
    const QList<QPolygonF> subpaths = path.toSubpathPolygons();
    if (subpaths.size() != 1)
    {
        return false;
    }

    const QPolygonF& polygon = subpaths.front();
    if (polygon.size() < 4 || polygon.size() > 5)
    {
        return false;
    }

    // Every vertex is a bounds corner, consecutive vertices share an axis (no diagonal, so no
    // bowtie) and the four corners are distinct (no doubled-back triangle).
    int corners = 0;
    for (int index = 0; index < polygon.size(); ++index)
    {
        const QPointF& point = polygon[index];
        const bool onLeft = std::abs(point.x() - bounds.left()) <= Tolerance;
        const bool onRight = std::abs(point.x() - bounds.right()) <= Tolerance;
        const bool onTop = std::abs(point.y() - bounds.top()) <= Tolerance;
        const bool onBottom = std::abs(point.y() - bounds.bottom()) <= Tolerance;
        if (!(onLeft || onRight) || !(onTop || onBottom))
        {
            return false;
        }
        corners |= (onLeft ? 1 : 2) << (onTop ? 0 : 2);

        const QPointF& next = polygon[(index + 1) % polygon.size()];
        if (std::abs(next.x() - point.x()) > Tolerance && std::abs(next.y() - point.y()) > Tolerance)
        {
            return false;
        }
    }
    return corners == 0xF;
}

/// True when the piece's bounding rect is fully painted: an image, a shading or a filled rectangle.
/// A stroke-only rectangle (a keyline) inks only its outline, so it does not count.
bool pieceFillsItsBounds(const PDFPrecompiledPage::GraphicPieceInfo& info)
{
    if (info.isImage() || info.isShading())
    {
        return true;
    }
    return info.isVectorGraphics() && info.isFilled && isAxisAlignedRectangle(info.pagePath, info.boundingRect);
}

struct StripCoverage
{
    qreal upper = 0.0;   ///< Share of the strip touched by any piece's bounds.
    qreal solid = 0.0;   ///< Share of the strip covered by pieces that fill their bounds.
};

qreal mergedLength(std::vector<std::pair<qreal, qreal>>& intervals)
{
    constexpr qreal Gap = 0.01;
    std::sort(intervals.begin(), intervals.end());

    qreal total = 0.0;
    qreal currentLo = 0.0;
    qreal currentHi = 0.0;
    bool open = false;
    for (const std::pair<qreal, qreal>& interval : intervals)
    {
        if (open && interval.first <= currentHi + Gap)
        {
            currentHi = qMax(currentHi, interval.second);
            continue;
        }
        if (open)
        {
            total += currentHi - currentLo;
        }
        currentLo = interval.first;
        currentHi = interval.second;
        open = true;
    }
    if (open)
    {
        total += currentHi - currentLo;
    }
    return total;
}

/// Splits the strip into bands across its depth and measures, per band, the length along the strip
/// that is touched by piece bounds (upper) and fully crossed by a solid piece (solid).
/// \param fillsBounds pieceFillsItsBounds() of each piece, computed once per page
StripCoverage measureStripCoverage(const PDFPrecompiledPage::GraphicPieceInfos& pieces,
                                   const std::vector<bool>& fillsBounds,
                                   const QRectF& strip,
                                   PDFBleedFixupSide side)
{
    constexpr int Bands = 8;
    constexpr qreal Tolerance = 0.01;

    const bool vertical = side == PDFBleedFixupSide::Left || side == PDFBleedFixupSide::Right;
    const qreal length = vertical ? strip.height() : strip.width();
    const qreal depth = vertical ? strip.width() : strip.height();
    if (!(length > 0.0) || !(depth > 0.0))
    {
        return StripCoverage();
    }

    const qreal alongLo = vertical ? strip.top() : strip.left();
    const qreal alongHi = alongLo + length;
    const qreal acrossLo = vertical ? strip.left() : strip.top();

    StripCoverage coverage;
    for (int band = 0; band < Bands; ++band)
    {
        const qreal bandLo = acrossLo + depth * band / Bands;
        const qreal bandHi = acrossLo + depth * (band + 1) / Bands;

        std::vector<std::pair<qreal, qreal>> touching;
        std::vector<std::pair<qreal, qreal>> solid;
        for (size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex)
        {
            const QRectF& rect = pieces[pieceIndex].boundingRect;
            if (!rect.isValid())
            {
                continue;
            }

            const qreal rectAcrossLo = vertical ? rect.left() : rect.top();
            const qreal rectAcrossHi = vertical ? rect.right() : rect.bottom();
            const qreal rectAlongLo = qMax(alongLo, vertical ? rect.top() : rect.left());
            const qreal rectAlongHi = qMin(alongHi, vertical ? rect.bottom() : rect.right());
            if (!(rectAlongHi > rectAlongLo) || !(rectAcrossHi > bandLo) || !(rectAcrossLo < bandHi))
            {
                continue;
            }

            touching.emplace_back(rectAlongLo, rectAlongHi);
            if (rectAcrossLo <= bandLo + Tolerance && rectAcrossHi >= bandHi - Tolerance && fillsBounds[pieceIndex])
            {
                solid.emplace_back(rectAlongLo, rectAlongHi);
            }
        }

        coverage.upper += mergedLength(touching) / length;
        coverage.solid += mergedLength(solid) / length;
    }

    coverage.upper /= Bands;
    coverage.solid /= Bands;
    return coverage;
}

}   // namespace

PDFBleedMarginProbe::PDFBleedMarginProbe(PDFDocumentSession* session) :
    m_session(session)
{
}

PDFBleedMarginProbeResult PDFBleedMarginProbe::probe(const PDFPage* page,
                                                     size_t pageIndex,
                                                     const PDFBleedMarginProbeSettings& settings)
{
    PDFBleedMarginProbeResult result = probeFast(page, pageIndex, settings);

    if (settings.fastOnly || (result.allEdgesCovered() && result.allEdgesConfirmed()) || !m_session)
    {
        return result;
    }

    const QRectF reference = referenceRectFromPage(page, settings.referenceBox);
    if (!reference.isValid() || reference.isEmpty())
    {
        return result;
    }

    const QRectF target = targetBleedRect(reference, settings.bleedMM);
    if (!target.isValid() || target.isEmpty())
    {
        return result;
    }

    PDFBleedMarginProbeResult rasterResult = probeRaster(page, pageIndex, settings, reference, target);

    // Raster confirmation decides every edge the bounds pass could not prove solid:
    //  - an empty edge is upgraded when the raster sees substantial margin ink (trim-edge
    //    antialiasing on an otherwise empty strip stays below the floor);
    //  - an edge that bounds called populated is demoted when the raster finds it too sparse.
    // An edge the raster could not measure (strip over budget) keeps its bounds verdict and
    // stays unconfirmed, so the caller can report the gap instead of passing it.
    auto applyRaster = [&settings](PDFBleedMarginProbeEdgeResult& target, const PDFBleedMarginProbeEdgeResult& rasterEdge)
    {
        if (rasterEdge.totalPixels <= 0)
        {
            return;
        }

        const qreal inkCoverage = static_cast<qreal>(rasterEdge.inkPixels) / static_cast<qreal>(rasterEdge.totalPixels);
        const bool populated = inkCoverage >= settings.minEdgeCoverage;
        const qreal boundsCoverage = target.boundsCoverage;
        if (target.confirmed)
        {
            // Solid by bounds, or no applicable bleed: keep the verdict, record the calibration.
            target.inkPixels = rasterEdge.inkPixels;
            target.totalPixels = rasterEdge.totalPixels;
            target.stripRect = rasterEdge.stripRect;
            if (!target.hasContent && populated)
            {
                target = rasterEdge;
                target.hasContent = true;
                target.boundsCoverage = boundsCoverage;
            }
            return;
        }

        target = rasterEdge;
        target.hasContent = populated;
        target.boundsCoverage = boundsCoverage;
        target.confirmed = true;
    };

    applyRaster(result.left, rasterResult.left);
    applyRaster(result.right, rasterResult.right);
    applyRaster(result.top, rasterResult.top);
    applyRaster(result.bottom, rasterResult.bottom);

    return result;
}

PDFBleedMarginProbeResult PDFBleedMarginProbe::probeFast(const PDFPage* page,
                                                         size_t pageIndex,
                                                         const PDFBleedMarginProbeSettings& settings)
{
    PDFBleedMarginProbeResult result;

    if (!page || !m_session)
    {
        return result;
    }

    const QRectF reference = referenceRectFromPage(page, settings.referenceBox);
    if (!reference.isValid() || reference.isEmpty())
    {
        return result;
    }

    const PDFPrecompiledPage* compiled = m_session->compilePage(pageIndex);
    if (!compiled || !compiled->isValid())
    {
        return result;
    }

    // Use the existing calculateGraphicPieceInfos to extract per-piece bounds.
    const PDFPrecompiledPage::GraphicPieceInfos infos = compiled->calculateGraphicPieceInfos(page->getMediaBox(), 0.01);

    QRectF contentBounds;
    for (const PDFPrecompiledPage::GraphicPieceInfo& info : infos)
    {
        if (info.boundingRect.isValid())
        {
            contentBounds = contentBounds.united(info.boundingRect);
        }
    }

    if (!contentBounds.isValid() || contentBounds.isEmpty())
    {
        // No content at all — all sides missing.
        return result;
    }

    std::vector<bool> fillsBounds;
    fillsBounds.reserve(infos.size());
    for (const PDFPrecompiledPage::GraphicPieceInfo& info : infos)
    {
        fillsBounds.push_back(pieceFillsItsBounds(info));
    }

    const PDFBleedFixupSide sides[4] = {
        PDFBleedFixupSide::Left, PDFBleedFixupSide::Right,
        PDFBleedFixupSide::Top, PDFBleedFixupSide::Bottom
    };

    for (PDFBleedFixupSide side : sides)
    {
        const PDFReal depthPt = sideBleedPt(settings.bleedMM, side);
        if (!(depthPt > 0.0))
        {
            PDFBleedMarginProbeEdgeResult edgeResult;
            edgeResult.hasContent = true;
            switch (side)
            {
                case PDFBleedFixupSide::Left:
                    result.left = edgeResult;
                    break;
                case PDFBleedFixupSide::Right:
                    result.right = edgeResult;
                    break;
                case PDFBleedFixupSide::Top:
                    result.top = edgeResult;
                    break;
                case PDFBleedFixupSide::Bottom:
                    result.bottom = edgeResult;
                    break;
            }
            continue;
        }

        const QRectF strip = sideStripRect(reference, side, depthPt);
        PDFBleedMarginProbeEdgeResult edgeResult;
        edgeResult.stripRect = strip;

        // A strip is populated only when the artwork bounds cover enough of it; a few stray
        // marks that merely touch it are not bleed.
        const StripCoverage coverage = measureStripCoverage(infos, fillsBounds, strip, side);
        edgeResult.boundsCoverage = coverage.upper;
        edgeResult.hasContent = contentBounds.intersects(strip) && coverage.upper >= settings.minEdgeCoverage;
        edgeResult.confirmed = !edgeResult.hasContent || coverage.solid >= settings.minEdgeCoverage;

        switch (side)
        {
            case PDFBleedFixupSide::Left:
                result.left = edgeResult;
                break;
            case PDFBleedFixupSide::Right:
                result.right = edgeResult;
                break;
            case PDFBleedFixupSide::Top:
                result.top = edgeResult;
                break;
            case PDFBleedFixupSide::Bottom:
                result.bottom = edgeResult;
                break;
        }
    }

    return result;
}

PDFBleedMarginProbeResult PDFBleedMarginProbe::probeRaster(const PDFPage* page,
                                                           size_t pageIndex,
                                                           const PDFBleedMarginProbeSettings& settings,
                                                           const QRectF& reference,
                                                           const QRectF&)
{
    PDFBleedMarginProbeResult result;

    if (!page || !m_session)
    {
        return result;
    }

    PDFDocument* document = m_session->getDocument();
    if (!document)
    {
        return result;
    }

    // Set up rendering infrastructure (same pattern as PDFBleedFixup).
    PDFOptionalContentActivity optionalContentActivity(document, OCUsage::Export, nullptr);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument md(document, &optionalContentActivity);
    fontCache.setDocument(md);
    fontCache.setCacheShrinkEnabled(nullptr, false);

    PDFRenderer::Features features = PDFRenderer::Features(PDFRenderer::Antialiasing | PDFRenderer::TextAntialiasing);
    features.setFlag(PDFRenderer::ClipToCropBox, false);
    features.setFlag(PDFRenderer::DisplayAnnotations, false);

    const QSizeF mediaSize = page->getRotatedMediaBox().size();
    if (settings.dpi <= 0 || !std::isfinite(mediaSize.width()) || !std::isfinite(mediaSize.height()) || mediaSize.width() <= 0.0 || mediaSize.height() <= 0.0)
    {
        return result;
    }

    const PDFReal pointToPixel = settings.dpi / 72.0;
    const double fullWidthPxReal = std::ceil(mediaSize.width() * pointToPixel);
    const double fullHeightPxReal = std::ceil(mediaSize.height() * pointToPixel);

    if (!std::isfinite(fullWidthPxReal) || !std::isfinite(fullHeightPxReal) || fullWidthPxReal > static_cast<double>(std::numeric_limits<int>::max()) || fullHeightPxReal > static_cast<double>(std::numeric_limits<int>::max()))
    {
        return result;
    }

    const int fullW = qMax(1, int(fullWidthPxReal));
    const int fullH = qMax(1, int(fullHeightPxReal));
    PDFMeshQualitySettings meshQualitySettings;
    PDFRenderer renderer(document,
                         &fontCache,
                         cms.get(),
                         &optionalContentActivity,
                         features,
                         meshQualitySettings,
                         m_session->getProcessingBudget());
    const QTransform pageToDevice = PDFRenderer::createPagePointToDevicePointMatrix(page, QRect(QPoint(0, 0), QSize(fullW, fullH)));

    const PDFBleedFixupSide sides[4] = {
        PDFBleedFixupSide::Left, PDFBleedFixupSide::Right,
        PDFBleedFixupSide::Top, PDFBleedFixupSide::Bottom
    };

    for (PDFBleedFixupSide side : sides)
    {
        const PDFReal depthPt = sideBleedPt(settings.bleedMM, side);
        if (!(depthPt > 0.0))
        {
            PDFBleedMarginProbeEdgeResult edgeResult;
            edgeResult.hasContent = true;
            switch (side)
            {
                case PDFBleedFixupSide::Left:
                    result.left = edgeResult;
                    break;
                case PDFBleedFixupSide::Right:
                    result.right = edgeResult;
                    break;
                case PDFBleedFixupSide::Top:
                    result.top = edgeResult;
                    break;
                case PDFBleedFixupSide::Bottom:
                    result.bottom = edgeResult;
                    break;
            }
            continue;
        }

        const QRectF stripRect = sideStripRect(reference, side, depthPt);
        if (!stripRect.isValid() || stripRect.isEmpty())
        {
            continue;
        }

        const QRect stripPxRect = mapPageRectToImage(stripRect, pageToDevice, QSize(fullW, fullH));
        if (stripPxRect.isEmpty() || stripPxRect.width() <= 0 || stripPxRect.height() <= 0)
        {
            continue;
        }

        const double pixelCount = double(stripPxRect.width()) * double(stripPxRect.height());
        if (settings.maxRasterPixels > 0 && pixelCount > double(settings.maxRasterPixels))
        {
            continue;
        }

        QImage stripImage(stripPxRect.size(), QImage::Format_ARGB32_Premultiplied);
        stripImage.fill(Qt::white);
        QPainter stripPainter(&stripImage);
        stripPainter.translate(-stripPxRect.left(), -stripPxRect.top());
        renderer.render(&stripPainter, pageToDevice, pageIndex);
        stripPainter.end();

        int inkCount = 0;
        const int pixelCountInt = stripImage.width() * stripImage.height();

        for (int y = 0; y < stripImage.height(); ++y)
        {
            for (int x = 0; x < stripImage.width(); ++x)
            {
                if (pixelIsInk(stripImage, x, y, settings.threshold))
                {
                    ++inkCount;
                }
            }
        }

        PDFBleedMarginProbeEdgeResult edgeResult;
        const qreal inkCoverage = pixelCountInt > 0 ? static_cast<qreal>(inkCount) / static_cast<qreal>(pixelCountInt) : 0.0;
        edgeResult.hasContent = inkCoverage > (1.0 - settings.whiteCoverageThreshold);
        edgeResult.inkPixels = inkCount;
        edgeResult.totalPixels = pixelCountInt;
        edgeResult.stripRect = stripRect;

        switch (side)
        {
            case PDFBleedFixupSide::Left:
                result.left = edgeResult;
                break;
            case PDFBleedFixupSide::Right:
                result.right = edgeResult;
                break;
            case PDFBleedFixupSide::Top:
                result.top = edgeResult;
                break;
            case PDFBleedFixupSide::Bottom:
                result.bottom = edgeResult;
                break;
        }
    }

    return result;
}

bool PDFBleedMarginProbe::pixelIsInk(const QImage& image, int x, int y, int threshold)
{
    if (image.isNull() || x < 0 || x >= image.width() || y < 0 || y >= image.height())
    {
        return false;
    }

    const QRgb pixel = image.pixel(x, y);

    if (qAlpha(pixel) < threshold)
    {
        return false;
    }

    // Premultiplied white/near-white background is empty margin, not ink.
    if (qRed(pixel) >= 255 - threshold && qGreen(pixel) >= 255 - threshold && qBlue(pixel) >= 255 - threshold)
    {
        return false;
    }

    return true;
}

}   // namespace pdf
