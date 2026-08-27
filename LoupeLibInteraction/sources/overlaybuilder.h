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

#ifndef OVERLAYBUILDER_H
#define OVERLAYBUILDER_H

#include "interactionglobal.h"
#include "interactionstate.h"
#include "overlayframe.h"
#include "viewportcontroller.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

namespace pdfinteraction
{

/// Hard limits on one frame, pre-registered rather than discovered under load,
/// as PageSurfaceBounds is.
struct OverlayBounds
{
    /// Total primitives in a frame. A corpus with tens of thousands of findings
    /// must produce a bounded frame, not a frame that takes a second to walk.
    int maxPrimitives = 4000;

    /// Of those, how many may be findings. Selection, handles and tool previews
    /// are what the user is steering; they must not be crowded out by markers.
    int maxFindingPrimitives = 3000;

    static OverlayBounds conservativeDefaults() { return OverlayBounds(); }
};

/// Builds an immutable OverlayFrame from transient state and marker inputs.
///
/// The builder reads the viewport but never writes it. That is the whole point:
/// everything here is a consequence of a hover, a selection or a drag preview,
/// and none of it may advance ViewportController::requestGeneration() or reach
/// PageSurfaceCoordinator. An overlay change costs an overlay frame; it never
/// costs a page render.
///
/// Geometry stays in page space. Clipping is decided against the page rectangle
/// the viewport currently places, using ViewportController's own transforms, so
/// overlays and page pixels are aligned by construction through pan, zoom,
/// rotation and page change rather than by two implementations agreeing.
class OverlayBuilder final
{
public:
    explicit OverlayBuilder(const ViewportController& viewport, OverlayBounds bounds = OverlayBounds::conservativeDefaults());

    const OverlayBounds& bounds() const noexcept { return m_bounds; }

    /// Findings to mark, as targets carrying page-space geometry -- the same
    /// values EvidenceHitTestSource::targetsForPage returns, so a marker and its
    /// hit region cannot disagree.
    void setFindings(QList<InteractionTarget> findings);
    const QList<InteractionTarget>& findings() const noexcept { return m_findings; }

    /// Severity per finding id. A finding with no entry draws as Info; the
    /// builder does not invent severities it was not told.
    void setSeverities(QHash<QString, OverlaySeverity> severities);

    /// Page boxes, guides and rulers, drawn under the findings.
    void setGuides(QList<InteractionTarget> guides);

    /// Manipulators for the current selection.
    void setHandles(QList<InteractionTarget> handles);

    /// Findings hidden by the operator. Hiding is presentation state and never
    /// touches the document (issue #143 AC4).
    void setHiddenFindingIds(QSet<QString> hiddenIds);

    /// When true, suppress tool/findings overlays (PDFRenderer::DenyExtraGraphics parity).
    void setDenyExtraGraphics(bool deny) noexcept { m_denyExtraGraphics = deny; }
    bool denyExtraGraphics() const noexcept { return m_denyExtraGraphics; }

    /// The finding that currently has keyboard focus, drawn with the focus ring.
    void setFocusedId(QString focusedId);

    /// Builds the frame for `state` under `token`. Returns an empty frame when
    /// the token is invalid: a frame for a document state nobody holds is not
    /// worth drawing, and drawing it is how a stale marker survives a reload.
    OverlayFrame build(const InteractionState& state, const RevisionFencedToken& token) const;

private:
    /// Page-space rectangle the viewport currently shows for `pageIndex`, or a
    /// null rect when the page is not placed. Off-page geometry is clipped
    /// against this, so a marker outside the page never paints over its
    /// neighbour.
    QRectF visiblePageBounds(int pageIndex) const;

    const ViewportController* m_viewport = nullptr;
    OverlayBounds m_bounds;

    QList<InteractionTarget> m_findings;
    QList<InteractionTarget> m_guides;
    QList<InteractionTarget> m_handles;
    QHash<QString, OverlaySeverity> m_severities;
    QSet<QString> m_hiddenFindingIds;
    QString m_focusedId;
    bool m_denyExtraGraphics = false;
};

}   // namespace pdfinteraction

#endif   // OVERLAYBUILDER_H
