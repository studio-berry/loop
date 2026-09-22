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

#include "pdfimagedecodeguard.h"
#include "pdfcolorspaces.h"
#include "pdfexception.h"
#include "pdfutils.h"

#include "pdfdbgheap.h"

namespace pdf
{

namespace PDFImageDecodeGuard
{

bool tryExpectedMinImageSampleBytes(std::uint64_t width,
                                    std::uint64_t height,
                                    std::uint64_t components,
                                    std::uint64_t bitsPerComponent,
                                    std::uint64_t& outBytes)
{
    outBytes = 0;
    if (width == 0 || height == 0 || components == 0 || bitsPerComponent == 0)
    {
        return false;
    }

    std::uint64_t bits = 0;
    if (!pdfTryMultiply(width, height, bits) ||
        !pdfTryMultiply(bits, components, bits) ||
        !pdfTryMultiply(bits, bitsPerComponent, bits))
    {
        return false;
    }

    outBytes = (bits / 8) + ((bits % 8) != 0 ? 1 : 0);
    return true;
}

std::uint64_t expectedMinImageSampleBytes(std::uint64_t width,
                                          std::uint64_t height,
                                          std::uint64_t components,
                                          std::uint64_t bitsPerComponent)
{
    std::uint64_t bytes = 0;
    if (!tryExpectedMinImageSampleBytes(width, height, components, bitsPerComponent, bytes))
    {
        throw PDFRendererException(RenderErrorType::Error,
                                   PDFTranslationContext::tr("Invalid image sample geometry (%1x%2, %3 components, %4 bpc).")
                                       .arg(width)
                                       .arg(height)
                                       .arg(components)
                                       .arg(bitsPerComponent));
    }
    return bytes;
}

void requireImageDimensions(PDFInteger width, PDFInteger height)
{
    PDFInteger pixelCount = 0;
    if (width <= 0 || height <= 0 ||
        width > MAXIMUM_IMAGE_DIMENSION || height > MAXIMUM_IMAGE_DIMENSION ||
        !pdfTryMultiply(width, height, pixelCount) || pixelCount > MAXIMUM_IMAGE_PIXELS)
    {
        throw PDFRendererException(RenderErrorType::Error,
                                   PDFTranslationContext::tr("Invalid size of image (%1x%2)").arg(width).arg(height));
    }
}

void requireSufficientSampleBytes(const QByteArray& data,
                                  std::uint64_t width,
                                  std::uint64_t height,
                                  std::uint64_t components,
                                  std::uint64_t bitsPerComponent,
                                  std::uint64_t stride)
{
    std::uint64_t required = expectedMinImageSampleBytes(width, height, components, bitsPerComponent);
    if (stride > 0)
    {
        std::uint64_t strideBytes = 0;
        if (!pdfTryMultiply(stride, height, strideBytes))
        {
            throw PDFRendererException(RenderErrorType::Error,
                                       PDFTranslationContext::tr("Invalid image stride for size (%1x%2).").arg(width).arg(height));
        }
        if (strideBytes > required)
        {
            required = strideBytes;
        }
    }

    if (static_cast<std::uint64_t>(data.size()) < required)
    {
        throw PDFRendererException(RenderErrorType::Error,
                                   PDFTranslationContext::tr("Image data is too short for declared dimensions (%1 bytes, need at least %2).")
                                       .arg(data.size())
                                       .arg(required));
    }
}

void requireSufficientSampleBytes(const PDFImageData& imageData)
{
    requireSufficientSampleBytes(imageData.getData(),
                                 imageData.getWidth(),
                                 imageData.getHeight(),
                                 imageData.getComponents(),
                                 imageData.getBitsPerComponent(),
                                 imageData.getStride());
}

void reserveRenderPixels(PDFProcessingBudget* budget,
                         std::uint64_t pixelCount,
                         QString context)
{
    if (!budget || pixelCount == 0)
    {
        return;
    }

    if (context.isEmpty())
    {
        context = PDFTranslationContext::tr("image raster");
    }
    budget->chargeRenderPixels(pixelCount, std::move(context));
}

}   // namespace PDFImageDecodeGuard

}   // namespace pdf
