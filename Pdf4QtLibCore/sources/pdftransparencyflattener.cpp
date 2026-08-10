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

#include "pdftransparencyflattener.h"

#include "pdfdocument.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdfcolorspaces.h"
#include "pdfoptimizer.h"
#include "pdfpage.h"
#include "pdfrenderer.h"
#include "pdfstreamfilters.h"
#include "pdftransparencyrenderer.h"

#include <QJsonArray>
#include <QPainter>

#include <algorithm>

namespace pdf
{

namespace
{

bool isTransparencyDictionary(const PDFDictionary* dictionary,
                              const PDFDocumentDataLoaderDecorator& loader,
                              QString* finding)
{
    if (!dictionary)
    {
        return false;
    }

    bool transparent = loader.readNameFromDictionary(dictionary, "S") == QByteArrayLiteral("Transparency")
        || dictionary->hasKey("SMask")
        || dictionary->hasKey("ca")
        || dictionary->hasKey("CA");
    if (dictionary->hasKey("BM"))
    {
        const QByteArray blendMode = loader.readNameFromDictionary(dictionary, "BM");
        transparent = transparent || (!blendMode.isEmpty()
                                       && blendMode != QByteArrayLiteral("Normal")
                                       && blendMode != QByteArrayLiteral("Compatible"));
    }

    if (transparent && finding)
    {
        *finding = QStringLiteral("transparency dictionary");
    }
    return transparent;
}

std::vector<int> pageIndices(const PDFDocument* document, const QString& pageRange)
{
    std::vector<int> result;
    const int pageCount = int(document->getCatalog()->getPageCount());
    if (pageRange.trimmed().isEmpty())
    {
        result.reserve(pageCount);
        for (int index = 0; index < pageCount; ++index)
        {
            result.push_back(index);
        }
        return result;
    }

    const QStringList parts = pageRange.split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts)
    {
        const QStringList range = part.trimmed().split('-', Qt::KeepEmptyParts);
        bool firstOk = false;
        const int first = range.value(0).toInt(&firstOk);
        if (!firstOk || first < 1)
        {
            return {};
        }
        int last = first;
        if (range.size() > 1 && !range[1].trimmed().isEmpty())
        {
            bool lastOk = false;
            last = range[1].toInt(&lastOk);
            if (!lastOk)
            {
                return {};
            }
        }
        else if (range.size() > 1)
        {
            last = pageCount;
        }
        if (last < first || last > pageCount)
        {
            return {};
        }
        for (int page = first; page <= last; ++page)
        {
            result.push_back(page - 1);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

QSize rasterSize(const PDFPage* page, int dpi)
{
    const QRectF mediaBox = page->getRotatedMediaBox();
    return QSize(qMax(1, qCeil(mediaBox.width() * dpi / 72.0)),
                 qMax(1, qCeil(mediaBox.height() * dpi / 72.0)));
}

void replacePageWithDeviceNImage(PDFDocumentBuilder& builder,
                                 PDFObjectReference pageReference,
                                 const QRectF& mediaBox,
                                 const PDFFloatBitmapWithColorSpace& bitmap,
                                 const PDFInkMapper& inkMapper)
{
    const PDFPixelFormat format = bitmap.getPixelFormat();
    const int colorCount = int(format.getColorChannelCount());
    QByteArray samples;
    samples.reserve(int(bitmap.getWidth() * bitmap.getHeight() * size_t(colorCount)));
    for (size_t y = 0; y < bitmap.getHeight(); ++y)
    {
        for (size_t x = 0; x < bitmap.getWidth(); ++x)
        {
            const PDFConstColorBuffer pixel = bitmap.getPixel(x, y);
            const PDFColorComponent opacity = qBound<PDFColorComponent>(0.0f,
                                                                          pixel[format.getOpacityChannelIndex()] * pixel[format.getShapeChannelIndex()],
                                                                          1.0f);
            for (int channel = 0; channel < colorCount; ++channel)
            {
                PDFColorComponent value = qBound<PDFColorComponent>(0.0f, pixel[size_t(channel)], 1.0f);
                if (format.hasProcessColorsSubtractive() || channel >= int(format.getProcessColorChannelCount()))
                {
                    value *= opacity;
                }
                else
                {
                    value = 1.0f - (1.0f - value) * opacity;
                }
                samples.append(char(qRound(value * 255.0f)));
            }
        }
    }

    PDFArray colorantNames;
    for (const PDFInkMapper::ColorInfo& info : inkMapper.getSeparations(4, true))
    {
        colorantNames.appendItem(PDFObject::createName(info.name));
    }

    PDFArray domain;
    for (int channel = 0; channel < colorCount; ++channel)
    {
        domain.appendItem(PDFObject::createInteger(0));
        domain.appendItem(PDFObject::createInteger(1));
    }
    PDFArray range;
    for (int channel = 0; channel < 4; ++channel)
    {
        range.appendItem(PDFObject::createInteger(0));
        range.appendItem(PDFObject::createInteger(1));
    }
    QByteArray tintCode;
    for (int channel = 4; channel < colorCount; ++channel)
    {
        tintCode.append("pop\n");
    }
    PDFDictionary functionDictionary;
    functionDictionary.addEntry(PDFInplaceOrMemoryString("FunctionType"), PDFObject::createInteger(4));
    functionDictionary.addEntry(PDFInplaceOrMemoryString("Domain"), PDFObject::createArray(std::make_shared<PDFArray>(qMove(domain))));
    functionDictionary.addEntry(PDFInplaceOrMemoryString("Range"), PDFObject::createArray(std::make_shared<PDFArray>(qMove(range))));
    functionDictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(tintCode.size()));
    const PDFObjectReference functionReference = builder.addObject(PDFObject::createStream(std::make_shared<PDFStream>(qMove(functionDictionary), qMove(tintCode))));

    PDFArray colorSpace;
    colorSpace.appendItem(PDFObject::createName("DeviceN"));
    colorSpace.appendItem(PDFObject::createArray(std::make_shared<PDFArray>(qMove(colorantNames))));
    colorSpace.appendItem(PDFObject::createName("DeviceCMYK"));
    colorSpace.appendItem(PDFObject::createReference(functionReference));

    PDFDictionary imageDictionary;
    imageDictionary.addEntry(PDFInplaceOrMemoryString("Type"), PDFObject::createName("XObject"));
    imageDictionary.addEntry(PDFInplaceOrMemoryString("Subtype"), PDFObject::createName("Image"));
    imageDictionary.addEntry(PDFInplaceOrMemoryString("Width"), PDFObject::createInteger(PDFInteger(bitmap.getWidth())));
    imageDictionary.addEntry(PDFInplaceOrMemoryString("Height"), PDFObject::createInteger(PDFInteger(bitmap.getHeight())));
    imageDictionary.addEntry(PDFInplaceOrMemoryString("ColorSpace"), PDFObject::createArray(std::make_shared<PDFArray>(qMove(colorSpace))));
    imageDictionary.addEntry(PDFInplaceOrMemoryString("BitsPerComponent"), PDFObject::createInteger(8));
    imageDictionary.addEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
    imageDictionary.addEntry(PDFInplaceOrMemoryString("Interpolate"), PDFObject::createBool(true));
    const QByteArray compressedSamples = PDFFlateDecodeFilter::compress(samples);
    imageDictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(compressedSamples.size()));
    const PDFObjectReference imageReference = builder.addObject(PDFObject::createStream(std::make_shared<PDFStream>(qMove(imageDictionary), QByteArray(compressedSamples))));

    PDFDictionary xObjects;
    xObjects.addEntry(PDFInplaceOrMemoryString("Im0"), PDFObject::createReference(imageReference));
    PDFDictionary resources;
    resources.addEntry(PDFInplaceOrMemoryString("XObject"), PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(xObjects))));
    PDFDictionary pageUpdate;
    pageUpdate.addEntry(PDFInplaceOrMemoryString("Resources"), PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(resources))));
    const QByteArray content = QByteArrayLiteral("q\n")
        + QByteArray::number(mediaBox.width()) + ' ' + QByteArrayLiteral("0 0 ")
        + QByteArray::number(mediaBox.height()) + ' '
        + QByteArray::number(mediaBox.left()) + ' ' + QByteArray::number(mediaBox.bottom())
        + QByteArrayLiteral(" cm\n/Im0 Do\nQ\n");
    PDFDictionary contentDictionary;
    contentDictionary.addEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
    const QByteArray compressedContent = PDFFlateDecodeFilter::compress(content);
    contentDictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(compressedContent.size()));
    const PDFObjectReference contentReference = builder.addObject(PDFObject::createStream(std::make_shared<PDFStream>(qMove(contentDictionary), QByteArray(compressedContent))));
    pageUpdate.addEntry(PDFInplaceOrMemoryString("Contents"), PDFObject::createReference(contentReference));
    pageUpdate.addEntry(PDFInplaceOrMemoryString("Group"), PDFObject::createNull());
    builder.mergeTo(pageReference, PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(pageUpdate))));
}

} // namespace

