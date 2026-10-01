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

#ifndef PDFINKCOVERAGEPROBE_H
#define PDFINKCOVERAGEPROBE_H

#include "pdfglobal.h"
#include "pdfdocumentsession.h"
#include "pdftransparencyrenderer.h"

#include <QRectF>

#include <vector>

namespace pdf
{

class PDFPage;

enum class PDFInkCoverageAnalysisBox
{
    Bleed,
    Trim,
    Crop,
    Media
};

struct LOOPLIBCORESHARED_EXPORT PDFInkCoverageProbeSettings
{
    /// Maximum allowed total area coverage, as a sum of colorant values (3.0 == 300%).
    qreal maxInkCoverage = 3.0;
    /// Rasterization resolution for the coverage probe.
    int dpi = 150;
    /// Smallest over-limit region, in mm^2, that is reported. The floor is physical rather
    /// than a fraction of the page: a 0.5 mm x 0.5 mm solid is about the smallest element a
    /// press reproduces as a distinct area rather than as dot gain, so it is the same on an
    /// A6 card and on an A0 poster.
    qreal minRegionAreaMM2 = 0.25;
    /// An over-limit region needs at least this many raster pixels to be told apart from
    /// antialiasing, so the raster can only vouch for regions of MinResolvablePixels pixels
    /// or more. When the floor is smaller than that, the probe result reports it.
    static constexpr size_t MinResolvablePixels = 4;
    /// Maximum number of regions reported per page; the largest are kept.
    int maxRegionsPerPage = 20;
    /// Maximum raster pixel count before the probe returns budgetExceeded.
    qint64 maxRasterPixels = 250LL * 1000 * 1000;
    /// Production region to analyze. Bleed falls back to Trim, Crop, then Media.
    PDFInkCoverageAnalysisBox analysisBox = PDFInkCoverageAnalysisBox::Bleed;
};

/// TAC findings are emitted only when coverage strictly exceeds the limit;
/// pixels exactly at the configured threshold are within policy.
inline bool inkCoverageExceedsLimit(qreal inkCoverage, qreal maxInkCoverage)
{
    return inkCoverage > maxInkCoverage;
}

struct LOOPLIBCORESHARED_EXPORT PDFInkCoverageRegion
{
    QRectF bbox;   // page points, PDF coordinate space
    qreal areaMM2 = 0.0;
    qreal peakInkCoverage = 0.0;   // max TAC found inside the region
};

struct LOOPLIBCORESHARED_EXPORT PDFInkCoverageProbeResult
{
    bool rasterized = false;   // false when rasterization was unavailable or over budget
    bool budgetExceeded = false;
    PDFRenderDiagnostics diagnostics;
    qreal peakInkCoverage = 0.0;   // page-wide max TAC
    qreal overLimitAreaMM2 = 0.0;
    /// Area of one raster pixel in mm^2 at the probe resolution.
    qreal pixelAreaMM2 = 0.0;
    /// Smallest region area the raster can resolve (MinResolvablePixels pixels). A configured
    /// floor below this cannot be honored, so a floor-sized element may go unseen.
    qreal minResolvableAreaMM2 = 0.0;
    std::vector<PDFInkCoverageRegion> regions;   // sorted by areaMM2, descending
};

class LOOPLIBCORESHARED_EXPORT PDFInkCoverageProbe
{
public:
    explicit PDFInkCoverageProbe(PDFDocumentSession* session);

    PDFInkCoverageProbeResult probe(const PDFPage* page,
                                    size_t pageIndex,
                                    const PDFInkCoverageProbeSettings& settings);

private:
    PDFDocumentSession* m_session;
};

struct LOOPLIBCORESHARED_EXPORT PDFOverprintProbeResult
{
    bool rendered = false;   // false when rasterization was unavailable or over budget
    bool budgetExceeded = false;
    PDFRenderDiagnostics diagnostics;
    PDFOverprintObservation observation;
};

/// Renders a page on the overprint-accurate compositor (the Output Preview path) and
/// reports the overprint that compositor applied, instead of reading page-view state.
class LOOPLIBCORESHARED_EXPORT PDFOverprintProbe
{
public:
    explicit PDFOverprintProbe(PDFDocumentSession* session);

    PDFOverprintProbeResult probe(const PDFPage* page, int dpi, qint64 maxRasterPixels);

private:
    PDFDocumentSession* m_session;
};

}   // namespace pdf

#endif   // PDFINKCOVERAGEPROBE_H
