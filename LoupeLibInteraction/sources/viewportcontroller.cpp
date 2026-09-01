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

#include "viewportcontroller.h"

#include "pdfdocument.h"
#include "pdfrenderer.h"

#include <QtGlobal>

#include <algorithm>

namespace pdfinteraction
{

namespace
{

/// Marks a slot in a left/right page pair that no page occupies.
constexpr int NoPage = -1;

/// Keep a 5% margin around a fitted page so fit behavior stays stable across
/// presentation hosts.
constexpr qreal FitMarginRatio = 0.95;

/// Pages after the last visible one that are still worth rendering. Matches the
/// look-ahead PDFDrawWidgetProxy::getActivePages already uses.
constexpr int PrefetchLookAhead = 2;

}   // namespace

const char* getPageLayoutName(PageLayout layout)
{
    switch (layout)
    {
        case PageLayout::SinglePage:
            return "single-page";
        case PageLayout::OneColumn:
            return "one-column";
        case PageLayout::TwoPagesLeft:
            return "two-pages-left";
        case PageLayout::TwoPagesRight:
            return "two-pages-right";
        case PageLayout::TwoColumnLeft:
            return "two-column-left";
        case PageLayout::TwoColumnRight:
            return "two-column-right";
    }

    return "one-column";
}

PDFDocumentPageGeometrySource::PDFDocumentPageGeometrySource(pdf::PDFDocumentContext* context) :
    m_context(context)
{
}

const pdf::PDFDocument* PDFDocumentPageGeometrySource::document() const
{
    return m_context ? m_context->getDocument() : nullptr;
}

int PDFDocumentPageGeometrySource::pageCount() const
{
    const pdf::PDFDocument* document = this->document();
    return document ? static_cast<int>(document->getCatalog()->getPageCount()) : 0;
}

QSizeF PDFDocumentPageGeometrySource::pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const
{
    const pdf::PDFDocument* document = this->document();
    if (!document || pageIndex < 0 || pageIndex >= pageCount())
    {
        return QSizeF();
    }

    const pdf::PDFPage* page = document->getCatalog()->getPage(static_cast<size_t>(pageIndex));
    if (!page)
    {
        return QSizeF();
    }

    // getRotatedMediaBoxMM() applies the page's own /Rotate; getRotatedBox then
    // applies the viewer rotation on top. Same composition the Widgets draw-space
    // controller performs.
    return pdf::PDFPage::getRotatedBox(page->getRotatedMediaBoxMM(), extraRotation).size();
}

QTransform PDFDocumentPageGeometrySource::pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const
{
    const pdf::PDFDocument* document = this->document();
    if (!document || pageIndex < 0 || pageIndex >= pageCount())
    {
        return QTransform();
    }

    const pdf::PDFPage* page = document->getCatalog()->getPage(static_cast<size_t>(pageIndex));
    if (!page)
    {
        return QTransform();
    }

    // Core's overload takes the extra rotation directly and combines it with the
    // page rotation before deriving the media box. PDFDrawWidgetProxy predates
    // that parameter and hand-composes an equivalent device-space transform; the
    // Core path is the maintained one and PDFThumbnailsRenderer already uses it.
    return pdf::PDFRenderer::createPagePointToDevicePointMatrix(page, deviceRect, extraRotation);
}

ViewportController::ViewportController(QObject* parent) :
    QObject(parent)
{
}

ViewportController::~ViewportController() = default;

void ViewportController::setGeometrySource(IPageGeometrySource* source)
{
    if (m_geometry == source)
    {
        return;
    }

    m_geometry = source;
    m_blockIndex = -1;
    rebuildLayout();
    supersedeDemand();
}

void ViewportController::setPageLayout(PageLayout layout)
{
    if (m_pageLayout == layout)
    {
        return;
    }

    m_pageLayout = layout;
    m_blockIndex = -1;
    rebuildLayout();
    supersedeDemand();
}

void ViewportController::setRotation(pdf::PageRotation rotation)
{
    if (m_rotation == rotation)
    {
        return;
    }

    m_rotation = rotation;
    rebuildLayout();
    supersedeDemand();
}

void ViewportController::setZoom(qreal zoom, std::optional<QPointF> anchorPx)
{
    const qreal clampedZoom = qBound(MinimumZoom, zoom, MaximumZoom);
    if (qFuzzyCompare(m_zoom, clampedZoom))
    {
        return;
    }

    if (anchorPx.has_value())
    {
        // Keep the draw-space point under the anchor still: convert the current
        // offsets to millimetres relative to the anchor, change the scale, then
        // convert back. Ported from PDFDrawWidgetProxy::zoomImpl.
        const QPointF anchor = anchorPx.value();
        const qreal pixelToDeviceSpaceUnit = 1.0 / deviceSpaceUnitToPixel();
        const qreal posX = (m_horizontalOffset - anchor.x()) * pixelToDeviceSpaceUnit;
        const qreal posY = (m_verticalOffset - anchor.y()) * pixelToDeviceSpaceUnit;

        m_zoom = clampedZoom;
        updatePlacements();

        setOffset(QPoint(qRound(posX * deviceSpaceUnitToPixel() + anchor.x()), qRound(posY * deviceSpaceUnitToPixel() + anchor.y())));
    }
    else
    {
        const qreal pixelToDeviceSpaceUnit = 1.0 / deviceSpaceUnitToPixel();
        const qreal horizontalOffsetMM = m_horizontalOffset * pixelToDeviceSpaceUnit;
        const qreal verticalOffsetMM = m_verticalOffset * pixelToDeviceSpaceUnit;

        m_zoom = clampedZoom;
        updatePlacements();

        setOffset(QPoint(qRound(horizontalOffsetMM * deviceSpaceUnitToPixel()), qRound(verticalOffsetMM * deviceSpaceUnitToPixel())));
    }

    supersedeDemand();
}

qreal ViewportController::zoomHint(ZoomHint hint, int pageIndex) const
{
    QSizeF referenceSizeMM;

    if (pageIndex < 0)
    {
        pageIndex = currentPage();
    }

    if (pageIndex < 0 && m_geometry && m_geometry->pageCount() > 0)
    {
        pageIndex = 0;
    }

    for (const LayoutItemMM& item : m_layoutItems)
    {
        if (item.pageIndex == pageIndex)
        {
            referenceSizeMM = item.pageRectMM.size();
            break;
        }
    }

    if (!referenceSizeMM.isValid() || referenceSizeMM.isEmpty() || m_viewportSizePx.isEmpty() || m_pixelPerMM <= 0.0)
    {
        return 1.0;
    }

    const qreal viewportWidthMM = (m_viewportSizePx.width() / m_pixelPerMM) * FitMarginRatio;
    const qreal viewportHeightMM = (m_viewportSizePx.height() / m_pixelPerMM) * FitMarginRatio;

    const qreal widthHint = viewportWidthMM / referenceSizeMM.width();
    const qreal heightHint = viewportHeightMM / referenceSizeMM.height();

    switch (hint)
    {
        case ZoomHint::Fit:
            return qBound(MinimumZoom, qMin(widthHint, heightHint), MaximumZoom);
        case ZoomHint::FitWidth:
            return qBound(MinimumZoom, widthHint, MaximumZoom);
        case ZoomHint::FitHeight:
            return qBound(MinimumZoom, heightHint, MaximumZoom);
    }

    return 1.0;
}

void ViewportController::setPixelPerMM(qreal pixelPerMM)
{
    if (!(pixelPerMM > 0.0) || qFuzzyCompare(m_pixelPerMM, pixelPerMM))
    {
        return;
    }

    m_pixelPerMM = pixelPerMM;
    updatePlacements();
    supersedeDemand();
}

void ViewportController::setDevicePixelRatio(qreal devicePixelRatio)
{
    if (!(devicePixelRatio > 0.0) || qFuzzyCompare(m_devicePixelRatio, devicePixelRatio))
    {
        return;
    }

    // The ratio changes no layout geometry at all -- it changes how many real
    // pixels a placed page needs, so every surface already rendered is the wrong
    // resolution.
    m_devicePixelRatio = devicePixelRatio;
    supersedeDemand();
}

void ViewportController::setViewportSizePx(QSize size)
{
    const QSize boundedSize = QSize(qMax(0, size.width()), qMax(0, size.height()));
    if (m_viewportSizePx == boundedSize)
    {
        return;
    }

    m_viewportSizePx = boundedSize;
    updatePlacements();
    supersedeDemand();
}

QRect ViewportController::viewportRect() const
{
    return QRect(QPoint(0, 0), m_viewportSizePx);
}

int ViewportController::blockCount() const
{
    return static_cast<int>(m_blockRectsMM.size());
}

void ViewportController::setBlockIndex(int blockIndex)
{
    const int count = blockCount();
    const int bounded = count > 0 ? qBound(0, blockIndex, count - 1) : -1;

    if (m_blockIndex == bounded)
    {
        return;
    }

    m_blockIndex = bounded;
    updatePlacements();
    supersedeDemand();
}

bool ViewportController::isBlockMode() const
{
    switch (m_pageLayout)
    {
        case PageLayout::OneColumn:
        case PageLayout::TwoColumnLeft:
        case PageLayout::TwoColumnRight:
            return false;

        case PageLayout::SinglePage:
        case PageLayout::TwoPagesLeft:
        case PageLayout::TwoPagesRight:
            return true;
    }

    return false;
}

QPoint ViewportController::offset() const
{
    return QPoint(m_horizontalOffset, m_verticalOffset);
}

void ViewportController::setOffset(QPoint offset)
{
    const int horizontal = qBound(m_minimumOffset.x(), offset.x(), m_maximumOffset.x());
    const int vertical = qBound(m_minimumOffset.y(), offset.y(), m_maximumOffset.y());

    if (horizontal == m_horizontalOffset && vertical == m_verticalOffset)
    {
        return;
    }

    m_horizontalOffset = horizontal;
    m_verticalOffset = vertical;

    // Panning does not supersede demand; see requestGeneration().
    Q_EMIT placementsChanged();
}

QPoint ViewportController::scrollByPixels(QPoint offset)
{
    const QPoint before(m_horizontalOffset, m_verticalOffset);
    setOffset(before + offset);
    return QPoint(m_horizontalOffset, m_verticalOffset) - before;
}

QPoint ViewportController::minimumOffset() const
{
    return m_minimumOffset;
}

QPoint ViewportController::maximumOffset() const
{
    return m_maximumOffset;
}

QRect ViewportController::toViewport(const ViewportPlacement& placement) const
{
    // The offsets are relative to the top-left of the block, and the block does
    // not necessarily start at the origin, so both translations are needed.
    return placement.placedRect.translated(m_horizontalOffset - m_blockRectPx.left(), m_verticalOffset - m_blockRectPx.top());
}

QRect ViewportController::placedPageRect(int pageIndex) const
{
    for (const ViewportPlacement& placement : m_placements)
    {
        if (placement.pageIndex == pageIndex)
        {
            return toViewport(placement);
        }
    }

    return QRect();
}

QList<int> ViewportController::visiblePagesFor(const QList<ViewportPlacement>& placements, QRect viewportRect)
{
    QList<int> pages;
    pages.reserve(placements.size());

    for (const ViewportPlacement& placement : placements)
    {
        if (placement.placedRect.intersects(viewportRect))
        {
            pages.push_back(placement.pageIndex);
        }
    }

    std::sort(pages.begin(), pages.end());
    return pages;
}

QList<int> ViewportController::visiblePages() const
{
    QList<ViewportPlacement> placed;
    placed.reserve(m_placements.size());

    for (const ViewportPlacement& placement : m_placements)
    {
        placed.push_back(ViewportPlacement{ placement.pageIndex, toViewport(placement) });
    }

    return visiblePagesFor(placed, viewportRect());
}

QList<int> ViewportController::activePages() const
{
    QList<int> pages = visiblePages();

    if (!pages.isEmpty() && m_geometry)
    {
        const int pageCount = m_geometry->pageCount();
        const int last = pages.back();
        const int end = qMin(pageCount, last + 1 + PrefetchLookAhead);

        for (int i = last + 1; i < end; ++i)
        {
            pages.push_back(i);
        }
    }

    return pages;
}

int ViewportController::currentPage() const
{
    const QList<int> pages = visiblePages();
    return pages.isEmpty() ? -1 : pages.front();
}

int ViewportController::pageCount() const
{
    return m_geometry ? m_geometry->pageCount() : 0;
}

QTransform ViewportController::pagePointToViewportMatrix(int pageIndex) const
{
    const QRect placedRect = placedPageRect(pageIndex);
    if (!m_geometry || placedRect.isEmpty())
    {
        return QTransform();
    }

    return m_geometry->pagePointToDeviceMatrix(pageIndex, placedRect, m_rotation);
}

std::optional<QPointF> ViewportController::viewportToPagePoint(QPoint viewportPoint, int pageIndex) const
{
    const QTransform matrix = pagePointToViewportMatrix(pageIndex);

    bool invertible = false;
    const QTransform inverted = matrix.inverted(&invertible);
    if (!invertible)
    {
        return std::nullopt;
    }

    return inverted.map(QPointF(viewportPoint));
}

int ViewportController::pageUnderPoint(QPoint viewportPoint, QPointF* pagePoint) const
{
    for (const ViewportPlacement& placement : m_placements)
    {
        QRect placedRect = toViewport(placement);

        // The inclusive-edge fudge the Widgets hit test uses: a click on the
        // right or bottom edge belongs to the page, not to the gap after it.
        placedRect.adjust(0, 0, 1, 1);

        if (placedRect.contains(viewportPoint))
        {
            if (pagePoint)
            {
                const std::optional<QPointF> mapped = viewportToPagePoint(viewportPoint, placement.pageIndex);
                *pagePoint = mapped.value_or(QPointF());
            }

            return placement.pageIndex;
        }
    }

    return -1;
}

int ViewportController::blockIndexForPage(int pageIndex) const
{
    if (pageIndex < 0)
    {
        return 0;
    }

    for (const LayoutItemMM& item : m_layoutItems)
    {
        if (item.pageIndex == pageIndex)
        {
            return item.blockIndex;
        }
    }

    return pageIndex;
}

void ViewportController::invalidateLayout()
{
    rebuildLayout();
    supersedeDemand();
}

qreal ViewportController::deviceSpaceUnitToPixel() const noexcept
{
    return m_pixelPerMM * m_zoom;
}

QRectF ViewportController::fromDeviceSpace(const QRectF& rectMM) const
{
    const qreal scale = deviceSpaceUnitToPixel();
    return QRectF(rectMM.left() * scale, rectMM.top() * scale, rectMM.width() * scale, rectMM.height() * scale);
}

void ViewportController::placePagesLeftRight(int blockIndex, int leftPageIndex, int rightPageIndex, qreal& yPos, QRectF& boundingRect)
{
    qreal yPosAdvance = 0.0;

    if (leftPageIndex != NoPage)
    {
        const QSizeF pageSize = m_geometry->pageSizeMM(leftPageIndex, m_rotation);
        const QRectF rect(-pageSize.width() - m_horizontalSpacingMM * 0.5, yPos, pageSize.width(), pageSize.height());
        m_layoutItems.push_back(LayoutItemMM{ blockIndex, leftPageIndex, rect });
        yPosAdvance = qMax(yPosAdvance, pageSize.height());
        boundingRect = boundingRect.united(rect);
    }

    if (rightPageIndex != NoPage)
    {
        const QSizeF pageSize = m_geometry->pageSizeMM(rightPageIndex, m_rotation);
        const QRectF rect(m_horizontalSpacingMM * 0.5, yPos, pageSize.width(), pageSize.height());
        m_layoutItems.push_back(LayoutItemMM{ blockIndex, rightPageIndex, rect });
        yPosAdvance = qMax(yPosAdvance, pageSize.height());
        boundingRect = boundingRect.united(rect);
    }

    if (yPosAdvance > 0.0)
    {
        yPos += yPosAdvance + m_verticalSpacingMM;
    }
}

void ViewportController::placePagesLeftRightByIndices(const QList<int>& indices, bool generateBlocks)
{
    Q_ASSERT(indices.size() % 2 == 0);

    qreal yPos = 0.0;
    int blockIndex = 0;
    QRectF boundingRectangle;

    const qsizetype pairCount = indices.size() / 2;
    for (qsizetype i = 0; i < pairCount; ++i)
    {
        placePagesLeftRight(blockIndex, indices[2 * i], indices[2 * i + 1], yPos, boundingRectangle);

        if (generateBlocks)
        {
            m_blockRectsMM.push_back(boundingRectangle);
            yPos = 0.0;
            ++blockIndex;
            boundingRectangle = QRectF();
        }
    }

    if (!generateBlocks)
    {
        m_blockRectsMM.push_back(boundingRectangle);
    }
}

void ViewportController::rebuildLayout()
{
    m_layoutItems.clear();
    m_blockRectsMM.clear();

    const int pageCount = m_geometry ? m_geometry->pageCount() : 0;
    if (pageCount <= 0)
    {
        m_blockIndex = -1;
        updatePlacements();
        return;
    }

    switch (m_pageLayout)
    {
        case PageLayout::SinglePage:
        {
            // Pages can differ in size, so each is centred on its own block.
            for (int i = 0; i < pageCount; ++i)
            {
                const QSizeF pageSize = m_geometry->pageSizeMM(i, m_rotation);
                const QRectF rect(-pageSize.width() * 0.5, -pageSize.height() * 0.5, pageSize.width(), pageSize.height());
                m_layoutItems.push_back(LayoutItemMM{ i, i, rect });
                m_blockRectsMM.push_back(rect);
            }
            break;
        }

        case PageLayout::OneColumn:
        {
            qreal yPos = 0.0;
            QRectF boundingRectangle;

            for (int i = 0; i < pageCount; ++i)
            {
                const QSizeF pageSize = m_geometry->pageSizeMM(i, m_rotation);
                const QRectF rect(-pageSize.width() * 0.5, yPos, pageSize.width(), pageSize.height());
                m_layoutItems.push_back(LayoutItemMM{ 0, i, rect });
                yPos += pageSize.height() + m_verticalSpacingMM;
                boundingRectangle = boundingRectangle.united(rect);
            }

            m_blockRectsMM.push_back(boundingRectangle);
            break;
        }

        case PageLayout::TwoColumnLeft:
        case PageLayout::TwoPagesLeft:
        {
            // Even page indices go left of the y axis, odd ones right.
            QList<int> indices;
            indices.reserve(pageCount + 1);
            for (int i = 0; i < pageCount; ++i)
            {
                indices.push_back(i);
            }
            if (indices.size() % 2 == 1)
            {
                indices.push_back(NoPage);
            }

            placePagesLeftRightByIndices(indices, m_pageLayout == PageLayout::TwoPagesLeft);
            break;
        }

        case PageLayout::TwoColumnRight:
        case PageLayout::TwoPagesRight:
        {
            // Same, but the sequence starts on the right, so page 0 stands alone.
            QList<int> indices;
            indices.reserve(pageCount + 2);
            indices.push_back(NoPage);
            for (int i = 0; i < pageCount; ++i)
            {
                indices.push_back(i);
            }
            if (indices.size() % 2 == 1)
            {
                indices.push_back(NoPage);
            }

            placePagesLeftRightByIndices(indices, m_pageLayout == PageLayout::TwoPagesRight);
            break;
        }
    }

    if (m_blockIndex < 0 || m_blockIndex >= blockCount())
    {
        m_blockIndex = blockCount() > 0 ? 0 : -1;
    }

    updatePlacements();
}

void ViewportController::updatePlacements()
{
    m_placements.clear();
    m_blockRectPx = QRect();

    if (m_blockIndex >= 0 && m_blockIndex < blockCount())
    {
        const QRectF blockRectMM = m_blockRectsMM.at(m_blockIndex);
        if (blockRectMM.isValid())
        {
            m_blockRectPx = fromDeviceSpace(blockRectMM).toRect();
        }

        for (const LayoutItemMM& item : m_layoutItems)
        {
            if (item.blockIndex == m_blockIndex)
            {
                m_placements.push_back(ViewportPlacement{ item.pageIndex, fromDeviceSpace(item.pageRectMM).toRect() });
            }
        }
    }

    const QSize blockSize = m_blockRectPx.size();

    // A block smaller than the viewport is centred and pinned; a larger one
    // scrolls between the negative difference and zero. Same rule as the Widgets
    // proxy, minus the scrollbar bookkeeping.
    const int horizontalDifference = blockSize.width() - m_viewportSizePx.width();
    if (horizontalDifference > 0)
    {
        m_minimumOffset.setX(-horizontalDifference);
        m_maximumOffset.setX(0);
    }
    else
    {
        const int centred = -horizontalDifference / 2;
        m_minimumOffset.setX(centred);
        m_maximumOffset.setX(centred);
    }

    // In block mode the vertical axis also selects blocks, but a block taller
    // than the viewport still has to be scrollable within itself, so the range
    // is derived the same way in both modes.
    const int verticalDifference = blockSize.height() - m_viewportSizePx.height();
    if (verticalDifference > 0)
    {
        m_minimumOffset.setY(-verticalDifference);
        m_maximumOffset.setY(0);
    }
    else
    {
        const int centred = -verticalDifference / 2;
        m_minimumOffset.setY(centred);
        m_maximumOffset.setY(centred);
    }

    m_horizontalOffset = qBound(m_minimumOffset.x(), m_horizontalOffset, m_maximumOffset.x());
    m_verticalOffset = qBound(m_minimumOffset.y(), m_verticalOffset, m_maximumOffset.y());

    Q_EMIT placementsChanged();
}

void ViewportController::supersedeDemand()
{
    ++m_requestGeneration;
    Q_EMIT demandChanged(m_requestGeneration);
}

}   // namespace pdfinteraction
