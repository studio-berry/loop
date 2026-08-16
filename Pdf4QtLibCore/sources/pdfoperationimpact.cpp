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

#include <algorithm>

namespace pdf
{

namespace
{

QJsonArray domainNames(PDFEvidenceDomains domains)
{
    QJsonArray names;
    for (PDFEvidenceDomain domain : { PDFEvidenceDomain::Images,
                                      PDFEvidenceDomain::Colorants,
                                      PDFEvidenceDomain::Strokes,
                                      PDFEvidenceDomain::OverprintTransparency,
                                      PDFEvidenceDomain::Fonts })
    {
        if (domains.testFlag(domain))
        {
            names.append(pdfEvidenceDomainToString(domain));
        }
    }
    return names;
}

}   // namespace

bool PDFOperationImpact::isFullRevalidation() const
{
    return !impactComplete || documentWide || requiresIndependentOracle || domains == PDFEvidenceDomains();
}

QJsonObject PDFOperationImpact::toJson() const
{
    QJsonArray pageArray;
    QList<int> sortedPages = pages.values();
    std::sort(sortedPages.begin(), sortedPages.end());
    for (int page : sortedPages)
    {
        pageArray.append(page);
    }

    return QJsonObject{
        { QStringLiteral("domains"), domainNames(domains) },
        { QStringLiteral("pages"), pageArray },
        { QStringLiteral("object_ids"), QJsonArray::fromStringList(objectIds) },
        { QStringLiteral("document_wide"), documentWide },
        { QStringLiteral("full_rewrite"), fullRewrite },
        { QStringLiteral("impact_complete"), impactComplete },
        { QStringLiteral("requires_independent_oracle"), requiresIndependentOracle }
    };
}

QJsonObject PDFRevalidationPlan::toJson() const
{
    QJsonArray pageArray;
    QList<int> sortedPages = pages.values();
    std::sort(sortedPages.begin(), sortedPages.end());
    for (int page : sortedPages)
    {
        pageArray.append(page);
    }

    return QJsonObject{
        { QStringLiteral("full"), full },
        { QStringLiteral("check_ids"), QJsonArray::fromStringList(checkIds) },
        { QStringLiteral("pages"), pageArray },
        { QStringLiteral("reason"), reason }
    };
}

std::optional<PDFEvidenceDomain> preflightEvidenceDomainForCheck(const QString& checkId)
{
    if (checkId == QLatin1String("image-resolution"))
    {
        return PDFEvidenceDomain::Images;
    }
    if (checkId == QLatin1String("color-mode") || checkId == QLatin1String("color-inventory"))
    {
        return PDFEvidenceDomain::Colorants;
    }
    if (checkId == QLatin1String("thin-strokes"))
    {
        return PDFEvidenceDomain::Strokes;
    }
    if (checkId == QLatin1String("white-overprint") || checkId == QLatin1String("transparency-risk"))
    {
        return PDFEvidenceDomain::OverprintTransparency;
    }
    if (checkId == QLatin1String("embedded-fonts"))
    {
        return PDFEvidenceDomain::Fonts;
    }
    return std::nullopt;
}

PDFRevalidationPlan planRevalidation(const PDFOperationImpact& impact,
                                     const QStringList& enabledCheckIds)
{
    PDFRevalidationPlan plan;
    plan.pages = impact.pages;

    if (!impact.impactComplete)
    {
        plan.full = true;
        plan.checkIds = enabledCheckIds;
        plan.reason = QStringLiteral("impact-incomplete");
        return plan;
    }
    if (impact.requiresIndependentOracle)
    {
        plan.full = true;
        plan.checkIds = enabledCheckIds;
        plan.reason = QStringLiteral("independent-oracle");
        return plan;
    }
    if (impact.documentWide || impact.domains == PDFEvidenceDomains())
    {
        plan.full = true;
        plan.checkIds = enabledCheckIds;
        plan.reason = impact.documentWide ? QStringLiteral("document-wide") : QStringLiteral("unspecified-domains");
        return plan;
    }

    for (const QString& checkId : enabledCheckIds)
    {
        const std::optional<PDFEvidenceDomain> domain = preflightEvidenceDomainForCheck(checkId);
        if (!domain.has_value())
        {
            plan.full = true;
            plan.checkIds = enabledCheckIds;
            plan.reason = QStringLiteral("unmapped-check");
            return plan;
        }
        if (impact.domains.testFlag(*domain))
        {
            plan.checkIds.append(checkId);
        }
    }

    plan.full = false;
    plan.reason = QStringLiteral("targeted");
    return plan;
}

}   // namespace pdf
