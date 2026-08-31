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

#include "pdfcontourbleedfixup.h"

#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include "pdfoptionalcontent.h"
#include "pdfpainter.h"

#include <QPainter>
#include <QJsonArray>
#include <QLineF>
#include <QPolygonF>
#include <QtMath>

#include <cmath>
#include <limits>

namespace pdf
{

namespace
{

QPointF nearestPointOnSegment(const QPointF& point, const QPointF& first, const QPointF& second)
{
    const QLineF line(first, second);
    if (line.length() <= 0.0)
    {
        return first;
    }
    const QPointF delta = second - first;
    const double denominator = QPointF::dotProduct(delta, delta);
    const double fraction = qBound(0.0, QPointF::dotProduct(point - first, delta) / denominator, 1.0);
    return first + delta * fraction;
}

QPointF nearestPointOnContour(const QPainterPath& contour, const QPointF& point)
{
    QPointF nearest;
    double nearestDistance = std::numeric_limits<double>::max();
    for (const QPolygonF& polygon : contour.toSubpathPolygons(QTransform()))
    {
        if (polygon.size() < 2)
        {
            continue;
        }
        for (int index = 0; index < polygon.size(); ++index)
        {
            const QPointF candidate = nearestPointOnSegment(point, polygon.at(index), polygon.at((index + 1) % polygon.size()));
            const double distance = QLineF(point, candidate).length();
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearest = candidate;
            }
        }
    }
    return nearest;
}

QImage buildContourRingImage(const QImage& source,
                             const QTransform& pageToDevice,
                             const QPainterPath& contour,
                             const PDFContourBleedPlan& plan)
{
    QImage fill(source.size(), QImage::Format_ARGB32_Premultiplied);
    fill.fill(Qt::transparent);
    bool invertible = false;
    const QTransform deviceToPage = pageToDevice.inverted(&invertible);
    if (!invertible)
    {
        return {};
    }

    const QRect pixelBounds = pageToDevice.mapRect(plan.bleedBounds).toAlignedRect().intersected(fill.rect());
    for (int y = pixelBounds.top(); y <= pixelBounds.bottom(); ++y)
    {
        for (int x = pixelBounds.left(); x <= pixelBounds.right(); ++x)
        {
            const QPointF pagePoint = deviceToPage.map(QPointF(x + 0.5, y + 0.5));
            if (!plan.annularRing.contains(pagePoint))
            {
                continue;
            }
            const QPointF sourcePoint = nearestPointOnContour(contour, pagePoint);
            const QPointF sourcePixel = pageToDevice.map(sourcePoint);
            const int sourceX = qBound(0, qRound(sourcePixel.x()), source.width() - 1);
            const int sourceY = qBound(0, qRound(sourcePixel.y()), source.height() - 1);
            fill.setPixelColor(x, y, source.pixelColor(sourceX, sourceY));
        }
    }
    return fill;
}

QJsonArray diagnosticsToJson(const QVector<PDFProductionDiagnostic>& diagnostics)
{
    QJsonArray values;
    for (const PDFProductionDiagnostic& diagnostic : diagnostics)
    {
        values.append(diagnostic.toJson());
    }
    return values;
}

} // namespace

QJsonObject PDFContourBleedFixupReport::toJson() const
{
    QJsonArray pagesJson;
    for (const PDFContourBleedFixupPageReport& page : pages)
    {
        pagesJson.append(QJsonObject{
            { QStringLiteral("page"), int(page.pageIndex + 1) },
            { QStringLiteral("contourId"), page.contourId },
            { QStringLiteral("planned"), page.planned },
            { QStringLiteral("applied"), page.applied },
            { QStringLiteral("warnings"), QJsonArray::fromStringList(page.warnings) },
            { QStringLiteral("diagnostics"), diagnosticsToJson(page.diagnostics) }
        });
    }
    return QJsonObject{{ QStringLiteral("schema"), QStringLiteral("loop-contour-bleed-report/1") },
                        { QStringLiteral("pages"), pagesJson }};
}

