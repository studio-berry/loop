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

#ifndef PDFTHINPARTPROBE_H
#define PDFTHINPARTPROBE_H

#include "pdfglobal.h"

#include <QPainterPath>
#include <QRectF>
#include <QString>

namespace pdf
{

struct LOOPLIBCORESHARED_EXPORT PDFThinPartMeasurement
{
    bool measured = false;
    qreal widthPt = 0.0;
    qreal precisionPt = 0.0;
    QRectF bbox;
};

/// Measures the thinnest local limb of a filled page-space path. The mask is
/// bounded to the path bounds, and the raster budget is charged before the
/// image is allocated. When invert is true, the measurement is taken over the
/// bounded negative space inside the path bounds.
LOOPLIBCORESHARED_EXPORT PDFThinPartMeasurement measureThinPartPath(const QPainterPath& pagePath,
                                                                      int dpi,
                                                                      qint64 maxRasterPixels,
                                                                      bool invert,
                                                                      const QString& context);

} // namespace pdf

#endif // PDFTHINPARTPROBE_H
