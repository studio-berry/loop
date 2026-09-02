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

#ifndef CANVASSNAPSHOT_H
#define CANVASSNAPSHOT_H

#include "pagesurfacekey.h"

#include <QList>
#include <QRect>

namespace pdfinteraction
{

/// One page's admitted pixels, placed in viewport coordinates.
struct CanvasTile
{
    PageSurfaceKey key;
    SurfaceBufferPointer pixels;
    QRect placedRect;

    /// False when this surface was rendered for a different resolution and is
    /// being scaled while the fidelity render is still in flight. A host may
    /// present an inexact tile; it may never present one from another revision.
    bool exact = true;
};

/// What the canvas should draw, as one immutable value.
///
/// The snapshot is replaced wholesale rather than mutated, so a scene-graph
/// thread reading it never sees a half-updated frame. Page pixels only: overlays
/// are a separate pass with a separate invalidation, which is what keeps a hover
/// change from costing a page rerender (P4-S4 owns them).
struct CanvasSnapshot
{
    RevisionFencedToken token;
    QList<CanvasTile> tiles;

    const CanvasTile* tileForPage(int pageIndex) const;
    bool isEmpty() const { return tiles.isEmpty(); }
};

}   // namespace pdfinteraction

#endif   // CANVASSNAPSHOT_H
