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

#include "canvassnapshot.h"

#include <QPainter>

namespace pdfinteraction
{

const CanvasTile* CanvasSnapshot::tileForPage(int pageIndex) const
{
    for (const CanvasTile& tile : tiles)
    {
        if (tile.key.pageIndex == pageIndex)
        {
            return &tile;
        }
    }

    return nullptr;
}

QImage composeCanvasImage(const CanvasSnapshot& snapshot, QRect viewportRect, qreal devicePixelRatio)
{
    const qreal ratio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;

    QImage image(viewportRect.size() * ratio, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
    {
        return image;
    }

    // A capture of the visible region shows paper where a page is and the
    // canvas background where one is not; the tiles carry the paper, so the
    // canvas is filled with white rather than left undefined.
    image.fill(Qt::white);
    image.setDevicePixelRatio(ratio);

    QPainter painter(&image);
    bool anyTile = false;
    for (const CanvasTile& tile : snapshot.tiles)
    {
        if (!tile.pixels || tile.pixels->image.isNull() || tile.placedRect.isEmpty())
        {
            continue;
        }

        if (!QRectF(tile.placedRect).intersects(QRectF(viewportRect)))
        {
            continue;
        }

        // The painter's coordinate space is viewport units: the composed
        // image's device pixel ratio maps them to device pixels, so the placed
        // rectangle is drawn unchanged. A tile rendered for another resolution
        // is scaled here exactly as the scene graph scales it, so the capture
        // shows what the canvas shows.
        painter.drawImage(QRectF(tile.placedRect), tile.pixels->image);
        anyTile = true;
    }
    painter.end();

    if (!anyTile)
    {
        return QImage();
    }

    return image;
}

}   // namespace pdfinteraction
