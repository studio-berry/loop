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

#ifndef PDFCONTOURBLEEDFIXUP_H
#define PDFCONTOURBLEEDFIXUP_H

#include "pdfdocument.h"
#include "pdfproductiongeometry.h"
#include "pdfrenderer.h"

#include <QRectF>
#include <QVector>

namespace pdf
{

struct LOOPLIBCORESHARED_EXPORT PDFContourBleedFixupSettings
{
    double amountPt = 9.0;
    double flatteningTolerancePt = 0.1;
    int maxSegments = 100000;
    int dpi = 300;
    qint64 maxRasterPixels = 250LL * 1000 * 1000;
    bool analyzeOnly = false;
    PDFRenderer::Features renderFeatures = PDFRenderer::Features(PDFRenderer::Antialiasing | PDFRenderer::TextAntialiasing);
};

struct LOOPLIBCORESHARED_EXPORT PDFContourBleedFixupPageReport
{
    PDFInteger pageIndex = 0;
    QString contourId;
    QRectF sourceBounds;
    QRectF bleedBounds;
    bool planned = false;
    bool applied = false;
    QStringList warnings;
    QVector<PDFProductionDiagnostic> diagnostics;
};

struct LOOPLIBCORESHARED_EXPORT PDFContourBleedFixupReport
{
    QVector<PDFContourBleedFixupPageReport> pages;

    QJsonObject toJson() const;
};

class LOOPLIBCORESHARED_EXPORT PDFContourBleedFixup
{
public:
    static PDFOperationResult apply(PDFDocument* document,
                                    const PDFProductionGeometryModel& geometry,
                                    const PDFContourBleedFixupSettings& settings = {},
                                    PDFContourBleedFixupReport* report = nullptr,
                                    PDFModifiedDocument::ModificationFlags* modificationFlags = nullptr);
};

} // namespace pdf

#endif // PDFCONTOURBLEEDFIXUP_H
