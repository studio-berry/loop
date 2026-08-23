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

#include "pagesurfacekey.h"

#include <QtGlobal>

#include <cmath>
#include <tuple>
#include <utility>

namespace pdfinteraction
{

namespace
{

/// Zoom buckets are logarithmic with sixteen steps per doubling. Linear buckets
/// would be far too coarse at 25% and far too fine at 800%; sixteen steps keeps
/// the worst-case resolution error of a reused surface near 4% at every zoom.
constexpr double ZoomBucketsPerDoubling = 16.0;

/// Below this, zoom is not a meaningful ratio and every value shares bucket zero.
constexpr double MinimumBucketedZoom = 1e-6;

}   // namespace

int zoomBucketFor(qreal zoom)
{
    if (!(zoom > MinimumBucketedZoom))
    {
        return 0;
    }

    return static_cast<int>(std::lround(std::log2(static_cast<double>(zoom)) * ZoomBucketsPerDoubling));
}

bool PageSurfaceKey::operator<(const PageSurfaceKey& other) const
{
    // Any strict weak ordering will do -- this exists so a key can be a std::map
    // key, not to rank surfaces. The two heavyweight fields are compared first
    // and by reference so the tuple below stays cheap and copy-free.
    if (revision != other.revision)
    {
        return revision < other.revision;
    }

    if (colorOutputIdentity != other.colorOutputIdentity)
    {
        return colorOutputIdentity < other.colorOutputIdentity;
    }

    return std::make_tuple(pageIndex, rotation, featureBits, zoomBucket, targetPixelSize.width(), targetPixelSize.height(), devicePixelRatio1000, pageTileBounds.x(), pageTileBounds.y(), pageTileBounds.width(), pageTileBounds.height()) < std::make_tuple(other.pageIndex, other.rotation, other.featureBits, other.zoomBucket, other.targetPixelSize.width(), other.targetPixelSize.height(), other.devicePixelRatio1000, other.pageTileBounds.x(), other.pageTileBounds.y(), other.pageTileBounds.width(), other.pageTileBounds.height());
}

bool PageSurfaceKey::compatibleWith(const PageSurfaceKey& desired) const
{
    // Every field except the two resolution fields. Adding a field to the struct
    // without adding it here is how a surface from a different output path ends
    // up on screen, so the test flips each field in turn.
    return revision == desired.revision && pageIndex == desired.pageIndex && rotation == desired.rotation && featureBits == desired.featureBits && colorOutputIdentity == desired.colorOutputIdentity && devicePixelRatio1000 == desired.devicePixelRatio1000 && pageTileBounds == desired.pageTileBounds;
}

bool PageSurfaceKey::isValid() const
{
    return revision.isValid() && pageIndex >= 0 && targetPixelSize.width() > 0 && targetPixelSize.height() > 0;
}

PageSurfaceKey makePageSurfaceKey(const pdf::PDFRevisionIdentity& revision,
                                  int pageIndex,
                                  pdf::PageRotation rotation,
                                  pdf::PDFRenderer::Features features,
                                  const QString& colorOutputIdentity,
                                  qreal zoom,
                                  QSize targetPixelSize,
                                  qreal devicePixelRatio,
                                  QRectF pageTileBounds)
{
    PageSurfaceKey key;
    key.revision = revision;
    key.pageIndex = pageIndex;
    key.rotation = rotation;
    key.featureBits = static_cast<int>(features);
    key.colorOutputIdentity = colorOutputIdentity;
    key.zoomBucket = zoomBucketFor(zoom);

    // A zero or negative dimension would render nothing and hash as a distinct
    // demand every time it is recomputed from a rounded rect.
    key.targetPixelSize = QSize(qMax(1, targetPixelSize.width()), qMax(1, targetPixelSize.height()));

    key.devicePixelRatio1000 = qMax(1, static_cast<int>(std::lround(static_cast<double>(devicePixelRatio) * 1000.0)));

    // Normalized so that the several ways of spelling "no tile" -- a default
    // QRectF, an empty one, a negative-width one -- are one value rather than
    // several keys for one demand.
    key.pageTileBounds = pageTileBounds.isEmpty() ? QRectF() : pageTileBounds.normalized();

    return key;
}

const char* getSurfaceTerminalStateName(SurfaceTerminalState state)
{
    switch (state)
    {
        case SurfaceTerminalState::Complete:
            return "complete";
        case SurfaceTerminalState::Cancelled:
            return "cancelled";
        case SurfaceTerminalState::Failed:
            return "failed";
        case SurfaceTerminalState::Stale:
            return "stale";
        case SurfaceTerminalState::BudgetExhausted:
            return "budget-exhausted";
    }

    return "failed";
}

SurfaceBufferPointer makeSurfaceBuffer(QImage image)
{
    if (image.isNull())
    {
        return {};
    }

    auto buffer = std::make_shared<SurfaceBuffer>();
    buffer->byteSize = static_cast<qint64>(image.sizeInBytes());
    buffer->image = std::move(image);

    return buffer;
}

}   // namespace pdfinteraction