PDFOperationResult PDFContourBleedFixup::apply(PDFDocument* document,
                                               const PDFProductionGeometryModel& geometry,
                                               const PDFContourBleedFixupSettings& settings,
                                               PDFContourBleedFixupReport* report,
                                               PDFModifiedDocument::ModificationFlags* modificationFlags)
{
    if (modificationFlags)
    {
        *modificationFlags = {};
    }
    if (report)
    {
        report->pages.clear();
    }
    if (!document)
    {
        return PDFTranslationContext::tr("Invalid document.");
    }
    if (settings.dpi <= 0 || settings.amountPt <= 0.0 || !std::isfinite(settings.amountPt) || settings.maxSegments <= 0)
    {
        return PDFTranslationContext::tr("Contour bleed amount and DPI must be positive.");
    }

    const PDFProductionValidationReport validation = validateProductionGeometry(geometry);
    if (!validation.valid)
    {
        return PDFTranslationContext::tr("Production geometry validation failed before contour bleed.");
    }

    PDFOptionalContentActivity optionalContentActivity(document, OCUsage::Export, nullptr);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument modifiedDocument(document, &optionalContentActivity);
    fontCache.setDocument(modifiedDocument);
    fontCache.setCacheShrinkEnabled(nullptr, false);
    PDFMeshQualitySettings meshQualitySettings;
    PDFRenderer::Features features = settings.renderFeatures;
    features.setFlag(PDFRenderer::ClipToCropBox, false);
    features.setFlag(PDFRenderer::DisplayAnnotations, false);
    PDFRenderer renderer(document, &fontCache, cms.get(), &optionalContentActivity, features, meshQualitySettings);
    PDFRasterizer rasterizer(nullptr);
    rasterizer.reset(RendererEngine::QPainter);
    PDFDocumentModifier modifier(document);
    PDFDocumentBuilder* builder = modifier.getBuilder();
    PDFModifiedDocument::ModificationFlags flags = PDFModifiedDocument::ModificationFlags(PDFModifiedDocument::Reset | PDFModifiedDocument::PreserveUndoRedo);
    bool changed = false;
    QVector<PDFContourBleedFixupPageReport> pageReports;

    for (const PDFProductionContour& contour : geometry.contours)
    {
        if (contour.pageIndex < 0 || contour.pageIndex >= document->getCatalog()->getPageCount())
        {
            return PDFTranslationContext::tr("Contour %1 references an invalid page.").arg(contour.id);
        }
        const PDFPage* page = document->getCatalog()->getPage(contour.pageIndex);
        const PDFContourBleedPlan plan = planContourBleed(contour, { settings.amountPt, settings.flatteningTolerancePt, settings.maxSegments });
        PDFContourBleedFixupPageReport pageReport;
        pageReport.pageIndex = contour.pageIndex;
        pageReport.contourId = contour.id;
        pageReport.sourceBounds = plan.sourceBounds;
        pageReport.bleedBounds = plan.bleedBounds;
        pageReport.planned = plan.valid;
        pageReport.diagnostics = plan.diagnostics;
        if (!plan.valid)
        {
            return PDFTranslationContext::tr("Contour bleed planning failed for %1.").arg(contour.id);
        }
        if (!settings.analyzeOnly)
        {
            const QSizeF mediaSize = page->getRotatedMediaBox().size();
            const double widthPxReal = std::ceil(mediaSize.width() * PDF_POINT_TO_INCH * settings.dpi);
            const double heightPxReal = std::ceil(mediaSize.height() * PDF_POINT_TO_INCH * settings.dpi);
            const double pixelCount = qMax(1.0, widthPxReal) * qMax(1.0, heightPxReal);
            if (!std::isfinite(widthPxReal) || !std::isfinite(heightPxReal) ||
                (settings.maxRasterPixels > 0 && pixelCount > double(settings.maxRasterPixels)))
            {
                return PDFTranslationContext::tr("Contour bleed raster exceeds the configured pixel budget on page %1.").arg(contour.pageIndex + 1);
            }
            const QSize imageSize(qMax(1, int(widthPxReal)), qMax(1, int(heightPxReal)));
            PDFPrecompiledPage compiledPage;
            renderer.compile(&compiledPage, size_t(contour.pageIndex));
            const QTransform pageToDevice = PDFRenderer::createPagePointToDevicePointMatrix(page, QRect(QPoint(0, 0), imageSize));
            const QImage source = rasterizer.render(contour.pageIndex, page, &compiledPage, imageSize, features, nullptr, cms.get(), PageRotation::None);
            if (source.isNull())
            {
                return PDFTranslationContext::tr("Failed to rasterize contour bleed source on page %1.").arg(contour.pageIndex + 1);
            }
            const QImage ring = buildContourRingImage(source, pageToDevice, contour.path, plan);
            if (ring.isNull())
            {
                return PDFTranslationContext::tr("Failed to build contour bleed ring on page %1.").arg(contour.pageIndex + 1);
            }
            PDFPageContentStreamBuilder contentStreamBuilder(builder,
                                                               PDFContentStreamBuilder::CoordinateSystem::PDF,
                                                               PDFPageContentStreamBuilder::Mode::PlaceBefore);
            QPainter* painter = contentStreamBuilder.begin(page->getPageReference());
            if (!painter)
            {
                return PDFTranslationContext::tr("Failed to open contour bleed content stream on page %1.").arg(contour.pageIndex + 1);
            }
            painter->drawImage(page->getMediaBox(), ring);
            contentStreamBuilder.end(painter);
            pageReport.applied = true;
            changed = true;
        }
        pageReports.append(std::move(pageReport));
    }

    if (!settings.analyzeOnly && changed)
    {
        modifier.markReset();
        modifier.markPageContentsChanged();
        flags |= PDFModifiedDocument::PageContents;
        if (!modifier.finalize())
        {
            return PDFTranslationContext::tr("Failed to finalize contour bleed modifications.");
        }
        *document = *modifier.getDocument();
    }
    if (report)
    {
        report->pages = std::move(pageReports);
    }
    if (modificationFlags)
    {
        *modificationFlags = flags;
    }
    return true;
}

} // namespace pdf
