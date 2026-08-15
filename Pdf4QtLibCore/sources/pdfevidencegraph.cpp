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

#include "pdfevidencegraph.h"

#include "pdfcms.h"
#include "pdfcolorinventory.h"
#include "pdfconstants.h"
#include "pdfdocumentsession.h"
#include "pdfexception.h"
#include "pdffont.h"
#include "pdfimage.h"
#include "pdfpagecontentprocessor.h"
#include "pdfprocessingbudget.h"

#include <QJsonArray>

#include <cmath>
#include <limits>

namespace pdf
{

QString pdfEvidenceDomainToString(PDFEvidenceDomain domain)
{
    switch (domain)
    {
        case PDFEvidenceDomain::Images:
            return QStringLiteral("images");
        case PDFEvidenceDomain::Colorants:
            return QStringLiteral("colorants");
        case PDFEvidenceDomain::Strokes:
            return QStringLiteral("strokes");
        case PDFEvidenceDomain::OverprintTransparency:
            return QStringLiteral("overprint-transparency");
        case PDFEvidenceDomain::Fonts:
            return QStringLiteral("fonts");
    }
    return QStringLiteral("unknown");
}

QJsonObject PDFEvidenceRecord::toJson() const
{
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("producer"), producer },
        { QStringLiteral("producer_version"), producerVersion },
        { QStringLiteral("domain"), pdfEvidenceDomainToString(domain) },
        { QStringLiteral("page"), page },
        { QStringLiteral("object_id"), objectId },
        { QStringLiteral("target"), target },
        { QStringLiteral("observed_value"), observedValue },
        { QStringLiteral("units"), units },
        { QStringLiteral("coverage_method"), coverageMethod },
        { QStringLiteral("fidelity"), fidelity },
        { QStringLiteral("confidence"), confidence },
        { QStringLiteral("incomplete_reason"), incompleteReason },
        { QStringLiteral("budget_context"), budgetContext }
    };
}

QList<PDFEvidenceRecord> PDFEvidenceGraph::recordsForDomain(PDFEvidenceDomain domain) const
{
    QList<PDFEvidenceRecord> matched;
    for (const PDFEvidenceRecord& record : records)
    {
        if (record.domain == domain)
        {
            matched.append(record);
        }
    }
    return matched;
}

QJsonObject PDFEvidenceGraph::toJson() const
{
    QJsonArray items;
    for (const PDFEvidenceRecord& record : records)
    {
        items.append(record.toJson());
    }
    return QJsonObject{
        { QStringLiteral("producer"), producer },
        { QStringLiteral("producer_version"), producerVersion },
        { QStringLiteral("complete"), isComplete() },
        { QStringLiteral("incomplete_reason"), incompleteReason },
        { QStringLiteral("records"), items }
    };
}

namespace
{

class EvidenceProcessor : public PDFPageContentProcessor
{
public:
    EvidenceProcessor(const PDFPage* page,
                      const PDFDocument* document,
                      const PDFFontCache* fontCache,
                      const PDFCMS* cms,
                      const PDFOptionalContentActivity* optionalContent,
                      const PDFMeshQualitySettings& meshQuality,
                      PDFProcessingBudget* budget,
                      PDFEvidenceGraph* graph,
                      PDFEvidenceDomains domains,
                      int pageNumber) :
        PDFPageContentProcessor(page, document, fontCache, cms, optionalContent, QTransform(), meshQuality, budget),
        m_graph(graph),
        m_domains(domains),
        m_pageNumber(pageNumber)
    {
    }

protected:
    bool isContentKindSuppressed(ContentKind kind) const override
    {
        switch (kind)
        {
            case ContentKind::Images:
            case ContentKind::Tiling:
            case ContentKind::Forms:
            case ContentKind::Shapes:
            case ContentKind::Text:
                return false;
            default:
                return true;
        }
    }

