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

#include "pdfbleedfixup.h"

#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include "pdfoptionalcontent.h"
#include "pdfpainter.h"
#include "pdfprocessingbudget.h"

#include <QColor>
#include <QPainter>
#include <QtMath>

#include <cmath>
#include <limits>
#include <set>

namespace pdf
{

namespace
{

QString formatPDFNumber(PDFReal value)
{
    QString text = QString::number(value, 'f', 6);
    while (text.endsWith(QLatin1Char('0')))
    {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.')))
    {
        text.chop(1);
    }
    if (text.isEmpty() || text == QStringLiteral("-0"))
    {
        text = QStringLiteral("0");
    }
    return text;
}

PDFObjectReference createContentStream(PDFDocumentBuilder* builder, const QByteArray& content)
{
    PDFDictionary dictionary;
    dictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(content.size()));
    PDFObject streamObject = PDFObject::createStream(std::make_shared<PDFStream>(std::move(dictionary), QByteArray(content)));
    return builder->addObject(std::move(streamObject));
}

void appendContentReference(PDFDocumentBuilder* builder,
                            std::vector<PDFObjectReference>& contentReferences,
                            PDFObject contentObject)
{
    const PDFObject dereferenced = builder->getObject(contentObject);
    if (dereferenced.isNull())
    {
        return;
    }

    if (dereferenced.isStream())
    {
        if (contentObject.isReference())
        {
            contentReferences.push_back(contentObject.getReference());
        }
        else
        {
            contentReferences.push_back(builder->addObject(dereferenced));
        }
        return;
    }

    if (dereferenced.isArray())
    {
        for (const PDFObject& object : *dereferenced.getArray())
        {
            if (object.isReference())
            {
                contentReferences.push_back(object.getReference());
            }
            else if (object.isStream())
            {
                contentReferences.push_back(builder->addObject(object));
            }
        }
    }
}

void prependContentTranslate(PDFDocumentBuilder* builder,
                             PDFObjectReference pageReference,
                             PDFReal tx,
                             PDFReal ty)
{
    const PDFObjectStorage* storage = builder->getStorage();
    const PDFDictionary* pageDictionary = storage->getDictionaryFromObject(storage->getObjectByReference(pageReference));
    if (!pageDictionary)
    {
        return;
    }

    std::vector<PDFObjectReference> contentReferences;
    appendContentReference(builder, contentReferences, pageDictionary->get("Contents"));
    if (contentReferences.empty())
    {
        return;
    }

    QByteArray prefix;
    prefix.append("q 1 0 0 1 ");
    prefix.append(formatPDFNumber(tx).toLatin1());
    prefix.append(' ');
    prefix.append(formatPDFNumber(ty).toLatin1());
    prefix.append(" cm\n");

    contentReferences.insert(contentReferences.begin(), createContentStream(builder, prefix));
    contentReferences.push_back(createContentStream(builder, QByteArray("Q\n")));

    PDFObjectFactory pageUpdateFactory;
    pageUpdateFactory.beginDictionary();
    pageUpdateFactory.beginDictionaryItem("Contents");
    pageUpdateFactory << contentReferences;
    pageUpdateFactory.endDictionaryItem();
    pageUpdateFactory.endDictionary();
    builder->mergeTo(pageReference, pageUpdateFactory.takeObject());
}

std::vector<PDFInteger> selectPageIndices(const PDFDocument* document,
                                          const QString& pageRange,
                                          QString* errorMessage)
{
    std::vector<PDFInteger> result;
    if (!document)
    {
        if (errorMessage)
        {
            *errorMessage = PDFTranslationContext::tr("Invalid document.");
        }
        return result;
    }

    const PDFCatalog* catalog = document->getCatalog();
    const PDFInteger pageCount = catalog->getPageCount();
    if (pageCount <= 0)
    {
        return result;
    }

    std::set<PDFInteger> rangeSelection;
    const QString rangeText = pageRange.simplified();
    if (!rangeText.isEmpty())
    {
        QString parseError;
        const PDFClosedIntervalSet closedIntervalSet = PDFClosedIntervalSet::parse(1, pageCount, rangeText, &parseError);
        if (!parseError.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = parseError;
            }
            return {};
        }

        for (const PDFInteger pageNumber : closedIntervalSet.unfold())
        {
            rangeSelection.insert(pageNumber);
        }
    }

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        const PDFInteger pageNumber = pageIndex + 1;
        if (!rangeSelection.empty() && !rangeSelection.count(pageNumber))
        {
            continue;
        }
        result.push_back(pageIndex);
    }

