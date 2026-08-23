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

#ifndef PDFTRANSPARENCYFLATTENER_H
#define PDFTRANSPARENCYFLATTENER_H

#include "pdfglobal.h"
#include "pdfutils.h"

#include <QJsonObject>
#include <QList>
#include <QRectF>
#include <QSize>
#include <QStringList>

#include <vector>

namespace pdf
{

class PDFDocument;
class PDFProgress;

struct LOUPELIBCORESHARED_EXPORT PDFTransparencyFlattenSettings
{
    int rasterizationDpi = 300;
    int lineArtResolutionDpi = 600;
    int textResolutionDpi = 600;
    qint64 maxRasterPixels = 250LL * 1000 * 1000;
    QString pageRange;
    bool preserveSpotColors = true;
    bool preserveTextAndVector = false;
    bool analyzeOnly = false;
};

struct LOUPELIBCORESHARED_EXPORT PDFTransparencyFlattenRegionReport
{
    int pageIndex = -1;
    QRectF bounds;
    QString reason;

    QJsonObject toJson() const;
};

struct LOUPELIBCORESHARED_EXPORT PDFTransparencyFlattenPageReport
{
    int pageIndex = -1;
    bool flattened = false;
    QSize rasterSize;
    QList<PDFTransparencyFlattenRegionReport> regions;
    QStringList warnings;

    QJsonObject toJson() const;
};

struct LOUPELIBCORESHARED_EXPORT PDFTransparencyFlattenReport
{
    bool changed = false;
    bool fullyOpaque = false;
    QStringList warnings;
    QList<PDFTransparencyFlattenPageReport> pages;

    QJsonObject toJson() const;
};

/// Converts selected PDF pages to opaque rendered images using the shared
/// transparency renderer. This is the common operation used by PdfTool and
/// PageMaster; no surface-specific implementation is permitted.
class LOUPELIBCORESHARED_EXPORT PDFTransparencyFlattener
{
public:
    static PDFOperationResult apply(PDFDocument* document,
                                    const PDFTransparencyFlattenSettings& settings,
                                    PDFTransparencyFlattenReport* report = nullptr,
                                    PDFProgress* progress = nullptr);

    /// Performs the structural revalidation used after flattening. This is a
    /// conservative document-wide check: unused objects containing transparency
    /// are also reported until the next normal PDF rewrite removes them.
    static bool hasLiveTransparency(const PDFDocument* document,
                                    QStringList* findings = nullptr);
};

} // namespace pdf

#endif // PDFTRANSPARENCYFLATTENER_H