    bool performOriginalImagePainting(const PDFImage& image,
                                      const PDFStream* stream,
                                      PDFObjectReference reference) override
    {
        Q_UNUSED(stream);
        if (!m_domains.testFlag(PDFEvidenceDomain::Images) || isContentSuppressed())
        {
            return true;
        }

        const QTransform ctm = getGraphicState()->getCurrentTransformationMatrix();
        const auto axisLength = [](qreal x, qreal y)
        {
            return std::hypot(static_cast<double>(x), static_cast<double>(y)) * PDF_POINT_TO_INCH;
        };
        const double widthInches = axisLength(ctm.m11(), ctm.m12());
        const double heightInches = axisLength(ctm.m21(), ctm.m22());
        if (widthInches <= std::numeric_limits<double>::epsilon() ||
            heightInches <= std::numeric_limits<double>::epsilon())
        {
            return true;
        }

        PDFEvidenceRecord record;
        record.producer = m_graph->producer;
        record.producerVersion = m_graph->producerVersion;
        record.artifact = m_graph->artifact;
        record.revision = m_graph->revision;
        record.domain = PDFEvidenceDomain::Images;
        record.page = m_pageNumber;
        record.objectId = reference.isValid() ? QString::number(reference.objectNumber) : QString();
        record.target = QStringLiteral("image-effective-dpi");
        record.observedValue = qMin(qreal(image.getImageData().getWidth()) / widthInches,
                                    qreal(image.getImageData().getHeight()) / heightInches);
        record.units = QStringLiteral("dpi");
        record.geometry = ctm.mapRect(QRectF(0, 0, 1, 1));
        record.coverageMethod = QStringLiteral("content-stream");
        record.fidelity = QStringLiteral("exact");
        record.id = QStringLiteral("img:%1:%2").arg(m_pageNumber).arg(record.objectId.isEmpty() ? QStringLiteral("anon") : record.objectId);
        m_graph->records.append(record);
        return true;
    }

    void performBeforePathPainting(const QPainterPath& path,
                                   bool stroke,
                                   bool fill,
                                   bool text,
                                   Qt::FillRule fillRule) override
    {
        Q_UNUSED(fill);
        Q_UNUSED(text);
        Q_UNUSED(fillRule);
        if (isContentSuppressed() || path.isEmpty())
        {
            return;
        }

        const PDFPageContentProcessorState* state = getGraphicState();
        if (stroke && m_domains.testFlag(PDFEvidenceDomain::Strokes))
        {
            PDFEvidenceRecord record;
            record.producer = m_graph->producer;
            record.producerVersion = m_graph->producerVersion;
            record.artifact = m_graph->artifact;
            record.revision = m_graph->revision;
            record.domain = PDFEvidenceDomain::Strokes;
            record.page = m_pageNumber;
            record.target = QStringLiteral("stroke-width");
            record.observedValue = state->getLineWidth();
            record.units = QStringLiteral("pt");
            record.geometry = getCurrentWorldMatrix().map(path).boundingRect();
            record.coverageMethod = QStringLiteral("content-stream");
            record.fidelity = QStringLiteral("exact");
            record.id = QStringLiteral("stroke:%1:%2").arg(m_pageNumber).arg(m_graph->records.size());
            m_graph->records.append(record);
        }

        if (m_domains.testFlag(PDFEvidenceDomain::OverprintTransparency))
        {
            const PDFOverprintMode overprint = state->getOverprintMode();
            if (overprint.overprintStroking || overprint.overprintFilling)
            {
                PDFEvidenceRecord record;
                record.producer = m_graph->producer;
                record.producerVersion = m_graph->producerVersion;
                record.artifact = m_graph->artifact;
                record.revision = m_graph->revision;
                record.domain = PDFEvidenceDomain::OverprintTransparency;
                record.page = m_pageNumber;
                record.target = QStringLiteral("overprint");
                record.observedValue = overprint.overprintMode;
                record.units = QStringLiteral("opm");
                record.coverageMethod = QStringLiteral("content-stream");
                record.fidelity = QStringLiteral("exact");
                record.id = QStringLiteral("overprint:%1:%2").arg(m_pageNumber).arg(m_graph->records.size());
                m_graph->records.append(record);
            }
        }
    }

private:
    PDFEvidenceGraph* m_graph = nullptr;
    PDFEvidenceDomains m_domains;
    int m_pageNumber = 1;
};

void collectFonts(PDFDocument* document, PDFEvidenceGraph* graph)
{
    const PDFCatalog* catalog = document->getCatalog();
    for (PDFInteger pageIndex = 0; pageIndex < catalog->getPageCount(); ++pageIndex)
    {
        const PDFPage* page = catalog->getPage(pageIndex);
        if (!page)
        {
            continue;
        }
        const PDFObject resourcesObject = document->getObject(page->getResources());
        if (!resourcesObject.isDictionary())
        {
            continue;
        }
        const PDFObject fontsObject = document->getObject(resourcesObject.getDictionary()->get("Font"));
        if (!fontsObject.isDictionary())
        {
            continue;
        }
        const PDFDictionary* fonts = fontsObject.getDictionary();
        for (size_t i = 0; i < fonts->getCount(); ++i)
        {
            PDFEvidenceRecord record;
            record.producer = graph->producer;
            record.producerVersion = graph->producerVersion;
            record.artifact = graph->artifact;
            record.revision = graph->revision;
            record.domain = PDFEvidenceDomain::Fonts;
            record.page = int(pageIndex + 1);
            record.objectId = QString::fromLatin1(fonts->getKey(i).getString());
            record.target = QStringLiteral("font-resource");
            record.coverageMethod = QStringLiteral("resource-dictionary");
            record.fidelity = QStringLiteral("catalog");
            record.id = QStringLiteral("font:%1:%2").arg(record.page).arg(record.objectId);
            graph->records.append(record);
        }
    }
}

void collectColorants(PDFDocumentSession* session, PDFEvidenceGraph* graph)
{
    PDFColorInventory inventory(session);
    const PDFColorInventoryResult result = inventory.inspect(PDFColorInventorySettings());
    int index = 0;
    const auto appendInk = [&](const PDFColorInventoryInk& ink)
    {
        PDFEvidenceRecord record;
        record.producer = graph->producer;
        record.producerVersion = graph->producerVersion;
        record.artifact = graph->artifact;
        record.revision = graph->revision;
        record.domain = PDFEvidenceDomain::Colorants;
        record.target = ink.name;
        record.coverageMethod = QStringLiteral("color-inventory");
        record.fidelity = QStringLiteral("catalog");
        record.id = QStringLiteral("colorant:%1").arg(index++);
        graph->records.append(record);
    };
    for (const PDFColorInventoryInk& ink : result.spotColors)
    {
        appendInk(ink);
    }
    for (const PDFColorInventoryInk& ink : result.separations)
    {
        appendInk(ink);
    }
}

}   // namespace

