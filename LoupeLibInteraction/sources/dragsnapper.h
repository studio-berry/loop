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

#ifndef DRAGSNAPPER_H
#define DRAGSNAPPER_H

#include "interactionglobal.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace pdfinteraction
{

class PageBoxHitTestSource;

/// One place a drag could come to rest.
struct SnapCandidate
{
    /// In the page's own space, like every other geometry in this layer.
    QPointF pagePoint;

    /// What produced it, for the trace and for a future UI that wants to say
    /// which guide a drag latched onto. Never document text.
    QString sourceId;
};

/// One domain's answer to "what is worth snapping to near here".
///
/// Shaped like IHitTestSource on purpose: asked about one page and one probe
/// rectangle, returns candidates in no particular order, and does not rank.
/// A provider cannot see the viewport and never converts a screen threshold.
class ISnapProvider
{
public:
    virtual ~ISnapProvider() = default;

    virtual QList<SnapCandidate> snapCandidates(int pageIndex, const QRectF& probePageRect) const = 0;
};

/// Pulls a drag preview onto a nearby candidate.
///
/// `pdf::PDFSnapper` already exists in Core and is deliberately not wrapped: it
/// is Widgets-era, shaped around the old viewer's input, and reusing it would
/// drag that shape into a layer that has no host. This is the neutral seam
/// instead, and `PageBoxSnapProvider` is the one provider that ships with it.
/// Object edges, text baselines, rulers and user guides are further providers,
/// not changes here.
///
/// The threshold is a screen quantity converted per call by the same rule as
/// HitTestDispatcher's tolerance -- a snap that got easier as the user zoomed
/// out would fight them at exactly the zoom where precision matters least.
class DragSnapper
{
public:
    /// Snap radius in screen pixels.
    static constexpr qreal DefaultScreenThresholdPx = 8.0;

    /// Providers are observed, not owned, and must outlive the snapper.
    void addProvider(ISnapProvider* provider);
    void clearProviders();

    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return m_enabled; }

    void setScreenThresholdPx(qreal pixels);
    qreal screenThresholdPx() const noexcept { return m_screenThresholdPx; }

    /// The nearest candidate within threshold, or `pagePoint` unchanged.
    ///
    /// `snappedTo` receives the winning candidate's source id, or an empty
    /// string when nothing was near enough. A caller distinguishes "snapped to
    /// a point that happens to equal the pointer" from "did not snap" by that
    /// string rather than by comparing points.
    /// `viewScale` is screen pixels per page unit -- display density and zoom
    /// together, the same quantity HitTestDispatcher::setViewScale takes.
    QPointF snap(int pageIndex, QPointF pagePoint, qreal viewScale, QString* snappedTo = nullptr) const;

private:
    qreal pageThreshold(qreal viewScale) const;

    QList<ISnapProvider*> m_providers;
    bool m_enabled = true;
    qreal m_screenThresholdPx = DefaultScreenThresholdPx;
};

/// Snaps to the corners of the media, crop, bleed, trim and art boxes.
///
/// Corners only. A page box's edges are what an operator aligns to, but an edge
/// is a line and a point-snap onto a line needs a projection rule that differs
/// per axis; doing it properly is an edge-alignment provider of its own. A
/// corner is unambiguous, and it is the case that matters for placing content
/// inside a trim box.
///
/// Boxes come from PageBoxHitTestSource, which already caches them per revision,
/// so this adds no second read of pdf::PDFPage and cannot disagree with what is
/// hittable.
class PageBoxSnapProvider final : public ISnapProvider
{
public:
    explicit PageBoxSnapProvider(const PageBoxHitTestSource* source);

    QList<SnapCandidate> snapCandidates(int pageIndex, const QRectF& probePageRect) const override;

private:
    const PageBoxHitTestSource* m_source = nullptr;
};

}   // namespace pdfinteraction

#endif   // DRAGSNAPPER_H
