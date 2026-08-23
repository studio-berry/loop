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

#ifndef OVERLAYFRAME_H
#define OVERLAYFRAME_H

#include "interactionglobal.h"
#include "interactiontarget.h"
#include "pagesurfacekey.h"

#include <QList>
#include <QPolygonF>
#include <QRectF>
#include <QString>

namespace pdfinteraction
{

/// Deterministic z-bands, painted in declaration order: the first is furthest
/// back. This is the reverse of InteractionTargetKind's hit-test precedence,
/// which is what makes a target drawn on top also the target that wins a click.
///
/// The order is fixed, not configurable. A provider that could choose its own
/// band would make the composite order depend on registration, which is what
/// issue #143 AC3 rules out.
enum class OverlayLayer
{
    PageChrome,
    Guides,
    Findings,

    /// Transient pointer feedback. Its own band rather than a flag on the
    /// findings band, because a hover highlight is drawn over every marker
    /// including the ones it is not on.
    Hover,

    Selection,
    DragHandles,
    ToolPreview
};

const char* getOverlayLayerName(OverlayLayer layer);

/// What a primitive is, so a host picks a visual treatment without the neutral
/// layer naming colours or pen widths it cannot see.
enum class OverlayPrimitiveKind
{
    Rectangle,
    Polyline,
    Marker,
    Handle
};

const char* getOverlayPrimitiveKindName(OverlayPrimitiveKind kind);

/// Severity of a finding marker, mapped by the host onto its own palette.
enum class OverlaySeverity
{
    None,
    Info,
    Warning,
    Error
};

const char* getOverlaySeverityName(OverlaySeverity severity);

/// One thing to draw over the page pixels.
///
/// Geometry is in page space, never in viewport pixels. Storing viewport
/// coordinates would freeze the frame to one scroll offset and make a pan
/// require a rebuild; the host applies the page-to-viewport matrix the page
/// surfaces already use, so overlays and pixels cannot drift apart.
struct OverlayPrimitive
{
    /// Stable across frames for the same thing: the finding id, the box name,
    /// the handle name. A host keys retained scene-graph nodes on it, so a
    /// regenerated id every frame would rebuild the whole overlay each time.
    QString id;

    OverlayLayer layer = OverlayLayer::Findings;
    OverlayPrimitiveKind kind = OverlayPrimitiveKind::Rectangle;
    OverlaySeverity severity = OverlaySeverity::None;

    int pageIndex = -1;
    QRectF pageBounds;
    QPolygonF pagePolygon;

    /// The thing this primitive represents, so a host can route a click back
    /// without a second lookup table.
    InteractionTarget target;

    bool hovered = false;
    bool selected = false;

    /// Carries the keyboard focus ring. Separate from `selected` because focus
    /// and selection diverge under keyboard navigation, and issue #143 AC7
    /// requires the focused state be visible to a test.
    bool focused = false;

    /// False when the source geometry was missing or degenerate. The primitive
    /// is still emitted, so a caller can count and report what could not be
    /// drawn instead of silently losing it, and a host skips it (issue #143
    /// AC6).
    bool renderable = true;

    /// Ordinal within the layer, assigned by the builder. Breaks ties between
    /// two primitives that would otherwise sort equal.
    quint32 sequence = 0;

    bool operator==(const OverlayPrimitive& other) const = default;
};

/// Everything to draw over the page pixels, as one immutable value.
///
/// Replaced wholesale, exactly as CanvasSnapshot is, so a scene-graph thread
/// reading it never sees half an update. Its invalidation is independent of the
/// page surface's: a hover change produces a new OverlayFrame and no
/// PageSurfaceRequest at all, which is the P4-S4 exit condition.
struct OverlayFrame
{
    /// The document state and demand generation this frame was built for. A
    /// frame whose token no longer matches is refused rather than drawn, the
    /// same rule PageSurfaceCoordinator applies to a tile.
    RevisionFencedToken token;

    QList<OverlayPrimitive> primitives;

    /// Primitives the builder dropped because the bound was reached. Reported
    /// rather than silently discarded: telemetry that is missing must not read
    /// as zero.
    int droppedPrimitives = 0;

    /// Primitives emitted with renderable == false.
    int unrenderablePrimitives = 0;

    bool isEmpty() const { return primitives.isEmpty(); }

    /// Primitives on one page, in paint order.
    QList<OverlayPrimitive> primitivesForPage(int pageIndex) const;

    /// True when the frame is in paint order: layer, then sequence. Cheap enough
    /// to assert in a test on every built frame.
    bool isOrdered() const;
};

/// The paint-order comparison, exposed so a test pins it rather than
/// re-deriving it.
bool overlayPaintsBefore(const OverlayPrimitive& left, const OverlayPrimitive& right);

}   // namespace pdfinteraction

#endif   // OVERLAYFRAME_H
