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

#ifndef PDFIMAGEDECODEGUARD_H
#define PDFIMAGEDECODEGUARD_H

#include "pdfglobal.h"
#include "pdfprocessingbudget.h"

#include <QByteArray>
#include <QString>

#include <cstdint>

namespace pdf
{

class PDFImageData;

/// Shared pre-allocation boundary for untrusted PDF image decode / raster paths.
/// Call these before QImage / large decoder output buffers so hostile Width/Height
/// (or codec-reported dimensions) cannot force unbounded resident memory before
/// PDFProcessingBudget has a chance to fail closed.
namespace PDFImageDecodeGuard
{

constexpr PDFInteger MAXIMUM_IMAGE_DIMENSION = 16384;
constexpr PDFInteger MAXIMUM_IMAGE_PIXELS = MAXIMUM_IMAGE_DIMENSION * MAXIMUM_IMAGE_DIMENSION;

/// Ceiling of (width * height * components * bitsPerComponent) / 8.
/// Returns false on overflow or invalid geometry.
LOOPLIBCORESHARED_EXPORT bool tryExpectedMinImageSampleBytes(std::uint64_t width,
                                                              std::uint64_t height,
                                                              std::uint64_t components,
                                                              std::uint64_t bitsPerComponent,
                                                              std::uint64_t& outBytes);

LOOPLIBCORESHARED_EXPORT std::uint64_t expectedMinImageSampleBytes(std::uint64_t width,
                                                                   std::uint64_t height,
                                                                   std::uint64_t components,
                                                                   std::uint64_t bitsPerComponent);

/// Reject dimensions outside the shared image caps (same policy as ordinary raw images).
LOOPLIBCORESHARED_EXPORT void requireImageDimensions(PDFInteger width, PDFInteger height);

/// Reject sample buffers shorter than the declared geometry requires.
LOOPLIBCORESHARED_EXPORT void requireSufficientSampleBytes(const QByteArray& data,
                                                           std::uint64_t width,
                                                           std::uint64_t height,
                                                           std::uint64_t components,
                                                           std::uint64_t bitsPerComponent,
                                                           std::uint64_t stride = 0);

LOOPLIBCORESHARED_EXPORT void requireSufficientSampleBytes(const PDFImageData& imageData);

/// Charge render pixels before raster / decoder output allocation. No-op when budget is null.
LOOPLIBCORESHARED_EXPORT void reserveRenderPixels(PDFProcessingBudget* budget,
                                                  std::uint64_t pixelCount,
                                                  QString context = {});

}   // namespace PDFImageDecodeGuard

}   // namespace pdf

#endif // PDFIMAGEDECODEGUARD_H
