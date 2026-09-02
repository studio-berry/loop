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

#ifndef PDFBLEEDFIXUP_H
#define PDFBLEEDFIXUP_H

#include "pdfglobal.h"
#include "pdfdocument.h"
#include "pdfrenderer.h"
#include "pdfutils.h"

#include <QImage>
#include <QMarginsF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace pdf
{

enum class PDFBleedFixupMode
{
    Mirror,
    PixelRepeat,
    Stretch
};

enum class PDFBleedFixupSide
{
    Left = 0,
    Bottom = 1,
    Right = 2,
    Top = 3
};

using PDFBleedFixupSideMask = quint8;

constexpr PDFBleedFixupSideMask bleedFixupSideBit(PDFBleedFixupSide side)
{
    return PDFBleedFixupSideMask(1u << static_cast<quint8>(side));
}

constexpr PDFBleedFixupSideMask PDFBleedFixupAllSides =
        bleedFixupSideBit(PDFBleedFixupSide::Left)
        | bleedFixupSideBit(PDFBleedFixupSide::Bottom)
        | bleedFixupSideBit(PDFBleedFixupSide::Right)
        | bleedFixupSideBit(PDFBleedFixupSide::Top);

constexpr bool isBleedFixupSideEnabled(PDFBleedFixupSideMask sides, PDFBleedFixupSide side)
{
    return (sides & bleedFixupSideBit(side)) != 0;
}

struct LOOPLIBCORESHARED_EXPORT PDFBleedFixupSettings
{
    enum class ReferenceBox
    {
        CropBox,
        TrimBox,
        MediaBox
    };

    PDFBleedFixupMode mode = PDFBleedFixupMode::Mirror;
    QString pageRange = "-";
    ReferenceBox referenceBox = ReferenceBox::TrimBox;
    QMarginsF bleedMM = QMarginsF(3.0, 3.0, 3.0, 3.0); ///< left, top, right, bottom
    PDFBleedFixupSideMask sides = PDFBleedFixupAllSides;
    bool expandMediaBox = true;
    bool expandCropBox = true;
    bool expandBleedBox = true;
    bool expandTrimBox = false;
    int dpi = 300;
    int samplePixels = 1; ///< PixelRepeat / Stretch sample depth
    bool skipIfAlreadyBleeding = true;
    bool force = false;

    /// Report box changes only; do not rasterize or paint bleed artwork. The page
    /// raster is sized by page area x dpi^2, so a dry run must not pay for it.
    bool analyzeOnly = false;

    /// Upper bound on the page raster used for edge sampling. Large-format work
    /// (signage, banners) at 300 dpi otherwise reaches multi-gigabyte
    /// allocations; fail with an actionable message instead.
    qint64 maxRasterPixels = 250LL * 1000 * 1000;
    PDFRenderer::Features renderFeatures = PDFRenderer::Features(PDFRenderer::Antialiasing | PDFRenderer::TextAntialiasing);
};

struct LOOPLIBCORESHARED_EXPORT PDFBleedFixupPageReport
{
    PDFInteger pageIndex = 0;
    QRectF originalMediaBox;
    QRectF originalCropBox;
    QRectF originalBleedBox;
    QRectF originalTrimBox;
    QRectF newMediaBox;
    QRectF newCropBox;
    QRectF newBleedBox;
    QRectF newTrimBox;
    PDFBleedFixupSideMask sidesRequested = 0;
    PDFBleedFixupSideMask sidesEligible = 0;
    QVector<PDFBleedFixupSide> sidesApplied;
    QStringList skipReasons;
};

struct LOOPLIBCORESHARED_EXPORT PDFBleedFixupReport
{
    QVector<PDFBleedFixupPageReport> pages;
};

/// Shared helpers exposed for unit tests (rect / skip / strip image builders).
namespace PDFBleedFixupMath
{

struct LOOPLIBCORESHARED_EXPORT PDFBleedRasterPlan
{
    QSize imageSize;
    double pixelCount = 0.0;
    bool rasterRequired = false;
    bool withinBudget = true;
    QString errorMessage;
};

LOOPLIBCORESHARED_EXPORT PDFBleedRasterPlan planRaster(const QSizeF& mediaSize,
                                                        int dpi,
                                                        qint64 maxRasterPixels,
                                                        bool analyzeOnly,
                                                        bool hasEligibleSides);

LOOPLIBCORESHARED_EXPORT QRectF referenceRect(const PDFPage* page, PDFBleedFixupSettings::ReferenceBox referenceBox);
LOOPLIBCORESHARED_EXPORT QRectF targetBleedRect(const QRectF& reference, const QMarginsF& bleedMM);
LOOPLIBCORESHARED_EXPORT QRectF expandBoxTo(const QRectF& box, const QRectF& target);
LOOPLIBCORESHARED_EXPORT bool sideAlreadyBleeding(const QRectF& reference,
                                                    const QRectF& bleedBox,
                                                    PDFBleedFixupSide side,
                                                    PDFReal requiredBleedPt);
LOOPLIBCORESHARED_EXPORT PDFReal sideBleedMM(const QMarginsF& bleedMM, PDFBleedFixupSide side);
LOOPLIBCORESHARED_EXPORT int stripWidthPx(PDFBleedFixupMode mode, int bleedDepthPx, int samplePixels);
LOOPLIBCORESHARED_EXPORT QRectF edgeStripSourceRect(const QRectF& reference,
                                                      PDFBleedFixupSide side,
                                                      PDFReal depthPt);
LOOPLIBCORESHARED_EXPORT QRectF edgeStripDestRect(const QRectF& reference,
                                                    PDFBleedFixupSide side,
                                                    PDFReal depthPt);
LOOPLIBCORESHARED_EXPORT QRectF cornerStripSourceRect(const QRectF& reference,
                                                        PDFBleedFixupSide horizontal,
                                                        PDFBleedFixupSide vertical,
                                                        PDFReal horizontalDepthPt,
                                                        PDFReal verticalDepthPt);
LOOPLIBCORESHARED_EXPORT QRectF cornerStripDestRect(const QRectF& reference,
                                                      PDFBleedFixupSide horizontal,
                                                      PDFBleedFixupSide vertical,
                                                      PDFReal horizontalDepthPt,
                                                      PDFReal verticalDepthPt);
LOOPLIBCORESHARED_EXPORT QImage buildEdgeFillImage(const QImage& pageImage,
                                                     const QRect& sourcePx,
                                                     PDFBleedFixupSide side,
                                                     PDFBleedFixupMode mode,
                                                     int bleedDepthPx);
LOOPLIBCORESHARED_EXPORT QImage buildCornerFillImage(const QImage& pageImage,
                                                       const QRect& sourcePx,
                                                       PDFBleedFixupSide horizontal,
                                                       PDFBleedFixupSide vertical,
                                                       PDFBleedFixupMode mode,
                                                       int destWidthPx,
                                                       int destHeightPx);

} // namespace PDFBleedFixupMath

class LOOPLIBCORESHARED_EXPORT PDFBleedFixup
{
public:
    static PDFOperationResult apply(PDFDocument* document,
                                    const PDFBleedFixupSettings& settings,
                                    PDFBleedFixupReport* report = nullptr,
                                    PDFModifiedDocument::ModificationFlags* modificationFlags = nullptr);
};

} // namespace pdf

#endif // PDFBLEEDFIXUP_H
