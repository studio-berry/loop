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

#include "pdfoperationimpact.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace pdf
{

QJsonObject PDFOperationImpact::toJson() const
{
    QJsonArray domainArray;
    const PDFEvidenceDomain all[] = {
        PDFEvidenceDomain::Images,
        PDFEvidenceDomain::Colorants,
        PDFEvidenceDomain::Strokes,
        PDFEvidenceDomain::OverprintTransparency,
        PDFEvidenceDomain::Fonts
    };
    for (PDFEvidenceDomain domain : all)
    {
        if (domains.testFlag(domain))
        {
            domainArray.append(pdfEvidenceDomainToString(domain));
        }
    }
    QJsonArray pageArray;
    for (int page : pages)
    {
        pageArray.append(page);
    }
    return QJsonObject{
        { QStringLiteral("domains"), domainArray },
        { QStringLiteral("pages"), pageArray },
        { QStringLiteral("object_ids"), QJsonArray::fromStringList(objectIds) },
        { QStringLiteral("document_wide"), documentWide },
        { QStringLiteral("full_rewrite"), fullRewrite },
        { QStringLiteral("impact_complete"), impactComplete }
    };
}

PDFOperationImpact mergePDFOperationImpact(const PDFOperationImpact& first,
                                           const PDFOperationImpact& second)
{
    PDFOperationImpact merged = first;
    merged.domains |= second.domains;
    merged.documentWide = first.documentWide || second.documentWide;
    merged.fullRewrite = first.fullRewrite || second.fullRewrite;
    merged.impactComplete = first.impactComplete && second.impactComplete;
    QSet<int> pages(first.pages.cbegin(), first.pages.cend());
    for (int page : second.pages)
    {
        pages.insert(page);
    }
    merged.pages = QList<int>(pages.cbegin(), pages.cend());
    std::sort(merged.pages.begin(), merged.pages.end());
    QSet<QString> objects(first.objectIds.cbegin(), first.objectIds.cend());
    for (const QString& objectId : second.objectIds)
    {
        objects.insert(objectId);
    }
    merged.objectIds = QStringList(objects.cbegin(), objects.cend());
    merged.objectIds.sort();
    return merged;
}

}   // namespace pdf
