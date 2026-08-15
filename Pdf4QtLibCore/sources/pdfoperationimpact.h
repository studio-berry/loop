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

#ifndef PDFOPERATIONIMPACT_H
#define PDFOPERATIONIMPACT_H

#include "pdfevidencegraph.h"
#include "pdfglobal.h"

#include <QJsonObject>
#include <QStringList>

namespace pdf
{

struct PDF4QTLIBCORESHARED_EXPORT PDFOperationImpact
{
    PDFEvidenceDomains domains;
    QList<int> pages;
    QStringList objectIds;
    bool documentWide = false;
    bool fullRewrite = false;
    bool impactComplete = false;

    bool requiresFullRevalidation() const { return !impactComplete || documentWide || fullRewrite; }
    QJsonObject toJson() const;
};

PDF4QTLIBCORESHARED_EXPORT PDFOperationImpact mergePDFOperationImpact(const PDFOperationImpact& first,
                                                                      const PDFOperationImpact& second);

}   // namespace pdf

#endif   // PDFOPERATIONIMPACT_H