QJsonObject PDFTransparencyFlattenRegionReport::toJson() const
{
    return QJsonObject{
        { QStringLiteral("page"), pageIndex + 1 },
        { QStringLiteral("bounds_pt"), QJsonArray{ bounds.left(), bounds.top(), bounds.width(), bounds.height() } },
        { QStringLiteral("reason"), reason }
    };
}

QJsonObject PDFTransparencyFlattenPageReport::toJson() const
{
    QJsonArray regionsJson;
    for (const PDFTransparencyFlattenRegionReport& region : regions)
    {
        regionsJson.append(region.toJson());
    }
    QJsonArray warningsJson;
    for (const QString& warning : warnings)
    {
        warningsJson.append(warning);
    }
    return QJsonObject{
        { QStringLiteral("page"), pageIndex + 1 },
        { QStringLiteral("flattened"), flattened },
        { QStringLiteral("raster_size"), QJsonArray{ rasterSize.width(), rasterSize.height() } },
        { QStringLiteral("regions"), regionsJson },
        { QStringLiteral("warnings"), warningsJson }
    };
}

QJsonObject PDFTransparencyFlattenReport::toJson() const
{
    QJsonArray pagesJson;
    for (const PDFTransparencyFlattenPageReport& page : pages)
    {
        pagesJson.append(page.toJson());
    }
    QJsonArray warningsJson;
    for (const QString& warning : warnings)
    {
        warningsJson.append(warning);
    }
    return QJsonObject{
        { QStringLiteral("changed"), changed },
        { QStringLiteral("fully_opaque"), fullyOpaque },
        { QStringLiteral("pages"), pagesJson },
        { QStringLiteral("warnings"), warningsJson }
    };
}

