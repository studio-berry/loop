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

struct LOUPELIBCORESHARED_EXPORT PDFInkCoverageProbeSettings
{
    /// Maximum allowed total area coverage, as a sum of colorant values (3.0 == 300%).
    qreal maxInkCoverage = 3.0;
    /// Rasterization resolution for the coverage probe.
    int dpi = 150;
    /// Regions smaller than this fraction of the page are ignored (antialiasing noise).
    qreal minRegionAreaRatio = 0.0005;
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

struct LOUPELIBCORESHARED_EXPORT PDFInkCoverageRegion
{
    QRectF bbox;                 // page points, PDF coordinate space
    qreal areaMM2 = 0.0;
    qreal peakInkCoverage = 0.0; // max TAC found inside the region
};

struct LOUPELIBCORESHARED_EXPORT PDFInkCoverageProbeResult
{
    bool rasterized = false;     // false when rasterization was unavailable or over budget
    bool budgetExceeded = false;
    qreal peakInkCoverage = 0.0; // page-wide max TAC
    qreal overLimitAreaMM2 = 0.0;
    std::vector<PDFInkCoverageRegion> regions;  // sorted by areaMM2, descending
};

class LOUPELIBCORESHARED_EXPORT PDFInkCoverageProbe
{
public:
    explicit PDFInkCoverageProbe(PDFDocumentSession* session);

    PDFInkCoverageProbeResult probe(const PDFPage* page,
                                    size_t pageIndex,
                                    const PDFInkCoverageProbeSettings& settings);

private:
    PDFDocumentSession* m_session;
};

} // namespace pdf

#endif // PDFINKCOVERAGEPROBE_H