PDFEvidenceGraph PDFEvidenceCollector::collect(PDFDocumentSession* session, PDFEvidenceDomains domains)
{
    if (domains == PDFEvidenceDomains())
    {
        domains = pdfEvidenceAllDomains();
    }
    PDFEvidenceGraph graph;
    graph.producerVersion = QString::fromLatin1(PDF_LIBRARY_VERSION);
    if (!session || !session->getDocument())
    {
        graph.complete = false;
        graph.incompleteReason = QStringLiteral("missing-document");
        return graph;
    }

    PDFDocument* document = session->getDocument();
    graph.revision = session->getRevision();
    try
    {
        if (domains.testFlag(PDFEvidenceDomain::Fonts))
        {
            collectFonts(document, &graph);
        }
        if (domains.testFlag(PDFEvidenceDomain::Colorants))
        {
            collectColorants(session, &graph);
        }

        if (domains.testFlag(PDFEvidenceDomain::Images) || domains.testFlag(PDFEvidenceDomain::Strokes) || domains.testFlag(PDFEvidenceDomain::OverprintTransparency))
        {
            PDFOptionalContentActivity ocActivity(document, OCUsage::Export, nullptr);
            PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
            fontCache.setDocument(PDFModifiedDocument(document, &ocActivity));
            fontCache.setCacheShrinkEnabled(nullptr, false);
            PDFCMSManager cmsManager(nullptr);
            cmsManager.setDocument(document);
            PDFCMSPointer cms = cmsManager.getCurrentCMS();
            PDFMeshQualitySettings meshQuality;
            const PDFCatalog* catalog = document->getCatalog();
            for (PDFInteger pageIndex = 0; pageIndex < catalog->getPageCount(); ++pageIndex)
            {
                const PDFPage* page = catalog->getPage(pageIndex);
                if (!page)
                {
                    continue;
                }
                EvidenceProcessor processor(page, document, &fontCache, cms.get(), &ocActivity, meshQuality,
                                            session->getProcessingBudget(), &graph, domains, int(pageIndex + 1));
                processor.processContents();
            }
        }
    }
    catch (const PDFBudgetExceededException& exception)
    {
        graph.complete = false;
        graph.incompleteReason = QString::fromLatin1(getPDFBudgetKindName(exception.getDetail().kind));
        if (!graph.records.isEmpty())
        {
            graph.records.last().budgetContext = exception.getDetail().context;
        }
    }
    catch (const PDFException& exception)
    {
        graph.complete = false;
        graph.incompleteReason = exception.getMessage();
    }

    return graph;
}

}   // namespace pdf