    return result;
}

QRect mapPageRectToImage(const QRectF& pageRect, const QTransform& pageToDevice, const QSize& imageSize)
{
    const QRectF mapped = pageToDevice.mapRect(pageRect).normalized();
    QRect pixelRect = mapped.toAlignedRect();
    return pixelRect.intersected(QRect(QPoint(0, 0), imageSize));
}

QString sideName(PDFBleedFixupSide side)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left:
            return QStringLiteral("left");
        case PDFBleedFixupSide::Right:
            return QStringLiteral("right");
        case PDFBleedFixupSide::Top:
            return QStringLiteral("top");
        case PDFBleedFixupSide::Bottom:
            return QStringLiteral("bottom");
    }
    return QStringLiteral("unknown");
}

}   // namespace

namespace PDFBleedFixupMath
{

PDFBleedRasterPlan planRaster(const QSizeF& mediaSize,
                              int dpi,
                              qint64 maxRasterPixels,
                              bool analyzeOnly,
                              bool hasEligibleSides)
{
    PDFBleedRasterPlan plan;
    plan.rasterRequired = hasEligibleSides && !analyzeOnly;
    if (!plan.rasterRequired)
    {
        return plan;
    }

    if (dpi <= 0 || !std::isfinite(mediaSize.width()) || !std::isfinite(mediaSize.height()) || !(mediaSize.width() > 0.0) || !(mediaSize.height() > 0.0))
    {
        plan.withinBudget = false;
        plan.errorMessage = PDFTranslationContext::tr("Page has an invalid media box size for rasterization.");
        return plan;
    }

    const double widthPxReal = std::ceil(mediaSize.width() * PDF_POINT_TO_INCH * dpi);
    const double heightPxReal = std::ceil(mediaSize.height() * PDF_POINT_TO_INCH * dpi);
    if (!std::isfinite(widthPxReal) || !std::isfinite(heightPxReal) || widthPxReal < 1.0 || heightPxReal < 1.0)
    {
        plan.withinBudget = false;
        plan.errorMessage = PDFTranslationContext::tr("Page has invalid raster dimensions.");
        return plan;
    }

    plan.pixelCount = widthPxReal * heightPxReal;
    if (!std::isfinite(plan.pixelCount))
    {
        plan.withinBudget = false;
        plan.errorMessage = PDFTranslationContext::tr("Page raster dimensions exceed the supported range.");
        return plan;
    }

    constexpr double maxImageDimension = double((std::numeric_limits<int>::max)());
    if (widthPxReal > maxImageDimension || heightPxReal > maxImageDimension)
    {
        plan.withinBudget = false;
        plan.errorMessage = PDFTranslationContext::tr("Page raster dimensions exceed the supported image size.");
        return plan;
    }

    if (maxRasterPixels > 0 && plan.pixelCount > double(maxRasterPixels))
    {
        plan.withinBudget = false;
        plan.errorMessage = PDFTranslationContext::tr(
                                "Page requires a %1 x %2 px raster at %3 DPI for edge sampling, which exceeds the limit of %4 megapixels. Lower --dpi or raise the raster limit.")
                                .arg(qint64(widthPxReal))
                                .arg(qint64(heightPxReal))
                                .arg(dpi)
                                .arg(maxRasterPixels / 1000000);
        return plan;
    }

    plan.imageSize = QSize(qMax(1, int(widthPxReal)), qMax(1, int(heightPxReal)));
    return plan;
}

QRectF referenceRect(const PDFPage* page, PDFBleedFixupSettings::ReferenceBox referenceBox)
{
    if (!page)
    {
        return QRectF();
    }

    switch (referenceBox)
    {
        case PDFBleedFixupSettings::ReferenceBox::MediaBox:
            return page->getMediaBox();
        case PDFBleedFixupSettings::ReferenceBox::CropBox:
            return page->getCropBox();
        case PDFBleedFixupSettings::ReferenceBox::TrimBox:
        default:
            return page->getTrimBox().isValid() ? page->getTrimBox() : page->getCropBox();
    }
}

QRectF targetBleedRect(const QRectF& reference, const QMarginsF& bleedMM)
{
    const PDFReal left = bleedMM.left() * PDF_MM_TO_POINT;
    const PDFReal right = bleedMM.right() * PDF_MM_TO_POINT;
    const PDFReal top = bleedMM.top() * PDF_MM_TO_POINT;
    const PDFReal bottom = bleedMM.bottom() * PDF_MM_TO_POINT;
    return QRectF(reference.left() - left,
                  reference.top() - bottom,
                  reference.width() + left + right,
                  reference.height() + top + bottom)
        .normalized();
}

QRectF expandBoxTo(const QRectF& box, const QRectF& target)
{
    if (!target.isValid())
    {
        return box;
    }
    if (!box.isValid())
    {
        return target;
    }
    return box.united(target);
}

PDFReal sideBleedMM(const QMarginsF& bleedMM, PDFBleedFixupSide side)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left:
            return bleedMM.left();
        case PDFBleedFixupSide::Right:
            return bleedMM.right();
        case PDFBleedFixupSide::Top:
            return bleedMM.top();
        case PDFBleedFixupSide::Bottom:
            return bleedMM.bottom();
    }
    return 0.0;
}

