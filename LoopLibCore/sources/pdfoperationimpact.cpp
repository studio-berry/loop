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
    if (impactComplete && !mutatesDocument)
    {
        return false;
    }
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
        { QStringLiteral("requires_independent_oracle"), requiresIndependentOracle },
        { QStringLiteral("mutates_document"), mutatesDocument }
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
        { QStringLiteral("reused_check_ids"), QJsonArray::fromStringList(reusedCheckIds) },
        { QStringLiteral("invalidated_evidence_domains"), domainNames(invalidatedEvidenceDomains) },
        { QStringLiteral("recomputed_evidence_domains"), domainNames(recomputedEvidenceDomains) },
        { QStringLiteral("reusable_evidence_domains"), domainNames(reusableEvidenceDomains) },
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
        plan.reusedCheckIds.clear();
        // A full run never advertises a misleading narrowed page scope (#129).
        plan.pages.clear();
        plan.invalidatedDomains = pdfEvidenceAllDomains();
        plan.invalidatedEvidenceDomains = pdfEvidenceAllDomains();
        plan.reusableEvidenceDomains = PDFEvidenceDomains();
        plan.reusePriorEvidence = false;
        plan.reason = reason;
    };

    if (!impact.declared)
    {
        selectFull(QStringLiteral("impact-undeclared"));
        return plan;
    }
    if (impact.requiresIndependentOracle)
    {
        selectFull(QStringLiteral("independent-oracle"));
        return plan;
    }
    if (!impact.impactComplete)
    {
        selectFull(QStringLiteral("impact-incomplete"));
        return plan;
    }
    if (!impact.mutatesDocument)
    {
        plan.full = false;
        plan.reusedCheckIds = enabledCheckIds;
        plan.invalidatedEvidenceDomains = PDFEvidenceDomains();
        plan.reusableEvidenceDomains = pdfEvidenceAllDomains();
        plan.reusePriorEvidence = true;
        plan.reason = QStringLiteral("no-document-mutation");
        return plan;
    }
    if (impact.documentWide)
    {
        selectFull(QStringLiteral("document-wide"));
        return plan;
    }
    if (hasDocumentPolicy)
    {
        selectFull(QStringLiteral("document-policy"));
        return plan;
    }
    if (impact.fullRewrite)
    {
        selectFull(QStringLiteral("full-rewrite"));
        return plan;
    }
    if (impact.domains == PDFEvidenceDomains())
    {
        selectFull(QStringLiteral("unspecified-domains"));
        return plan;
    }

    plan.invalidatedEvidenceDomains = impact.domains;
    plan.reusableEvidenceDomains = pdfEvidenceAllDomains() & ~impact.domains;

    for (const QString& checkId : enabledCheckIds)
    {
        const std::optional<PDFEvidenceDomain> domain = preflightEvidenceDomainForCheck(checkId);
        if (!domain.has_value())
        {
            selectFull(QStringLiteral("unmapped-check"));
            return plan;
        }
        if (impact.domains.testFlag(*domain))
        {
            plan.checkIds.append(checkId);
        }
        else
        {
            plan.reusedCheckIds.append(checkId);
        }
    }

    if (plan.checkIds.isEmpty() && plan.reusedCheckIds.isEmpty())
    {
        selectFull(QStringLiteral("no-targeted-checks"));
        return plan;
    }

    plan.full = false;
    plan.reusePriorEvidence = true;
    plan.reason = QStringLiteral("targeted");
    return plan;
}

PDFOperationImpact combineOperationImpacts(const QList<PDFOperationImpact>& impacts)
{
    if (impacts.isEmpty())
    {
        return PDFOperationImpact();
    }

    PDFOperationImpact combined;
    combined.declared = !impacts.isEmpty();
    combined.impactComplete = !impacts.isEmpty();
    combined.mutatesDocument = false;
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
        if (impact.mutatesDocument)
        {
            combined.mutatesDocument = true;
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
