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

#include "pagespatialindex.h"

#include <cmath>

namespace pdfinteraction
{

namespace
{

constexpr int MaxGridDimension = 128;

qint64 cellKey(int column, int row)
{
    return (qint64(column) << 32) | quint32(row);
}

}   // namespace

void PageSpatialIndex::clear()
{
    m_extent = QRectF();
    m_cellWidth = 0.0;
    m_cellHeight = 0.0;
    m_columns = 1;
    m_rows = 1;
    m_itemCount = 0;
    m_cells.clear();
}

void PageSpatialIndex::build(const QList<QRectF>& bounds)
{
    clear();
    m_itemCount = bounds.size();

    if (bounds.isEmpty())
    {
        return;
    }

    QRectF extent;
    for (const QRectF& box : bounds)
    {
        extent = extent.isNull() ? box.normalized() : extent.united(box.normalized());
    }
    m_extent = extent;

    // Aim for roughly one item per cell so a query touches a small, mostly
    // constant number of candidates regardless of document size; capped so a
    // pathological single-axis document (a very tall, one-column page of
    // stacked findings) cannot allocate an unbounded number of cells.
    const int target = qMax(1, int(std::ceil(std::sqrt(double(m_itemCount)))));
    m_columns = qBound(1, target, MaxGridDimension);
    m_rows = qBound(1, target, MaxGridDimension);

    m_cellWidth = m_extent.width() / m_columns;
    m_cellHeight = m_extent.height() / m_rows;

    // A zero-width or zero-height extent (every item collapses onto a line or
    // point) cannot be divided into columns/rows; fall back to one cell on
    // that axis so every item still indexes instead of being dropped.
    if (!(m_cellWidth > 0.0))
    {
        m_columns = 1;
        m_cellWidth = qMax(m_extent.width(), 1.0);
    }
    if (!(m_cellHeight > 0.0))
    {
        m_rows = 1;
        m_cellHeight = qMax(m_extent.height(), 1.0);
    }

    for (int index = 0; index < bounds.size(); ++index)
    {
        const QRectF box = bounds.at(index).normalized();

        const int columnBegin = columnForX(box.left());
        const int columnEnd = columnForX(box.right());
        const int rowBegin = rowForY(box.top());
        const int rowEnd = rowForY(box.bottom());

        for (int column = columnBegin; column <= columnEnd; ++column)
        {
            for (int row = rowBegin; row <= rowEnd; ++row)
            {
                m_cells[cellKey(column, row)].push_back(index);
            }
        }
    }
}

int PageSpatialIndex::columnForX(qreal x) const
{
    if (m_cellWidth <= 0.0)
    {
        return 0;
    }
    return qBound(0, int((x - m_extent.left()) / m_cellWidth), m_columns - 1);
}

int PageSpatialIndex::rowForY(qreal y) const
{
    if (m_cellHeight <= 0.0)
    {
        return 0;
    }
    return qBound(0, int((y - m_extent.top()) / m_cellHeight), m_rows - 1);
}

QList<int> PageSpatialIndex::query(QPointF point) const
{
    if (m_itemCount == 0)
    {
        return {};
    }

    // No indexed rectangle extends past the extent they were all built from,
    // so a point outside it cannot be contained by anything -- reject before
    // touching a single cell. Compared against the extent's numeric bounds
    // directly (not QRectF::contains, whose edge-open semantics degrade for
    // a zero-width/height extent) so this can only ever be a superset of
    // what any individual item's own precise contains() test would accept --
    // an early-out this permissive can produce a false positive (caught by
    // the caller's precise test) but never a false negative.
    if (point.x() < m_extent.left() || point.x() > m_extent.right() || point.y() < m_extent.top() ||
        point.y() > m_extent.bottom())
    {
        return {};
    }

    const qint64 key = cellKey(columnForX(point.x()), rowForY(point.y()));
    return m_cells.value(key);
}

}   // namespace pdfinteraction
