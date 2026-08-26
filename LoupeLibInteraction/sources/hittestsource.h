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

#ifndef HITTESTSOURCE_H
#define HITTESTSOURCE_H

#include "interactionglobal.h"
#include "interactiontarget.h"

#include "pdfdocumentcontext.h"
#include "pdfevidencegraph.h"

#include <QList>
#include <QHash>
#include <QPointF>
#include <QPointer>

namespace pdfinteraction
{

/// One domain's answer to "what is under this page point".
///
/// Deliberately narrow. A source is asked about one page and one point and
/// returns candidates; it does not rank against other domains, does not know
/// what is selected, and cannot see the viewport. Ranking lives in
/// HitTestDispatcher so precedence is decided in one place rather than emerging
/// from whichever source happened to be asked first.
class IHitTestSource
{
public:
    virtual ~IHitTestSource() = default;

    /// Candidates whose geometry contains `pagePoint`, in any order. Returning
    /// them unordered is intentional: an implementation that pre-sorts invites
    /// callers to depend on its order, and the dispatcher's tie-break is the
    /// only ordering the contract guarantees.
    virtual QList<InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const = 0;
};

/// Hit-tests preflight and evidence geometry.
///
/// pdf::PDFEvidenceRecord already carries a stable `id`, a 1-based `page`, and a
/// page-space `geometry` rectangle. Reusing it is what makes the finding a user
/// clicks on the canvas the same identity P4-S8's findings model lists and
/// P4-S9's inspector resolves; a parallel id assigned here would be a second
/// selection truth by another name.
///
/// The graph is held by value, not by reference to a live collector. An
/// evidence graph is a revision-bound result, so a copy taken with its revision
/// is the correct thing to hit-test against; re-reading a mutable one mid-drag
/// would let the geometry move under the gesture.
class EvidenceHitTestSource final : public IHitTestSource
{
public:
    EvidenceHitTestSource() = default;
    explicit EvidenceHitTestSource(pdf::PDFEvidenceGraph graph);

    /// Replaces the graph. The revision it was collected against is the graph's
    /// own; callers fence on it rather than on anything stored here.
    void setGraph(pdf::PDFEvidenceGraph graph);
    const pdf::PDFEvidenceGraph& graph() const noexcept { return m_graph; }

    /// Records with no usable geometry, which cannot be hit or drawn.
    int unrenderableRecordCount() const noexcept { return m_unrenderableRecords; }

    QList<InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const override;

    /// Every renderable record on a page, for the overlay pass. Same geometry
    /// and same ids as hitTest, so a marker cannot be drawn where nothing is
    /// hittable or the reverse.
    QList<InteractionTarget> targetsForPage(int pageIndex) const;

private:
    void indexGraph();

    pdf::PDFEvidenceGraph m_graph;
    QList<InteractionTarget> m_targets;
    int m_unrenderableRecords = 0;
};

/// Hit-tests finding geometry supplied as interaction targets (for example from
/// PreflightFindingsModel::interactionTargets()).
class FindingListHitTestSource final : public IHitTestSource
{
public:
    void setTargets(QList<InteractionTarget> targets);

    QList<InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const override;

private:
    QList<InteractionTarget> m_targets;
};

/// Hit-tests the page boxes: media, crop, bleed, trim, art.
///
/// Boxes come from pdf::PDFPage, which already owns them, through the observed
/// pdf::PDFDocumentContext. A destroyed context degrades to no candidates rather
/// than a dangling read, the rule PDFDocumentContextSource and
/// PDFDocumentPageGeometrySource both follow.
///
/// Only the box edges are hittable, not their interiors. A crop box covers most
/// of the page, so an interior hit would shadow every finding inside it.
class PageBoxHitTestSource final : public IHitTestSource
{
public:
    explicit PageBoxHitTestSource(pdf::PDFDocumentContext* context);

    /// Edge tolerance in page units. Set from the viewport by the caller, which
    /// is the only thing that knows the current zoom.
    void setEdgeTolerance(qreal tolerance);
    qreal edgeTolerance() const noexcept { return m_edgeTolerance; }

    QList<InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const override;

    /// Every page box on a page, for the overlay pass.
    QList<InteractionTarget> targetsForPage(int pageIndex) const;

private:
    QPointer<pdf::PDFDocumentContext> m_context;
    qreal m_edgeTolerance = 1.0;
    mutable pdf::PDFRevisionIdentity m_cachedRevision;
    mutable QHash<int, QList<InteractionTarget>> m_pageTargets;
};

/// Ranks candidates from every source into one answer.
///
/// Precedence, highest first:
///
///   1. drag handles, so a manipulator is never shadowed by the object under it;
///   2. the current selection, so a selected object stays grabbable beneath an
///      overlapping one;
///   3. InteractionTargetKind's declaration order -- finding, guide, page box,
///      page;
///   4. smallest page-space area, so a marker nested inside a larger one is
///      reachable;
///   5. stable id.
///
/// Rules 4 and 5 exist so the answer cannot depend on which source was
/// registered first or on how a container happened to be ordered. Issue #143 AC3
/// requires that determinism, and a test asserts it by reordering the sources.
class HitTestDispatcher final
{
public:
    /// Sources are observed, not owned, and must outlive the dispatcher.
    void addSource(IHitTestSource* source);
    void clearSources();

    /// The selection is ranked above its peers when it is hit again. Its kind is
    /// left alone: a target that changed identity on being selected could not be
    /// compared with the one the caller is holding.
    void setSelection(const InteractionTarget& target);

    /// Handles are supplied by the caller rather than discovered: only the host
    /// knows where it drew them, and they exist for one selection at a time.
    void setHandles(QList<InteractionTarget> handles);
    const QList<InteractionTarget>& handles() const noexcept { return m_handles; }

    /// Every candidate, ranked. Empty when nothing is hit.
    QList<InteractionTarget> hitTestAll(int pageIndex, QPointF pagePoint) const;

    /// The winner, or a Page target when only the page itself is under the
    /// point. `pageIndex` of -1 yields an invalid target.
    InteractionTarget hitTest(int pageIndex, QPointF pagePoint) const;

    /// Rules 3 to 5 above, exposed so a test pins them rather than re-deriving
    /// them from the implementation. The handle and selection rules are applied
    /// by hitTestAll, which is the only place that knows what is selected.
    static bool ranksBefore(const InteractionTarget& left, const InteractionTarget& right);

    /// Whether two targets name the same thing: same page, same id. Used for the
    /// selection rule, and by callers that must not compare whole structs whose
    /// geometry may have been clipped.
    static bool isSameTarget(const InteractionTarget& left, const InteractionTarget& right);

private:
    QList<IHitTestSource*> m_sources;
    InteractionTarget m_selection;
    QList<InteractionTarget> m_handles;
};

}   // namespace pdfinteraction

#endif   // HITTESTSOURCE_H
