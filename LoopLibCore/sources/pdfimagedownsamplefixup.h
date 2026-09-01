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

#ifndef PDFIMAGEDOWNSAMPLEFIXUP_H
#define PDFIMAGEDOWNSAMPLEFIXUP_H

#include "pdfdocument.h"
#include "pdfglobal.h"
#include "pdfimageoptimizer.h"
#include "pdfutils.h"

#include <QVector>

namespace pdf
{

struct LOOPLIBCORESHARED_EXPORT PDFImageDownsampleFixupSettings
{
    int targetDpi = 300;
    int jpegQuality = 90;
    bool preserveTransparency = true;
    bool keepOriginalIfLarger = true;
    bool preserveColorMode = true;
};

struct LOOPLIBCORESHARED_EXPORT PDFImageDownsampleFixupReport
{
    int imagesExamined = 0;
    int imagesChanged = 0;
    int imagesSkipped = 0;
    qint64 originalBytes = 0;
    qint64 resultingBytes = 0;
    std::vector<PDFImageOptimizer::ImageResult> images;
};

class LOOPLIBCORESHARED_EXPORT PDFImageDownsampleFixup
{
public:
    /// Applies conservative downsampling to a document in place. Callers that
    /// must preserve the source document should pass a document copy.
    static PDFOperationResult apply(PDFDocument* document,
                                    const PDFImageDownsampleFixupSettings& settings,
                                    PDFImageDownsampleFixupReport* report = nullptr);
};

} // namespace pdf

#endif // PDFIMAGEDOWNSAMPLEFIXUP_H
