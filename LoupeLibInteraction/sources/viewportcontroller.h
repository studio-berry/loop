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

#ifndef VIEWPORTCONTROLLER_H
#define VIEWPORTCONTROLLER_H

#include "interactionglobal.h"

#include "pdfdocumentcontext.h"
#include "pdfpage.h"

#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QTransform>

#include <optional>

namespace pdfinteraction
{

/// How pages are arranged in the draw space. The same set the Widgets draw-space
/// controller supports, minus Custom: no product surface sets a custom layout,
/// and its "rotation must be None" precondition is a trap to carry forward
/// unused.
enum class PageLayout
{
    SinglePage,
    OneColumn,
    TwoPagesLeft,
    TwoPagesRight,
    TwoColumnLeft,
    TwoColumnRight
};

enum class ZoomHint
{
    Fit,
    FitWidth,
    FitHeight
};

const char* getPageLayoutName(PageLayout layout);

/// The page geometry the viewport needs, and nothing else.
///
/// The controller asks questions about pages; it never holds a document. That is
/// not ceremony: a viewport that owns a pdf::PDFDocument* is a second document
/// truth with its own lifetime, and it makes every layout test need a real PDF.
class IPageGeometrySource
{
public:
    virtual ~IPageGeometrySource() = default;

    virtual int pageCount() const = 0;

    /// Laid-out page size in millimetres with the page's own rotation and the
    /// viewer's extra rotation both applied.
    virtual QSizeF pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const = 0;

    /// Maps page points onto the device rectangle the page is placed in.
    virtual QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const = 0;
};

/// Answers those questions from the document the context currently holds.
///
/// The context is observed through a QPointer, so a destroyed context degrades to
/// an empty document rather than a dangling read -- the same rule
/// PDFDocumentContextSource follows.
class PDFDocumentPageGeometrySource final : public IPageGeometrySource
{
public:
    explicit PDFDocumentPageGeometrySource(pdf::PDFDocumentContext* context);

    int pageCount() const override;
    QSizeF pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const override;
    QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const override;

private:
    const pdf::PDFDocument* document() const;

    QPointer<pdf::PDFDocumentContext> m_context;
};

/// One page placed in viewport pixels.
struct ViewportPlacement
{
    int pageIndex = -1;
    QRect placedRect;
};

/// Host-neutral viewport: layout, zoom, rotation, pan, and the transforms between
/// page space and viewport space.
///
/// Everything the Widgets proxy scrapes from its host is injected here instead.
/// setPixelPerMM() replaces QGuiApplication::primaryScreen(), which is wrong on a
/// second monitor and untestable anywhere; setViewportSizePx() replaces
/// QWidget::rect(); setDevicePixelRatio() replaces QWidget::devicePixelRatioF().
/// There is no scrollbar here either -- the controller publishes an offset and its
/// range, and the host binds a scrollbar or a Flickable to them.
///
/// The controller owns presentation state only. It never mutates the document,
/// and it renders nothing.
class ViewportController final : public QObject
{
    Q_OBJECT

public:
    explicit ViewportController(QObject* parent = nullptr);
    ~ViewportController() override;

    ViewportController(const ViewportController&) = delete;
    ViewportController& operator=(const ViewportController&) = delete;

    static constexpr qreal MinimumZoom = 8.0 / 100.0;
    static constexpr qreal MaximumZoom = 6400.0 / 100.0;
    static constexpr qreal ZoomStep = 1.2;

    /// The source is observed, not owned, and must outlive the controller.
    /// Passing nullptr is how a closed document is expressed.
    void setGeometrySource(IPageGeometrySource* source);

    void setPageLayout(PageLayout layout);
    PageLayout pageLayout() const noexcept { return m_pageLayout; }

    void setRotation(pdf::PageRotation rotation);
    pdf::PageRotation rotation() const noexcept { return m_rotation; }

    /// Sets the zoom, optionally keeping the point under `anchorPx` still.
    void setZoom(qreal zoom, std::optional<QPointF> anchorPx = std::nullopt);
    qreal zoom() const noexcept { return m_zoom; }

    /// The zoom that would satisfy `hint`. Returned rather than applied: a fit
    /// mode that silently re-applies itself on every layout change is how the
    /// Widgets proxy ends up recomputing zoom inside its own update pass.
    qreal zoomHint(ZoomHint hint, int pageIndex = -1) const;

    void setPixelPerMM(qreal pixelPerMM);
    qreal pixelPerMM() const noexcept { return m_pixelPerMM; }

    /// Screen pixels per page unit: `pixelPerMM() * zoom()`.
    ///
    /// The whole page-to-screen scale, which is what converts a screen-space
    /// hit tolerance or snap threshold into page units. The zoom alone is not
    /// that number -- it omits the display density, so dividing a pixel slack
    /// by it leaves the density factor in and overstates the reach.
    qreal pageUnitToPixel() const noexcept { return deviceSpaceUnitToPixel(); }

