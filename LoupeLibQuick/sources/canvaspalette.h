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


#ifndef CANVASPALETTE_H
#define CANVASPALETTE_H

#include "loupequickglobal.h"

#include "overlayframe.h"

#include <QColor>

namespace pdfquick
{

/// Visual treatment for one overlay primitive, in viewport pixels.
///
/// The neutral layer names what a primitive *is* -- a finding at Error severity,
/// a selected handle -- and never a colour or a pen width, because it cannot see
/// a screen. This struct is the other half of that split: it is the only place
/// that turns those names into something drawable.
struct OverlayStyle
{
    QColor stroke;

    /// Fully transparent for an outline-only primitive. Alpha is used rather
    /// than a separate `filled` flag so the node builder has one code path.
    QColor fill;

    float strokeWidthPx = 1.0F;

    /// Half-extent of a Marker or Handle. Markers and handles have no page-space
    /// size of their own -- they are points -- so their size is presentation.
    float pointRadiusPx = 4.0F;

    /// Drawn in addition to the stroke, outside it, when the primitive carries
    /// keyboard focus. Focus and selection are separate states in OverlayFrame
    /// and stay separate here.
    bool focusRing = false;
    float focusRingWidthPx = 2.0F;
    float focusRingOffsetPx = 2.0F;
};

/// The canvas half of the provisional Quick design tokens.
///
/// Values mirror docs/quick-design-tokens.json, which
/// scripts/verify-quick-shell-policy.py checks for contrast. They are duplicated
/// here as constants rather than parsed at runtime because the canvas must draw
/// before any file is read, and a canvas that renders nothing when a token file
/// is missing is worse than one that renders the admitted defaults.
///
/// Two rules from that contract are structural, not cosmetic, and a change here
/// must keep both:
///
///   `must_not_depend_on_color_alone` -- severity changes stroke width as well
///   as hue, so the four severities stay distinguishable in a monochrome capture
///   and to a red-green colour-blind reader.
///
///   `must_preserve_focus_indicator` -- the focus ring survives high contrast.
///   It is the one treatment high contrast widens rather than flattens.
class LOUPELIBQUICK_EXPORT CanvasPalette
{
public:
    /// The default treatment, on the tokens' dark surface.
    static CanvasPalette standard();

    /// Pure-white strokes on pure black, every width at least 2px. Used when the
    /// platform reports a high-contrast preference.
    static CanvasPalette highContrast();

    bool isHighContrast() const noexcept { return m_highContrast; }

    OverlayStyle styleFor(const pdfinteraction::OverlayPrimitive& primitive) const;

    /// Behind the page pixels. A page that has not arrived yet shows this rather
    /// than whatever the compositor last left in the buffer.
    QColor canvasBackground() const noexcept { return m_background; }

    /// The unrendered page rectangle, drawn where a tile is expected but absent.
    QColor pageBackground() const noexcept { return m_surface; }

    QColor hudText() const noexcept { return m_text; }
    QColor hudMutedText() const noexcept { return m_mutedText; }
    QColor hudBackground() const;

    /// Severity colour, exposed so the trace overlay can label a count in the
    /// same hue the primitives use.
    QColor severityColor(pdfinteraction::OverlaySeverity severity) const;

private:
    QColor m_background;
    QColor m_surface;
    QColor m_text;
    QColor m_mutedText;
    QColor m_accent;
    QColor m_focus;
    QColor m_danger;
    QColor m_success;
    bool m_highContrast = false;
};

}   // namespace pdfquick

#endif   // CANVASPALETTE_H
