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

#include "pdfthinpartprobe.h"

#include "pdfprocessingbudget.h"

#include <QColor>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace pdf
{

namespace
{

bool isUsableBounds(const QRectF& bounds)
{
    return bounds.isValid() && std::isfinite(bounds.left()) && std::isfinite(bounds.top()) && std::isfinite(bounds.width()) && std::isfinite(bounds.height()) && bounds.width() > 0.0 && bounds.height() > 0.0;
}

QImage rasterizeMask(const QPainterPath& path,
                     const QRectF& bounds,
                     int dpi,
                     qint64 maxRasterPixels,
                     const QString& context)
{
    const qreal scale = static_cast<qreal>(dpi) / 72.0;
    const double widthReal = std::ceil(bounds.width() * scale) + 2.0;
    const double heightReal = std::ceil(bounds.height() * scale) + 2.0;
    if (!std::isfinite(widthReal) || !std::isfinite(heightReal) || widthReal <= 0.0 || heightReal <= 0.0 || widthReal > static_cast<double>(std::numeric_limits<int>::max()) || heightReal > static_cast<double>(std::numeric_limits<int>::max()))
    {
        PDFBudgetExceeded detail;
        detail.kind = PDFBudgetKind::RenderPixels;
        detail.pool = budgetPoolFor(detail.kind);
        detail.limit = static_cast<std::uint64_t>(std::max<qint64>(1, maxRasterPixels));
        detail.attempted = std::numeric_limits<std::uint64_t>::max();
        detail.context = context;
        throw PDFBudgetExceededException(detail);
    }

    const int width = std::max(1, static_cast<int>(widthReal));
    const int height = std::max(1, static_cast<int>(heightReal));
    const qint64 rasterPixels = static_cast<qint64>(width) * static_cast<qint64>(height);
    if (maxRasterPixels > 0 && rasterPixels > maxRasterPixels)
    {
        PDFBudgetExceeded detail;
        detail.kind = PDFBudgetKind::RenderPixels;
        detail.pool = budgetPoolFor(detail.kind);
        detail.limit = static_cast<std::uint64_t>(maxRasterPixels);
        detail.attempted = static_cast<std::uint64_t>(rasterPixels);
        detail.context = context;
        throw PDFBudgetExceededException(detail);
    }

    QImage mask(QSize(width, height), QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QTransform transform(scale,
                               0.0,
                               0.0,
                               scale,
                               1.0 - bounds.left() * scale,
                               1.0 - bounds.top() * scale);
    painter.setTransform(transform);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(path);
    painter.end();
    return mask;
}

std::vector<float> distanceTransform(const QImage& mask, bool invert)
{
    const int width = mask.width();
    const int height = mask.height();
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    const float infinity = std::numeric_limits<float>::infinity();
    std::vector<float> distances(count, infinity);

    const auto isInside = [&mask, invert](int x, int y)
    {
        const bool painted = qGray(mask.pixel(x, y)) > 0;
        // The raster has a one-pixel guard band around the bounded path. It
        // must not become a negative-space candidate, otherwise every path
        // would appear to have a thin gap at the mask boundary.
        return invert
                   ? (!painted && x > 0 && y > 0 && x + 1 < mask.width() && y + 1 < mask.height())
                   : painted;
    };

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (!isInside(x, y))
            {
                distances[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = 0.0f;
            }
        }
    }

    const float diagonal = std::sqrt(2.0f);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float& distance = distances[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            if (x > 0)
                distance = std::min(distance, distances[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x - 1)] + 1.0f);
            if (y > 0)
                distance = std::min(distance, distances[static_cast<size_t>(y - 1) * static_cast<size_t>(width) + static_cast<size_t>(x)] + 1.0f);
            if (x > 0 && y > 0)
                distance = std::min(distance, distances[static_cast<size_t>(y - 1) * static_cast<size_t>(width) + static_cast<size_t>(x - 1)] + diagonal);
            if (x + 1 < width && y > 0)
                distance = std::min(distance, distances[static_cast<size_t>(y - 1) * static_cast<size_t>(width) + static_cast<size_t>(x + 1)] + diagonal);
        }
    }
    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = width - 1; x >= 0; --x)
        {
            float& distance = distances[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            if (x + 1 < width)
                distance = std::min(distance, distances[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x + 1)] + 1.0f);
            if (y + 1 < height)
                distance = std::min(distance, distances[static_cast<size_t>(y + 1) * static_cast<size_t>(width) + static_cast<size_t>(x)] + 1.0f);
            if (x + 1 < width && y + 1 < height)
                distance = std::min(distance, distances[static_cast<size_t>(y + 1) * static_cast<size_t>(width) + static_cast<size_t>(x + 1)] + diagonal);
            if (x > 0 && y + 1 < height)
                distance = std::min(distance, distances[static_cast<size_t>(y + 1) * static_cast<size_t>(width) + static_cast<size_t>(x - 1)] + diagonal);
        }
    }

    return distances;
}

}   // namespace

PDFThinPartMeasurement measureThinPartPath(const QPainterPath& pagePath,
                                           int dpi,
                                           qint64 maxRasterPixels,
                                           bool invert,
                                           const QString& context)
{
    PDFThinPartMeasurement result;
    if (dpi <= 0 || pagePath.isEmpty())
    {
        return result;
    }

    const QRectF bounds = pagePath.boundingRect().normalized();
    if (!isUsableBounds(bounds))
    {
        return result;
    }

    const QImage mask = rasterizeMask(pagePath, bounds, dpi, maxRasterPixels, context);
    const std::vector<float> distances = distanceTransform(mask, invert);
    const int width = mask.width();
    const int height = mask.height();
    const qreal precisionPt = 72.0 / static_cast<qreal>(dpi);
    const auto isInside = [&mask, invert](int x, int y)
    {
        const bool painted = qGray(mask.pixel(x, y)) > 0;
        return invert ? !painted : painted;
    };

    float thinnestRadius = std::numeric_limits<float>::infinity();
    float largestRadius = 0.0f;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (!isInside(x, y))
            {
                continue;
            }

            const float distance = distances[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            largestRadius = std::max(largestRadius, distance);
            bool localMaximum = distance > 0.5f;
            for (int neighborY = std::max(0, y - 1); neighborY <= std::min(height - 1, y + 1) && localMaximum; ++neighborY)
            {
                for (int neighborX = std::max(0, x - 1); neighborX <= std::min(width - 1, x + 1); ++neighborX)
                {
                    if (neighborX == x && neighborY == y)
                    {
                        continue;
                    }
                    if (isInside(neighborX, neighborY) && distances[static_cast<size_t>(neighborY) * static_cast<size_t>(width) + static_cast<size_t>(neighborX)] > distance + 0.001f)
                    {
                        localMaximum = false;
                        break;
                    }
                }
            }
            if (localMaximum)
            {
                thinnestRadius = std::min(thinnestRadius, distance);
            }
        }
    }

    if (!std::isfinite(thinnestRadius))
    {
        thinnestRadius = largestRadius;
    }
    if (!(thinnestRadius > 0.0f))
    {
        return result;
    }

    result.measured = true;
    result.widthPt = 2.0 * static_cast<qreal>(thinnestRadius) * precisionPt;
    result.precisionPt = precisionPt;
    result.bbox = bounds;
    return result;
}

}   // namespace pdf
