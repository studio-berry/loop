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
#include <utility>

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
    return !declared || !impactComplete || documentWide || fullRewrite || requiresIndependentOracle || domains == PDFEvidenceDomains();
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
        { QStringLiteral("declared"), declared },
        { QStringLiteral("all_pages"), allPages },
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
        { QStringLiteral("invalidated_domains"), domainNames(invalidatedDomains) },
        { QStringLiteral("reuse_prior_evidence"), reusePriorEvidence },
        { QStringLiteral("requires_independent_oracle"), requiresIndependentOracle },
        { QStringLiteral("reason"), reason }
    };
}

QJsonObject PDFEvidenceRevalidation::toJson() const
{
    return QJsonObject{
        { QStringLiteral("reused_evidence_ids"), QJsonArray::fromStringList(reusedEvidenceIds) },
        { QStringLiteral("recomputed_evidence_ids"), QJsonArray::fromStringList(recomputedEvidenceIds) }
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
                                     const QStringList& enabledCheckIds,
                                     bool hasDocumentPolicy)
{
    PDFRevalidationPlan plan;
    plan.pages = impact.allPages ? QSet<int>() : impact.pages;
    plan.invalidatedDomains = impact.domains;
    plan.requiresIndependentOracle = impact.requiresIndependentOracle;

    const auto selectFull = [&plan, &enabledCheckIds](const QString& reason)
    {
        plan.full = true;
        plan.checkIds = enabledCheckIds;
        plan.invalidatedDomains = pdfEvidenceAllDomains();
        plan.reusePriorEvidence = false;
        plan.reason = reason;
        return plan;
    };

    if (!impact.declared)
    {
        return selectFull(QStringLiteral("impact-undeclared"));
    }
    if (!impact.impactComplete)
    {
        return selectFull(QStringLiteral("impact-incomplete"));
    }
    if (impact.requiresIndependentOracle)
    {
        return selectFull(QStringLiteral("independent-oracle"));
    }
    if (impact.fullRewrite)
    {
        return selectFull(QStringLiteral("full-rewrite"));
    }
    if (impact.documentWide)
    {
        return selectFull(QStringLiteral("document-wide"));
    }
    if (hasDocumentPolicy)
    {
        return selectFull(QStringLiteral("document-policy"));
    }
    if (impact.domains == PDFEvidenceDomains())
    {
        return selectFull(QStringLiteral("unspecified-domains"));
    }

    for (const QString& checkId : enabledCheckIds)
    {
        const std::optional<PDFEvidenceDomain> domain = preflightEvidenceDomainForCheck(checkId);
        if (!domain.has_value())
        {
            return selectFull(QStringLiteral("unmapped-check"));
        }
        if (impact.domains.testFlag(*domain))
        {
            plan.checkIds.append(checkId);
        }
    }

    if (plan.checkIds.isEmpty())
    {
        return selectFull(QStringLiteral("no-targeted-checks"));
    }

    plan.full = false;
    plan.reusePriorEvidence = true;
    plan.reason = QStringLiteral("targeted");
    return plan;
}

PDFOperationImpact combineOperationImpacts(const QList<PDFOperationImpact>& impacts)
{
    PDFOperationImpact combined;
    combined.declared = !impacts.isEmpty();
    combined.impactComplete = !impacts.isEmpty();
    for (const PDFOperationImpact& impact : impacts)
    {
        if (!impact.declared)
        {
            combined.declared = false;
        }
        if (!impact.impactComplete)
        {
            combined.impactComplete = false;
        }
        if (impact.allPages)
        {
            combined.allPages = true;
        }
        if (impact.documentWide)
        {
            combined.documentWide = true;
        }
        if (impact.requiresIndependentOracle)
        {
            combined.requiresIndependentOracle = true;
        }
        if (impact.fullRewrite)
        {
            combined.fullRewrite = true;
        }
        combined.domains |= impact.domains;
        combined.pages.unite(impact.pages);
        for (const QString& objectId : impact.objectIds)
        {
            if (!combined.objectIds.contains(objectId))
            {
                combined.objectIds.append(objectId);
            }
        }
    }
    return combined;
}

PDFEvidenceRevalidation reconcileEvidenceForRevalidation(const PDFEvidenceGraph& previous,
                                                         const PDFEvidenceGraph& recomputed,
                                                         const PDFRevalidationPlan& plan)
{
    PDFEvidenceRevalidation result;
    result.graph = recomputed;

    const auto markRecomputed = [&result]()
    {
        for (PDFEvidenceRecord& record : result.graph.records)
        {
            record.extra.insert(QStringLiteral("revalidation"), QStringLiteral("recomputed"));
            result.recomputedEvidenceIds.append(record.id);
        }
    };

    if (plan.full || !plan.reusePriorEvidence || !previous.isComplete() || !recomputed.isComplete())
    {
        markRecomputed();
        return result;
    }

    result.graph.records.clear();
    const auto isInvalidated = [&plan](const PDFEvidenceRecord& record)
    {
        if (!plan.invalidatedDomains.testFlag(record.domain))
        {
            return false;
        }
        return plan.pages.isEmpty() || plan.pages.contains(record.page);
    };

    for (PDFEvidenceRecord record : previous.records)
    {
        if (isInvalidated(record))
        {
            continue;
        }
        record.artifact = recomputed.artifact;
        record.revision = recomputed.revision;
        record.extra.insert(QStringLiteral("revalidation"), QStringLiteral("reused"));
        record.extra.insert(QStringLiteral("reused_from_revision"), previous.revision.toString());
        result.reusedEvidenceIds.append(record.id);
        result.graph.records.append(std::move(record));
    }

    for (PDFEvidenceRecord record : recomputed.records)
    {
        if (!isInvalidated(record))
        {
            continue;
        }
        record.extra.insert(QStringLiteral("revalidation"), QStringLiteral("recomputed"));
        result.recomputedEvidenceIds.append(record.id);
        result.graph.records.append(std::move(record));
    }

    return result;
}

}   // namespace pdf
