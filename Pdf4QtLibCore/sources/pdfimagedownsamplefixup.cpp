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

#include "pdfimagedownsamplefixup.h"

#include <QtGlobal>

#include <cmath>

namespace pdf
{

PDFOperationResult PDFImageDownsampleFixup::apply(PDFDocument* document,
                                                   const PDFImageDownsampleFixupSettings& settings,
                                                   PDFImageDownsampleFixupReport* report)
{
    if (report)
    {
        *report = PDFImageDownsampleFixupReport();
    }

    if (!document)
    {
        return PDFOperationResult(QStringLiteral("Invalid document."));
    }
    if (settings.targetDpi < 72 || settings.targetDpi > 1200)
    {
        return PDFOperationResult(QStringLiteral("Target DPI must be between 72 and 1200."));
    }

    PDFImageOptimizer::Settings optimizerSettings = PDFImageOptimizer::Settings::createDefault();
    optimizerSettings.enabled = true;
    optimizerSettings.autoMode = false;
    optimizerSettings.colorMode = settings.preserveColorMode
        ? PDFImageOptimizer::ColorMode::Preserve
        : PDFImageOptimizer::ColorMode::Color;
    optimizerSettings.goal = PDFImageOptimizer::OptimizationGoal::PreferQuality;
    optimizerSettings.keepOriginalIfLarger = settings.keepOriginalIfLarger;
    optimizerSettings.preserveTransparency = settings.preserveTransparency;

    const auto configureProfile = [&](PDFImageOptimizer::CompressionProfile& profile)
    {
        profile.targetDpi = settings.targetDpi;
        profile.resampleFilter = PDFImage::ResampleFilter::Bicubic;
        profile.jpegQuality = qBound(50, settings.jpegQuality, 100);
    };
    configureProfile(optimizerSettings.colorProfile);
    configureProfile(optimizerSettings.grayProfile);
    configureProfile(optimizerSettings.bitonalProfile);

    const std::vector<PDFImageOptimizer::ImageInfo> imageInfos = PDFImageOptimizer::collectImageInfos(document);
    PDFImageOptimizer::ImageOverrides overrides;
    constexpr double qualityThreshold = 1.15;
    for (const PDFImageOptimizer::ImageInfo& image : imageInfos)
    {
        if (image.isImageMask)
        {
            PDFImageOptimizer::ImageOverride overrideSettings;
            overrideSettings.enabled = false;
            overrides.emplace(image.reference, overrideSettings);
            continue;
        }
        const double dpiX = image.minimalDpi.x();
        const double dpiY = image.minimalDpi.y();
        const bool eligible = (std::isfinite(dpiX) && dpiX > settings.targetDpi * qualityThreshold)
            || (std::isfinite(dpiY) && dpiY > settings.targetDpi * qualityThreshold);
        if (!eligible)
        {
            PDFImageOptimizer::ImageOverride overrideSettings;
            overrideSettings.enabled = false;
            overrides.emplace(image.reference, overrideSettings);
        }
    }

    PDFImageOptimizer optimizer;
    std::vector<PDFImageOptimizer::ImageResult> imageResults;
    PDFDocument optimized = optimizer.optimize(document,
                                               optimizerSettings,
                                               overrides,
                                               nullptr,
                                               nullptr,
                                               &imageResults);
    if (report)
    {
        report->imagesExamined = static_cast<int>(imageInfos.size());
        report->images = imageResults;
        for (const PDFImageOptimizer::ImageResult& image : imageResults)
        {
            report->originalBytes += image.originalBytes;
            if (image.keptOriginal)
            {
                ++report->imagesSkipped;
                report->resultingBytes += image.originalBytes;
            }
            else
            {
                ++report->imagesChanged;
                report->resultingBytes += image.newBytes;
            }
        }
    }

    *document = std::move(optimized);
    return PDFOperationResult(true);
}

} // namespace pdf
