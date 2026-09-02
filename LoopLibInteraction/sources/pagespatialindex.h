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

#ifndef PAGESPATIALINDEX_H
#define PAGESPATIALINDEX_H

#include "interactionglobal.h"

#include <QHash>
#include <QList>
#include <QPointF>
#include <QRectF>

namespace pdfinteraction
{

/// A uniform-grid spatial index over page-space rectangles, for one page.
///
/// Issue #145 requires that hit testing query candidates in document space
/// rather than scanning every object on every pointer move. A grid is chosen
/// over an R-tree/quadtree because interaction geometry (findings, page
/// boxes) is inserted once per revision and never restructured incrementally
/// -- there is no rebalancing to justify a tree's complexity, and a grid's
/// query cost is a single bucket lookup rather than a descent.
///
/// The index answers "which items might contain this point" by cell
/// membership only. A rectangle can span several cells, so a caller still
/// applies a precise geometry test to whatever `query()` returns -- the
/// index narrows candidates, it does not replace the exact test.
class PageSpatialIndex
{
public:
    PageSpatialIndex() = default;

    /// Discards any built index.
    void clear();

    /// Builds the grid from `bounds`, where `bounds[i]` is the page-space
    /// rectangle for item index `i`. Item indices are the caller's own --
    /// this class stores no payload, only which indices land in which cell,
    /// so the caller's item list and this index must be rebuilt together.
    ///
    /// A degenerate overall extent (no items, or every item collapsing to
    /// the same point/zero-size rect) falls back to one cell covering
    /// everything: every item is then returned as a candidate for any point
    /// inside that extent, which is correct -- just not narrowed. That is
    /// preferable to guessing a query rectangle no item can actually match.
    void build(const QList<QRectF>& bounds);

    /// Item indices whose rectangle's cell footprint includes `point`'s
    /// cell. Never a false negative: any item whose rectangle contains
    /// `point` necessarily shares at least one cell with `point`. May
    /// contain false positives (an item overlapping the cell without
    /// containing the point), which the caller filters with an exact test.
    ///
    /// Returns an empty list without touching any cell when `point` falls
    /// outside the indexed extent -- no item's rectangle extends beyond the
    /// extent that was built from those same rectangles.
    QList<int> query(QPointF point) const;

    int itemCount() const noexcept { return m_itemCount; }
    int cellCount() const noexcept { return m_columns * m_rows; }

private:
    int columnForX(qreal x) const;
    int rowForY(qreal y) const;

    QRectF m_extent;
    qreal m_cellWidth = 0.0;
    qreal m_cellHeight = 0.0;
    int m_columns = 1;
    int m_rows = 1;
    int m_itemCount = 0;
    QHash<qint64, QList<int>> m_cells;
    QList<int> m_overflowItems;
};

}   // namespace pdfinteraction

#endif   // PAGESPATIALINDEX_H