bool sideAlreadyBleeding(const QRectF& reference,
                         const QRectF& bleedBox,
                         PDFBleedFixupSide side,
                         PDFReal requiredBleedPt)
{
    if (!(requiredBleedPt > 0.0) || !reference.isValid() || !bleedBox.isValid())
    {
        return false;
    }

    constexpr PDFReal epsilon = 0.5;
    switch (side)
    {
        case PDFBleedFixupSide::Left:
            return (reference.left() - bleedBox.left()) + epsilon >= requiredBleedPt;
        case PDFBleedFixupSide::Right:
            return (bleedBox.right() - reference.right()) + epsilon >= requiredBleedPt;
        case PDFBleedFixupSide::Bottom:
            return (reference.top() - bleedBox.top()) + epsilon >= requiredBleedPt;
        case PDFBleedFixupSide::Top:
            return (bleedBox.bottom() - reference.bottom()) + epsilon >= requiredBleedPt;
    }
    return false;
}

int stripWidthPx(PDFBleedFixupMode mode, int bleedDepthPx, int samplePixels)
{
    if (mode == PDFBleedFixupMode::Mirror)
    {
        return qMax(1, bleedDepthPx);
    }
    return qMax(1, samplePixels);
}

QRectF edgeStripSourceRect(const QRectF& reference, PDFBleedFixupSide side, PDFReal depthPt)
{
    if (!(depthPt > 0.0) || !reference.isValid())
    {
        return QRectF();
    }

    switch (side)
    {
        case PDFBleedFixupSide::Left:
            return QRectF(reference.left(), reference.top(), depthPt, reference.height());
        case PDFBleedFixupSide::Right:
            return QRectF(reference.right() - depthPt, reference.top(), depthPt, reference.height());
        case PDFBleedFixupSide::Bottom:
            return QRectF(reference.left(), reference.top(), reference.width(), depthPt);
        case PDFBleedFixupSide::Top:
            return QRectF(reference.left(), reference.bottom() - depthPt, reference.width(), depthPt);
    }
    return QRectF();
}

QRectF edgeStripDestRect(const QRectF& reference, PDFBleedFixupSide side, PDFReal depthPt)
{
    if (!(depthPt > 0.0) || !reference.isValid())
    {
        return QRectF();
    }

    switch (side)
    {
        case PDFBleedFixupSide::Left:
            return QRectF(reference.left() - depthPt, reference.top(), depthPt, reference.height());
        case PDFBleedFixupSide::Right:
            return QRectF(reference.right(), reference.top(), depthPt, reference.height());
        case PDFBleedFixupSide::Bottom:
            return QRectF(reference.left(), reference.top() - depthPt, reference.width(), depthPt);
        case PDFBleedFixupSide::Top:
            return QRectF(reference.left(), reference.bottom(), reference.width(), depthPt);
    }
    return QRectF();
}

static bool isHorizontalBleedSide(PDFBleedFixupSide side)
{
    return side == PDFBleedFixupSide::Left || side == PDFBleedFixupSide::Right;
}

static bool isVerticalBleedSide(PDFBleedFixupSide side)
{
    return side == PDFBleedFixupSide::Top || side == PDFBleedFixupSide::Bottom;
}

QRectF cornerStripSourceRect(const QRectF& reference,
                             PDFBleedFixupSide horizontal,
                             PDFBleedFixupSide vertical,
                             PDFReal horizontalDepthPt,
                             PDFReal verticalDepthPt)
{
    if (!(horizontalDepthPt > 0.0) || !(verticalDepthPt > 0.0) || !reference.isValid() || !isHorizontalBleedSide(horizontal) || !isVerticalBleedSide(vertical))
    {
        return QRectF();
    }

    const qreal x = (horizontal == PDFBleedFixupSide::Left)
                        ? reference.left()
                        : reference.right() - horizontalDepthPt;
    const qreal y = (vertical == PDFBleedFixupSide::Bottom)
                        ? reference.top()
                        : reference.bottom() - verticalDepthPt;
    return QRectF(x, y, horizontalDepthPt, verticalDepthPt);
}