PDFOperationResult PDFTransparencyFlattener::apply(PDFDocument* document,
                                                    const PDFTransparencyFlattenSettings& settings,
                                                    PDFTransparencyFlattenReport* report,
                                                    PDFProgress* progress)
{
    if (!document || !document->getCatalog())
    {
        return PDFTranslationContext::tr("Transparency flattening requires a valid PDF document.");
    }
    if (settings.rasterizationDpi <= 0 || settings.maxRasterPixels <= 0)
    {
        return PDFTranslationContext::tr("Transparency flattening requires positive rasterization limits.");
    }
    if (settings.preserveTextAndVector)
    {
        return PDFTranslationContext::tr("Vector-preserving transparency flattening is not available in this production mode.");
    }

    PDFTransparencyFlattenReport localReport;
    PDFTransparencyFlattenReport& outputReport = report ? *report : localReport;
    outputReport = PDFTransparencyFlattenReport();

    const std::vector<int> pages = pageIndices(document, settings.pageRange);
    if (pages.empty() && !settings.pageRange.trimmed().isEmpty())
    {
        return PDFTranslationContext::tr("Invalid transparency flattening page range '%1'.").arg(settings.pageRange);
    }

    PDFDocumentSession session(document);
    PDFDocumentBuilder builder(document);
    for (int pageIndex : pages)
    {
        const PDFPage* page = document->getCatalog()->getPage(size_t(pageIndex));
        const QSize size = rasterSize(page, settings.rasterizationDpi);
        const qint64 pixels = qint64(size.width()) * qint64(size.height());
        if (pixels > settings.maxRasterPixels)
        {
            return PDFTranslationContext::tr("Page %1 requires %2 raster pixels, exceeding the configured limit of %3.")
                .arg(pageIndex + 1).arg(pixels).arg(settings.maxRasterPixels);
        }

        PDFTransparencyFlattenPageReport pageReport;
        pageReport.pageIndex = pageIndex;
        pageReport.rasterSize = size;

        PDFInkMapper inkMapper(nullptr, document);
        inkMapper.createSpotColors(true);
        const bool preserveSpots = settings.preserveSpotColors && inkMapper.getActiveSpotColorCount() > 0;

        PDFTransparencyRendererSettings rendererSettings;
        rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayImages, true);
        rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayText, true);
        rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayVectorGraphics, true);
        rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayShadings, true);
        rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::DisplayTilingPatterns, true);
        rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::MultithreadedPathSampler, true);
        rendererSettings.flags.setFlag(PDFTransparencyRendererSettings::SaveOriginalProcessImage, preserveSpots);

        PDFTransparencyRenderer renderer(page,
                                         document,
                                         session.getFontCache(),
                                         session.getCMS(),
                                         session.getOptionalContentActivity(),
                                         &inkMapper,
                                         rendererSettings,
                                         PDFRenderer::createPagePointToDevicePointMatrix(page, QRect(QPoint(0, 0), size)));
        if (preserveSpots)
        {
            const PDFColorSpacePointer cmykColorSpace(new PDFDeviceCMYKColorSpace());
            renderer.setDeviceColorSpace(cmykColorSpace);
            renderer.setProcessColorSpace(cmykColorSpace);
        }
        renderer.beginPaint(size);
        renderer.processContents();
        renderer.endPaint();
        QImage image;
        PDFFloatBitmapWithColorSpace processBitmap;
        if (preserveSpots)
        {
            processBitmap = renderer.getOriginalProcessBitmap();
            if (processBitmap.getWidth() == 0 || processBitmap.getHeight() == 0)
            {
                return PDFTranslationContext::tr("Failed to capture spot separations for page %1 during transparency flattening.").arg(pageIndex + 1);
            }
        }
        else
        {
            image = renderer.toImage(false, true, PDFRGB{ 1.0f, 1.0f, 1.0f });
            if (image.isNull())
            {
                return PDFTranslationContext::tr("Failed to rasterize page %1 during transparency flattening.").arg(pageIndex + 1);
            }
        }

        pageReport.flattened = true;
        PDFTransparencyFlattenRegionReport region;
        region.pageIndex = pageIndex;
        region.bounds = page->getMediaBox();
        region.reason = QStringLiteral("Full-page rasterization for transparency flattening (blend modes, groups, soft masks, knockout, and isolation).");
        pageReport.regions.append(region);

        if (!settings.analyzeOnly)
        {
            if (preserveSpots)
            {
                replacePageWithDeviceNImage(builder, page->getPageReference(), page->getMediaBox(), processBitmap, inkMapper);
            }
            else
            {
                PDFPageContentStreamBuilder contentBuilder(&builder,
                                                            PDFContentStreamBuilder::CoordinateSystem::PDF,
                                                            PDFPageContentStreamBuilder::Mode::Replace);
                QPainter* painter = contentBuilder.begin(page->getPageReference());
                if (!painter)
                {
                    return PDFTranslationContext::tr("Failed to replace page content for page %1.").arg(pageIndex + 1);
                }
                painter->drawImage(page->getMediaBox(), image);
                contentBuilder.end(painter);

                PDFObjectFactory pageUpdate;
                pageUpdate.beginDictionary();
                pageUpdate.beginDictionaryItem("Group");
                pageUpdate << PDFObject::createNull();
                pageUpdate.endDictionaryItem();
                pageUpdate.endDictionary();
                builder.mergeTo(page->getPageReference(), pageUpdate.takeObject());
            }
        }

        outputReport.pages.append(pageReport);
        outputReport.changed = true;
        if (progress)
        {
            progress->step();
        }
    }

    if (!settings.analyzeOnly)
    {
        *document = builder.build();
        PDFOptimizer optimizer(PDFOptimizer::OptimizationFlags(PDFOptimizer::RemoveNullObjects
                                                                  | PDFOptimizer::RemoveUnusedObjects
                                                                  | PDFOptimizer::ShrinkObjectStorage),
                               nullptr);
        optimizer.setDocument(document);
        optimizer.optimize();
        *document = optimizer.takeOptimizedDocument();
    }

    if (settings.analyzeOnly)
    {
        outputReport.fullyOpaque = false;
        return PDFOperationResult(true);
    }

    QStringList findings;
    outputReport.fullyOpaque = !hasLiveTransparency(document, &findings);
    if (!outputReport.fullyOpaque)
    {
        outputReport.warnings.append(findings);
        return PDFTranslationContext::tr("Transparency flattening revalidation found live transparency in the output document.");
    }
    return PDFOperationResult(true);
}

bool PDFTransparencyFlattener::hasLiveTransparency(const PDFDocument* document,
                                                    QStringList* findings)
{
    if (!document)
    {
        return true;
    }
    PDFDocumentDataLoaderDecorator loader(document);
    bool found = false;
    for (const PDFObjectStorage::Entry& entry : document->getStorage().getObjects())
    {
        QString finding;
        if (isTransparencyDictionary(document->getDictionaryFromObject(entry.object), loader, &finding))
        {
            found = true;
            if (findings)
            {
                findings->append(finding);
            }
        }
    }
    return found;
}

} // namespace pdf
