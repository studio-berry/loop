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


#ifndef CANVASTRACEOVERLAY_H
#define CANVASTRACEOVERLAY_H

#include "canvaspalette.h"
#include "loopquickglobal.h"

#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QStringList>

namespace pdfquick
{

/// What the canvas itself drew last frame. The recorder cannot know any of it --
/// it counts stages, not nodes.
struct CanvasFrameStats
{
    int tiles = 0;
    int inexactTiles = 0;
    int overlayPrimitives = 0;

    /// Primitives the node builder could not draw.
    int skippedPrimitives = 0;

    /// Reported by OverlayFrame itself: dropped at the builder's bound, and
    /// emitted with renderable == false.
    int droppedPrimitives = 0;
    int unrenderablePrimitives = 0;

    /// Frames the presenter refused to draw because the snapshot or the overlay
    /// frame carried a revision that is no longer current. Shown because a
    /// canvas that is correctly refusing and a canvas that has stopped being
    /// asked for anything look identical from the outside.
    int refusedStaleFrames = 0;
};

/// The developer-facing trace overlay -- the other half of issue #140 that P4-S4
/// could not deliver, for the same reason: a layer with no scene graph has
/// nothing to draw on.
///
/// **It renders only the privacy-safe summaries.** Its inputs are
/// InteractionTraceRecorder::summary(), CanvasPresentMetrics::summary(), and the
/// node counts above. Every one of those is a timing, a count or an enumeration
/// name by construction, so the overlay cannot display page text, geometry, a
/// file path or a revision identity even if the canvas beneath it is showing a
/// filled-in form. Passing document-derived values in here would defeat issue
/// #140 AC6, which is why this function takes summaries rather than the
/// recorder, the controller or the document.
///
/// It is deliberately a pure function returning a QImage rather than a QQuickItem
/// or a QML component: it has no state to get out of sync, it is callable from
/// the render thread, and a test can assert on its output without a window.
class LOOPLIBQUICK_EXPORT CanvasTraceOverlay
{
public:
    /// Renders the panel, or a null image when there is nothing to report.
    ///
    /// `devicePixelRatio` is baked into the returned image so the text is sharp
    /// on a scaled display; the image's logical size is its pixel size divided
    /// by that ratio.
    static QImage render(const QJsonObject& traceSummary,
                         const QJsonObject& presentSummary,
                         const CanvasFrameStats& stats,
                         const CanvasPalette& palette,
                         qreal devicePixelRatio);

    /// The lines the panel would contain, exposed so a test can assert on the
    /// content -- including that it contains no document payload -- without
    /// rasterizing anything.
    static QStringList lines(const QJsonObject& traceSummary,
                             const QJsonObject& presentSummary,
                             const CanvasFrameStats& stats);
};

}   // namespace pdfquick

#endif   // CANVASTRACEOVERLAY_H
