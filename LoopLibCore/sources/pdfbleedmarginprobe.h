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

#ifndef PDFBLEEDMARGINPROBE_H
#define PDFBLEEDMARGINPROBE_H

#include "pdfglobal.h"
#include "pdfbleedfixup.h"
#include "pdfdocumentsession.h"

#include <QRectF>

namespace pdf
{

struct LOOPLIBCORESHARED_EXPORT PDFBleedMarginProbeSettings
{
    int dpi = 150;
    int threshold = 16;
    qreal whiteCoverageThreshold = 0.9975;
    PDFBleedFixupSettings::ReferenceBox referenceBox = PDFBleedFixupSettings::ReferenceBox::TrimBox;
    QMarginsF bleedMM = QMarginsF(3.0, 3.0, 3.0, 3.0);
    bool fastOnly = false;
    qint64 maxRasterPixels = 250LL * 1000 * 1000;
    /// Share of a bleed strip that artwork must cover (by bounds) or ink (by raster)
    /// before the edge counts as populated. A margin holding only a few stray marks is
    /// below it and is reported as empty.
    qreal minEdgeCoverage = 0.10;
};

struct LOOPLIBCORESHARED_EXPORT PDFBleedMarginProbeEdgeResult
{
    bool hasContent = false;
    int inkPixels = 0;
    int totalPixels = 0;
    QRectF stripRect;
    /// Upper bound of the share of the strip covered by artwork bounds (0..1), set by the fast pass.
    qreal boundsCoverage = 0.0;
    /// False when hasContent rests on bounds that touch enough of the strip, but the filled
    /// geometry (images, filled paths and glyphs, clipped) paints less than minEdgeCoverage of
    /// it, and no raster measurement confirmed it.
    bool confirmed = true;
};

struct LOOPLIBCORESHARED_EXPORT PDFBleedMarginProbeResult
{
    PDFBleedMarginProbeEdgeResult left;
    PDFBleedMarginProbeEdgeResult top;
    PDFBleedMarginProbeEdgeResult right;
    PDFBleedMarginProbeEdgeResult bottom;

    bool allEdgesCovered() const
    {
        return left.hasContent && right.hasContent && top.hasContent && bottom.hasContent;
    }

    bool allEdgesConfirmed() const
    {
        return left.confirmed && right.confirmed && top.confirmed && bottom.confirmed;
    }
};

/// Probes whether rendered artwork on a page extends into the bleed margin.
///
/// Fast path: measures how much of each bleed strip the bounding rects from
/// `PDFPrecompiledPage::calculateGraphicPieceInfos` cover. A strip is populated when that
/// coverage reaches `minEdgeCoverage`, so a few stray marks do not count. No rasterization.
/// The edge is confirmed only when images and filled paths, cut by their clip, actually paint
/// `minEdgeCoverage` of the strip; strokes alone leave it unconfirmed.
///
/// Raster path (raster_confirm): renders the four edge strips at probe_dpi and counts
/// non-background pixels against `minEdgeCoverage`. It confirms or demotes every edge the bounds
/// pass could not prove solid, and upgrades empty edges that the raster finds inked.
class LOOPLIBCORESHARED_EXPORT PDFBleedMarginProbe
{
public:
    explicit PDFBleedMarginProbe(PDFDocumentSession* session);

    /// Full probe: fast bounds pass first, then raster confirmation if the settings request it
    /// and the fast pass left any side empty or unconfirmed.
    PDFBleedMarginProbeResult probe(const PDFPage* page,
                                    size_t pageIndex,
                                    const PDFBleedMarginProbeSettings& settings);

    /// Fast vector-content bounds pass only. No rasterization.
    PDFBleedMarginProbeResult probeFast(const PDFPage* page,
                                        size_t pageIndex,
                                        const PDFBleedMarginProbeSettings& settings);

private:
    static bool pixelIsInk(const QImage& image, int x, int y, int threshold);
    PDFBleedMarginProbeResult probeRaster(const PDFPage* page,
                                          size_t pageIndex,
                                          const PDFBleedMarginProbeSettings& settings,
                                          const QRectF& reference,
                                          const QRectF& targetBleed);

    PDFDocumentSession* m_session;
};

}   // namespace pdf

#endif   // PDFBLEEDMARGINPROBE_H
