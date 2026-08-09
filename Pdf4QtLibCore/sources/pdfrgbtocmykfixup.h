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

#ifndef PDFRGBTOCMYKFIXUP_H
#define PDFRGBTOCMYKFIXUP_H

#include "pdfdocument.h"
#include "pdfglobal.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pdf
{

enum class PDFRgbToCmykOutputIntentPolicy
{
    PreserveMatching,
    Replace
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRgbToCmykSettings
{
    QByteArray targetIccData;
    QByteArray targetIccId;
    QString targetProfileName;

    QByteArray fallbackRgbIccData;
    QByteArray fallbackRgbIccId;

    RenderingIntent intent = RenderingIntent::RelativeColorimetric;
    bool blackPointCompensation = true;

    bool preserveGray = true;
    bool preserveSpotColors = true;
    bool embedOutputIntent = true;
    bool revalidate = true;

    QString pageRange = QStringLiteral("-");
    bool dryRunOnly = false;

    PDFRgbToCmykOutputIntentPolicy outputIntentPolicy =
        PDFRgbToCmykOutputIntentPolicy::Replace;
};

enum class PDFRgbToCmykObjectKind
{
    VectorPaint,
    Image,
    InlineImage,
    Form,
    AnnotationAppearance,
    IndexedPalette,
    Shading,
    Pattern
};

struct PDFRgbToCmykUnsupportedItem
{
    PDFInteger pageIndex = -1;
    PDFObjectReference objectReference;
    PDFRgbToCmykObjectKind kind = PDFRgbToCmykObjectKind::VectorPaint;
    QString reason;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRgbToCmykReport
{
    int vectorPaintsConverted = 0;
    int imagesConverted = 0;
    int indexedPalettesConverted = 0;
    int formsVisited = 0;
    int annotationAppearancesVisited = 0;

    bool outputIntentChanged = false;
    QVector<PDFRgbToCmykUnsupportedItem> unsupported;
    QStringList warnings;
    bool postflightPassed = false;
};

class PDF4QTLIBCORESHARED_EXPORT PDFRgbToCmykFixup
{
public:
    static PDFOperationResult convertRgbToCmyk(const PDFDocument* document,
                                    const PDFRgbToCmykSettings& settings,
                                    PDFRgbToCmykReport* report);
};

} // namespace pdf

#endif // PDFRGBTOCMYKFIXUP_H