    void setDevicePixelRatio(qreal devicePixelRatio);
    qreal devicePixelRatio() const noexcept { return m_devicePixelRatio; }

    void setViewportSizePx(QSize size);
    QSize viewportSizePx() const noexcept { return m_viewportSizePx; }
    QRect viewportRect() const;

    int blockCount() const;
    int blockIndex() const noexcept { return m_blockIndex; }
    void setBlockIndex(int blockIndex);

    /// True when the vertical axis selects blocks rather than scrolling pixels.
    bool isBlockMode() const;

    QPoint offset() const;
    void setOffset(QPoint offset);

    /// Applies `offset` and returns the delta actually applied after clamping.
    QPoint scrollByPixels(QPoint offset);

    QPoint minimumOffset() const;
    QPoint maximumOffset() const;

    const QList<ViewportPlacement>& placements() const noexcept { return m_placements; }
    QRect placedPageRect(int pageIndex) const;

    QList<int> visiblePages() const;

    /// Visible pages plus the two-page look-ahead the Widgets path already
    /// prefetches. These are the pages worth a NearViewport render.
    QList<int> activePages() const;

    /// First visible page, or -1.
    int currentPage() const;

    /// Pages reported by the geometry source, or 0 when none is bound.
    int pageCount() const;

    QTransform pagePointToViewportMatrix(int pageIndex) const;
    std::optional<QPointF> viewportToPagePoint(QPoint viewportPoint, int pageIndex) const;

    /// The page under `viewportPoint`, or -1. Writes the page-space point when
    /// `pagePoint` is given.
    int pageUnderPoint(QPoint viewportPoint, QPointF* pagePoint = nullptr) const;

    /// Block that contains `pageIndex` in the current layout, or 0 when unknown.
    int blockIndexForPage(int pageIndex) const;

    /// Rebuilds the layout for a replaced document or a changed revision. This
    /// supersedes all prior surface demand.
    void invalidateLayout();

    /// Increments only when a change makes previously requested surfaces wrong.
    ///
    /// Panning deliberately does not increment it. A pan changes which pages are
    /// wanted, not what a wanted page should look like, and bumping the
    /// generation on every pointer delta would cancel the in-flight renders that
    /// issue #142 requires be reused during a drag.
    quint64 requestGeneration() const noexcept { return m_requestGeneration; }

    /// Pure projection, so the visible-set rule is pinned by a test rather than
    /// re-derived by every caller.
    static QList<int> visiblePagesFor(const QList<ViewportPlacement>& placements, QRect viewportRect);

signals:
    /// Prior surface demand is superseded; `generation` is the new value.
    void demandChanged(quint64 generation);

    /// Placements changed without superseding demand -- a pan, or a block switch
    /// that kept the same geometry.
    void placementsChanged();

private:
    struct LayoutItemMM
    {
        int blockIndex = 0;
        int pageIndex = -1;
        QRectF pageRectMM;
    };

    QRect toViewport(const ViewportPlacement& placement) const;
    void rebuildLayout();
    void updatePlacements();
    void supersedeDemand();
    qreal deviceSpaceUnitToPixel() const noexcept;
    QRectF fromDeviceSpace(const QRectF& rectMM) const;
    void placePagesLeftRight(int blockIndex, int leftPageIndex, int rightPageIndex, qreal& yPos, QRectF& boundingRect);
    void placePagesLeftRightByIndices(const QList<int>& indices, bool generateBlocks);

    IPageGeometrySource* m_geometry = nullptr;

    PageLayout m_pageLayout = PageLayout::OneColumn;
    pdf::PageRotation m_rotation = pdf::PageRotation::None;
    qreal m_zoom = 1.0;

    /// A sane default for a 96 DPI logical screen. Hosts are expected to set the
    /// real value; tests set an exact one.
    qreal m_pixelPerMM = 96.0 / 25.4;

    qreal m_devicePixelRatio = 1.0;
    QSize m_viewportSizePx;

    qreal m_verticalSpacingMM = 5.0;
    qreal m_horizontalSpacingMM = 1.0;

    QList<LayoutItemMM> m_layoutItems;
    QList<QRectF> m_blockRectsMM;
    int m_blockIndex = -1;

    QList<ViewportPlacement> m_placements;
    QRect m_blockRectPx;

    int m_horizontalOffset = 0;
    int m_verticalOffset = 0;
    QPoint m_minimumOffset;
    QPoint m_maximumOffset;

    quint64 m_requestGeneration = 0;
};

}   // namespace pdfinteraction

#endif   // VIEWPORTCONTROLLER_H
