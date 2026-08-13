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

#ifndef PDFCOLORINVENTORY_H
#define PDFCOLORINVENTORY_H

#include "pdfglobal.h"
#include "pdftransparencyrenderer.h"

#include <QColor>
#include <QList>
#include <QString>

namespace pdf
{

class PDFDocumentSession;

struct PDF4QTLIBCORESHARED_EXPORT PDFColorInventoryInk
{
    QString name;
    QColor displayColor;
    bool isSpot = false;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFRichBlackInventory
{
    int page = 0;
    qreal areaMM2 = 0.0;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFColorInventoryResult
{
    QList<PDFColorInventoryInk> separations;
    QList<PDFColorInventoryInk> spotColors;
    QList<PDFRichBlackInventory> richBlackPages;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFColorInventorySettings
{
    int probeDpi = 150;
    qreal richBlackKThreshold = 0.10;
};

/// Shared rich-black predicate used by preflight and Output Preview.
PDF4QTLIBCORESHARED_EXPORT bool isRichBlackPixel(PDFConstColorBuffer buffer,
                                                 const PDFPixelFormat& format,
                                                 PDFColorComponent kThreshold);

class PDF4QTLIBCORESHARED_EXPORT PDFColorInventory
{
public:
    explicit PDFColorInventory(PDFDocumentSession* session);

    PDFColorInventoryResult inspect(const PDFColorInventorySettings& settings) const;

private:
    PDFDocumentSession* m_session = nullptr;
};

} // namespace pdf

#endif // PDFCOLORINVENTORY_H