QRectF cornerStripDestRect(const QRectF& reference,
                           PDFBleedFixupSide horizontal,
                           PDFBleedFixupSide vertical,
                           PDFReal horizontalDepthPt,
                           PDFReal verticalDepthPt)
{
    if (!(horizontalDepthPt > 0.0) || !(verticalDepthPt > 0.0) || !reference.isValid() || !isHorizontalBleedSide(horizontal) || !isVerticalBleedSide(vertical))
    {
        return QRectF();
    }

    const qreal x = (horizontal == PDFBleedFixupSide::Left)
                        ? reference.left() - horizontalDepthPt
                        : reference.right();
    const qreal y = (vertical == PDFBleedFixupSide::Bottom)
                        ? reference.top() - verticalDepthPt
                        : reference.bottom();
    return QRectF(x, y, horizontalDepthPt, verticalDepthPt);
}

QImage buildEdgeFillImage(const QImage& pageImage,
                          const QRect& sourcePx,
                          PDFBleedFixupSide side,
                          PDFBleedFixupMode mode,
                          int bleedDepthPx)
{
    if (pageImage.isNull() || !sourcePx.isValid() || bleedDepthPx <= 0)
    {
        return QImage();
    }

    const QRect clipped = sourcePx.intersected(pageImage.rect());
    if (!clipped.isValid() || clipped.isEmpty())
    {
        return QImage();
    }

    QImage strip = pageImage.copy(clipped);
    if (strip.isNull() || strip.width() <= 0 || strip.height() <= 0)
    {
        return QImage();
    }

    switch (mode)
    {
        case PDFBleedFixupMode::Mirror:
        {
            const bool horizontal = (side == PDFBleedFixupSide::Left || side == PDFBleedFixupSide::Right);
            return strip.flipped(horizontal ? Qt::Horizontal : Qt::Vertical);
        }
        case PDFBleedFixupMode::PixelRepeat:
        {
            if (side == PDFBleedFixupSide::Left || side == PDFBleedFixupSide::Right)
            {
                QImage out(bleedDepthPx, strip.height(), QImage::Format_ARGB32_Premultiplied);
                const int srcX = (side == PDFBleedFixupSide::Left) ? 0 : (strip.width() - 1);
                for (int x = 0; x < bleedDepthPx; ++x)
                {
                    for (int y = 0; y < strip.height(); ++y)
                    {
                        out.setPixel(x, y, strip.pixel(srcX, y));
                    }
                }
                return out;
            }

            QImage out(strip.width(), bleedDepthPx, QImage::Format_ARGB32_Premultiplied);
            // After page->device Y flip, page top maps near image y=0.
            const int edgeY = (side == PDFBleedFixupSide::Top) ? 0 : (strip.height() - 1);
            for (int y = 0; y < bleedDepthPx; ++y)
            {
                for (int x = 0; x < strip.width(); ++x)
                {
                    out.setPixel(x, y, strip.pixel(x, edgeY));
                }
            }
            return out;
        }
        case PDFBleedFixupMode::Stretch:
        {
            if (side == PDFBleedFixupSide::Left || side == PDFBleedFixupSide::Right)
            {
                return strip.scaled(bleedDepthPx, strip.height(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
            }
            return strip.scaled(strip.width(), bleedDepthPx, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        }
    }

    return QImage();
}

QImage buildCornerFillImage(const QImage& pageImage,
                            const QRect& sourcePx,
                            PDFBleedFixupSide horizontal,
                            PDFBleedFixupSide vertical,
                            PDFBleedFixupMode mode,
                            int destWidthPx,
                            int destHeightPx)
{
    if (pageImage.isNull() || !sourcePx.isValid() || destWidthPx <= 0 || destHeightPx <= 0 || !isHorizontalBleedSide(horizontal) || !isVerticalBleedSide(vertical))
    {
        return QImage();
    }

    const QRect clipped = sourcePx.intersected(pageImage.rect());
    if (!clipped.isValid() || clipped.isEmpty())
    {
        return QImage();
    }

    QImage strip = pageImage.copy(clipped);
    if (strip.isNull() || strip.width() <= 0 || strip.height() <= 0)
    {
        return QImage();
    }

    switch (mode)
    {
        case PDFBleedFixupMode::Mirror:
            return strip.flipped(Qt::Horizontal | Qt::Vertical);
        case PDFBleedFixupMode::PixelRepeat:
        {
            QImage out(destWidthPx, destHeightPx, QImage::Format_ARGB32_Premultiplied);
            const int srcX = (horizontal == PDFBleedFixupSide::Left) ? 0 : (strip.width() - 1);
            // After page->device Y flip, page top maps near image y=0.
            const int srcY = (vertical == PDFBleedFixupSide::Top) ? 0 : (strip.height() - 1);
            out.fill(QColor::fromRgba(strip.pixel(srcX, srcY)));
            return out;
        }
        case PDFBleedFixupMode::Stretch:
            return strip.scaled(destWidthPx, destHeightPx, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }

    return QImage();
}

}   // namespace PDFBleedFixupMath

PDFOperationResult PDFBleedFixup::apply(PDFDocument* document,
                                        const PDFBleedFixupSettings& settings,
                                        PDFBleedFixupReport* report,
                                        PDFModifiedDocument::ModificationFlags* modificationFlags)
{
    if (modificationFlags)
    {
        *modificationFlags = PDFModifiedDocument::ModificationFlags();
    }
    if (report)
    {
        report->pages.clear();
    }

    // Accumulated locally and only published into *report once the whole batch
    // has committed successfully: document mutations are all-or-nothing (rolled
    // back on any error return below), so a partially-filled *report must not
    // describe pages whose changes were never actually applied.
    QVector<PDFBleedFixupPageReport> pageReports;

    if (!document)
    {
        return PDFTranslationContext::tr("Invalid document.");
    }
    if (settings.dpi <= 0)
    {
        return PDFTranslationContext::tr("DPI must be positive.");
    }
    if (settings.samplePixels <= 0)
    {
        return PDFTranslationContext::tr("Sample pixel count must be positive.");
    }

    if (settings.bleedMM.left() < 0.0 || settings.bleedMM.top() < 0.0 || settings.bleedMM.right() < 0.0 || settings.bleedMM.bottom() < 0.0)
    {
        return PDFTranslationContext::tr("Bleed margins must be non-negative.");
    }

    QString pageSelectionError;
    const std::vector<PDFInteger> pageIndices = selectPageIndices(document, settings.pageRange, &pageSelectionError);
    if (!pageSelectionError.isEmpty())
    {
        return pageSelectionError;
    }
    if (pageIndices.empty())
    {
        return true;
    }

    PDFOptionalContentActivity optionalContentActivity(document, OCUsage::Export, nullptr);
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSPointer cms = cmsManager.getCurrentCMS();
    PDFFontCache fontCache(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument md(document, &optionalContentActivity);
    fontCache.setDocument(md);
    fontCache.setCacheShrinkEnabled(nullptr, false);

    PDFMeshQualitySettings meshQualitySettings;
    PDFRenderer::Features features = settings.renderFeatures;
    features.setFlag(PDFRenderer::ClipToCropBox, false);
    features.setFlag(PDFRenderer::DisplayAnnotations, false);

    PDFRenderer renderer(document, &fontCache, cms.get(), &optionalContentActivity, features, meshQualitySettings);
    PDFRasterizer rasterizer(nullptr);
    rasterizer.reset(RendererEngine::QPainter);

    // Issue #38 (bug-hunt finding F-04): settings.maxRasterPixels already caps
    // any single page's strip probe, but nothing charged the shared,
    // document-scoped pdf::PDFProcessingBudget that every other rendering and
    // decode path in this codebase reports through (docs/RESOURCE_BUDGETS.md).
    // A conservative budget here bounds the *cumulative* raster cost across
    // every page this call touches, not just the largest single page.
    PDFProcessingBudget rasterBudget;

    PDFDocumentModifier modifier(document);
    PDFDocumentBuilder* builder = modifier.getBuilder();
    Q_ASSERT(builder);

    PDFModifiedDocument::ModificationFlags flags = PDFModifiedDocument::ModificationFlags(PDFModifiedDocument::Reset | PDFModifiedDocument::PreserveUndoRedo);
    bool isPageContentChanged = false;

    const PDFBleedFixupSide allSides[4] = {
        PDFBleedFixupSide::Left,
        PDFBleedFixupSide::Bottom,
        PDFBleedFixupSide::Right,
        PDFBleedFixupSide::Top
    };

    for (const PDFInteger pageIndex : pageIndices)
    {
        const PDFPage* page = document->getCatalog()->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        PDFBleedFixupPageReport pageReport;
        pageReport.pageIndex = pageIndex;
        pageReport.originalMediaBox = page->getMediaBox();
        pageReport.originalCropBox = page->getCropBox();
        pageReport.originalBleedBox = page->getBleedBox();
        pageReport.originalTrimBox = page->getTrimBox();

        const QRectF reference = PDFBleedFixupMath::referenceRect(page, settings.referenceBox);
        if (!reference.isValid() || reference.isEmpty())
        {
            return PDFTranslationContext::tr("Reference box on page %1 is invalid.").arg(pageIndex + 1);
        }

        const QMarginsF effectiveBleedMM(
            isBleedFixupSideEnabled(settings.sides, PDFBleedFixupSide::Left) ? settings.bleedMM.left() : 0.0,
            isBleedFixupSideEnabled(settings.sides, PDFBleedFixupSide::Top) ? settings.bleedMM.top() : 0.0,
            isBleedFixupSideEnabled(settings.sides, PDFBleedFixupSide::Right) ? settings.bleedMM.right() : 0.0,
            isBleedFixupSideEnabled(settings.sides, PDFBleedFixupSide::Bottom) ? settings.bleedMM.bottom() : 0.0);
        const QRectF targetBleed = PDFBleedFixupMath::targetBleedRect(reference, effectiveBleedMM);
        if (!targetBleed.isValid() || targetBleed.isEmpty())
        {
            return PDFTranslationContext::tr("Target bleed box on page %1 is invalid.").arg(pageIndex + 1);
        }

        struct SideWork
        {
            PDFBleedFixupSide side = PDFBleedFixupSide::Left;
            PDFReal depthPt = 0.0;
        };
        std::vector<SideWork> sidesToApply;

        for (PDFBleedFixupSide side : allSides)
        {
            if (!isBleedFixupSideEnabled(settings.sides, side))
            {
                continue;
            }

            const PDFReal depthMM = PDFBleedFixupMath::sideBleedMM(settings.bleedMM, side);
            if (!(depthMM > 0.0))
            {
                continue;
            }

            pageReport.sidesRequested |= bleedFixupSideBit(side);

            const PDFReal depthPt = depthMM * PDF_MM_TO_POINT;
            if (!settings.force && settings.skipIfAlreadyBleeding &&
                PDFBleedFixupMath::sideAlreadyBleeding(reference, page->getBleedBox(), side, depthPt))
            {
                pageReport.skipReasons.append(PDFTranslationContext::tr("Skipped %1: BleedBox already sufficient.")
                                                  .arg(sideName(side)));
                continue;
            }

            pageReport.sidesEligible |= bleedFixupSideBit(side);
            sidesToApply.push_back(SideWork{ side, depthPt });
        }

        QRectF newBleed = page->getBleedBox();
        QRectF newCrop = page->getCropBox();
        QRectF newMedia = page->getMediaBox();
        QRectF newTrim = page->getTrimBox();

        if (settings.expandBleedBox)
        {
            newBleed = PDFBleedFixupMath::expandBoxTo(newBleed, targetBleed);
        }
        if (settings.expandCropBox)
        {
            newCrop = PDFBleedFixupMath::expandBoxTo(newCrop, newBleed);
        }
        if (settings.expandMediaBox)
        {
            newMedia = PDFBleedFixupMath::expandBoxTo(newMedia, newCrop);
        }
        if (settings.expandTrimBox)
        {
            newTrim = PDFBleedFixupMath::expandBoxTo(newTrim, targetBleed);
        }

        // Regardless of which individual expand flags were requested, keep the
        // box nesting PDF-conformant (MediaBox superset CropBox superset
        // BleedBox): an enclosing box left smaller than one that just grew
        // produces an invalid PDF and clips bleed artwork outside the
        // paintable/visible area. expandBoxTo only grows (union), so this is a
        // no-op when the boxes already nest correctly.
        newCrop = PDFBleedFixupMath::expandBoxTo(newCrop, newBleed);
        newMedia = PDFBleedFixupMath::expandBoxTo(newMedia, newCrop);

        const PDFObjectReference pageReference = page->getPageReference();
        const PDFReal translateX = -newMedia.left();
        const PDFReal translateY = -newMedia.top();
        // Default policy keeps TrimBox fixed; only re-anchor the page origin when trim
        // itself is being expanded and the enlarged media box would use negative coords.
        const bool reanchorOrigin = settings.expandTrimBox && (!qFuzzyIsNull(translateX) || !qFuzzyIsNull(translateY));
        const QRectF outputMedia = reanchorOrigin
                                       ? QRectF(0.0, 0.0, newMedia.width(), newMedia.height())
                                       : newMedia;

        auto mapOutputRect = [reanchorOrigin, translateX, translateY](const QRectF& rect) -> QRectF
        {
            if (!reanchorOrigin)
            {
                return rect;
            }
            return QRectF(rect.left() + translateX, rect.top() + translateY, rect.width(), rect.height());
        };

        QImage pageImage;
        QTransform pageToDevice;
        const PDFBleedFixupMath::PDFBleedRasterPlan rasterPlan = PDFBleedFixupMath::planRaster(
            page->getRotatedMediaBox().size(),
            settings.dpi,
            settings.maxRasterPixels,
            settings.analyzeOnly,
            !sidesToApply.empty());
        if (!rasterPlan.withinBudget)
        {
            return PDFTranslationContext::tr("Page %1: %2").arg(pageIndex + 1).arg(rasterPlan.errorMessage);
        }

        if (rasterPlan.rasterRequired)
        {
            const QSize imageSize = rasterPlan.imageSize;

            try
            {
                rasterBudget.chargeRenderPixels(static_cast<std::uint64_t>(rasterPlan.pixelCount),
                                                PDFTranslationContext::tr("bleed edge-sample raster"));
            }
            catch (const PDFException& exception)
            {
                return PDFTranslationContext::tr("Page %1: %2").arg(pageIndex + 1).arg(exception.getMessage());
            }

            PDFPrecompiledPage compiledPage;
            renderer.compile(&compiledPage, static_cast<size_t>(pageIndex));

            pageToDevice = PDFRenderer::createPagePointToDevicePointMatrix(page, QRect(QPoint(0, 0), imageSize));
            pageImage = rasterizer.render(pageIndex, page, &compiledPage, imageSize, features, nullptr, cms.get(), PageRotation::None);
            if (pageImage.isNull())
            {
                return PDFTranslationContext::tr("Failed to rasterize page %1.").arg(pageIndex + 1);
            }
        }

        // analyzeOnly is a dry run: report what the box/content changes would be
        // without touching the document (the builder must stay untouched so the
        // final modifier.finalize()/getDocument() commit below is skipped).
        if (!settings.analyzeOnly)
        {
            if (reanchorOrigin)
            {
                prependContentTranslate(builder, pageReference, translateX, translateY);
                isPageContentChanged = true;
            }

            // Also write MediaBox/CropBox when the nesting cascade above grew them
            // to stay conformant, even if their own expand flag was not set.
            if (settings.expandMediaBox || reanchorOrigin || newMedia != page->getMediaBox())
            {
                builder->setPageMediaBox(pageReference, outputMedia);
            }
            if (settings.expandCropBox || reanchorOrigin || newCrop != page->getCropBox())
            {
                builder->setPageCropBox(pageReference, mapOutputRect(newCrop));
            }
            if (settings.expandBleedBox || reanchorOrigin)
            {
                builder->setPageBleedBox(pageReference, mapOutputRect(newBleed));
            }
            if (settings.expandTrimBox)
            {
                builder->setPageTrimBox(pageReference, mapOutputRect(newTrim));
            }
            else
            {
                // Rewrite unchanged trim so outer-box expansion does not collapse it.
                builder->setPageTrimBox(pageReference, page->getTrimBox());
            }
        }

        pageReport.newMediaBox = outputMedia;
        pageReport.newCropBox = mapOutputRect(newCrop);
        pageReport.newBleedBox = mapOutputRect(newBleed);
        pageReport.newTrimBox = settings.expandTrimBox ? mapOutputRect(newTrim) : page->getTrimBox();

        if (settings.analyzeOnly)
        {
            // No artwork is painted, so report the sides the run would fill.
            for (const SideWork& work : sidesToApply)
            {
                pageReport.sidesApplied.append(work.side);
            }
        }
        else if (!sidesToApply.empty())
        {
            PDFPageContentStreamBuilder pageContentStreamBuilder(builder,
                                                                 PDFContentStreamBuilder::CoordinateSystem::PDF,
                                                                 PDFPageContentStreamBuilder::Mode::PlaceBefore);
            QPainter* painter = pageContentStreamBuilder.begin(pageReference);
            if (!painter)
            {
                return PDFTranslationContext::tr("Failed to open content stream for page %1.").arg(pageIndex + 1);
            }

            for (const SideWork& work : sidesToApply)
            {
                const PDFReal sampleDepthPt = (settings.mode == PDFBleedFixupMode::Mirror)
                                                  ? work.depthPt
                                                  : (72.0 / PDFReal(settings.dpi)) * PDFReal(settings.samplePixels);

                const QRectF sourcePageRect = PDFBleedFixupMath::edgeStripSourceRect(reference, work.side, sampleDepthPt);
                const QRectF destPageRect = PDFBleedFixupMath::edgeStripDestRect(reference, work.side, work.depthPt);
                if (!sourcePageRect.isValid() || !destPageRect.isValid())
                {
                    continue;
                }

                const QRect sourcePx = mapPageRectToImage(sourcePageRect, pageToDevice, pageImage.size());
                const int bleedDepthPx = qMax(1, qCeil(work.depthPt * settings.dpi / 72.0));
                QImage fill = PDFBleedFixupMath::buildEdgeFillImage(pageImage, sourcePx, work.side, settings.mode, bleedDepthPx);
                if (fill.isNull())
                {
                    pageReport.skipReasons.append(PDFTranslationContext::tr("Empty edge strip on %1.").arg(sideName(work.side)));
                    continue;
                }

                painter->drawImage(mapOutputRect(destPageRect), fill);
                pageReport.sidesApplied.append(work.side);
                isPageContentChanged = true;
            }

            struct CornerWork
            {
                PDFBleedFixupSide horizontal = PDFBleedFixupSide::Left;
                PDFBleedFixupSide vertical = PDFBleedFixupSide::Bottom;
            };
            const CornerWork corners[4] = {
                { PDFBleedFixupSide::Left, PDFBleedFixupSide::Bottom },
                { PDFBleedFixupSide::Right, PDFBleedFixupSide::Bottom },
                { PDFBleedFixupSide::Left, PDFBleedFixupSide::Top },
                { PDFBleedFixupSide::Right, PDFBleedFixupSide::Top }
            };

            auto depthPtFor = [&sidesToApply](PDFBleedFixupSide side) -> PDFReal
            {
                for (const SideWork& work : sidesToApply)
                {
                    if (work.side == side)
                    {
                        return work.depthPt;
                    }
                }
                return 0.0;
            };

            for (const CornerWork& corner : corners)
            {
                const PDFReal horizontalDepthPt = depthPtFor(corner.horizontal);
                const PDFReal verticalDepthPt = depthPtFor(corner.vertical);
                if (!(horizontalDepthPt > 0.0) || !(verticalDepthPt > 0.0))
                {
                    continue;
                }

                const PDFReal sampleHorizontalPt = (settings.mode == PDFBleedFixupMode::Mirror)
                                                       ? horizontalDepthPt
                                                       : (72.0 / PDFReal(settings.dpi)) * PDFReal(settings.samplePixels);
                const PDFReal sampleVerticalPt = (settings.mode == PDFBleedFixupMode::Mirror)
                                                     ? verticalDepthPt
                                                     : (72.0 / PDFReal(settings.dpi)) * PDFReal(settings.samplePixels);

                const QRectF sourcePageRect = PDFBleedFixupMath::cornerStripSourceRect(
                    reference, corner.horizontal, corner.vertical, sampleHorizontalPt, sampleVerticalPt);
                const QRectF destPageRect = PDFBleedFixupMath::cornerStripDestRect(
                    reference, corner.horizontal, corner.vertical, horizontalDepthPt, verticalDepthPt);
                if (!sourcePageRect.isValid() || !destPageRect.isValid())
                {
                    continue;
                }

                const QRect sourcePx = mapPageRectToImage(sourcePageRect, pageToDevice, pageImage.size());
                const int destWidthPx = qMax(1, qCeil(horizontalDepthPt * settings.dpi / 72.0));
                const int destHeightPx = qMax(1, qCeil(verticalDepthPt * settings.dpi / 72.0));
                QImage fill = PDFBleedFixupMath::buildCornerFillImage(pageImage,
                                                                      sourcePx,
                                                                      corner.horizontal,
                                                                      corner.vertical,
                                                                      settings.mode,
                                                                      destWidthPx,
                                                                      destHeightPx);
                if (fill.isNull())
                {
                    continue;
                }

                painter->drawImage(mapOutputRect(destPageRect), fill);
                isPageContentChanged = true;
            }

            pageContentStreamBuilder.end(painter);
        }

        pageReports.append(pageReport);
    }

    if (!settings.analyzeOnly)
    {
        modifier.markReset();
        if (isPageContentChanged)
        {
            modifier.markPageContentsChanged();
            flags |= PDFModifiedDocument::PageContents;
        }

        if (!modifier.finalize())
        {
            return PDFTranslationContext::tr("Failed to finalize document modifications.");
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

}   // namespace pdf
