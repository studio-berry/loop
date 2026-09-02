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

// Architecture invariant I23, viewport half: the host-neutral layer owns layout,
// zoom, rotation, pan, and the page/viewport transforms.
//
// As in tst_interactionboundarytest.cpp, the strongest assertion here is the link
// line in UnitTests/CMakeLists.txt. This target links LoopLibInteraction,
// LoopLibCore, Qt6::Core, Qt6::Gui and Qt6::Test, and deliberately not
// Qt6::Widgets. QTEST_GUILESS_MAIN then proves the P4-S3 exit condition for the
// viewport: none of this behaviour needs a QWidget, a QScreen, or a scrollbar.

#include <QtTest>

#include <memory>

#include "viewportcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"

namespace
{

/// Fixed-size pages and a transform simple enough to invert by hand, so a layout
/// test does not need a PDF at all.
class FakeGeometrySource final : public pdfinteraction::IPageGeometrySource
{
public:
    FakeGeometrySource(int pageCount, QSizeF sizeMM) :
        m_pageCount(pageCount),
        m_sizeMM(sizeMM)
    {
    }

    int pageCount() const override { return m_pageCount; }

    QSizeF pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);

        const bool transposed = extraRotation == pdf::PageRotation::Rotate90 || extraRotation == pdf::PageRotation::Rotate270;
        return transposed ? m_sizeMM.transposed() : m_sizeMM;
    }

    QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(extraRotation);

        // A 100x100 page box mapped onto the placed rectangle, y flipped as PDF
        // page space is.
        QTransform matrix;
        matrix.translate(deviceRect.left(), deviceRect.bottom());
        matrix.scale(deviceRect.width() / PageBoxSize, -deviceRect.height() / PageBoxSize);
        return matrix;
    }

    static constexpr qreal PageBoxSize = 100.0;

private:
    int m_pageCount = 0;
    QSizeF m_sizeMM;
};

constexpr QSizeF A4 = QSizeF(210.0, 297.0);

}   // namespace

class ViewportControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void oneColumnStacksPagesAndCentresThem();
    void singlePageLayoutIsOneBlockPerPage();
    void zoomAnchorKeepsTheAnchoredPointStill();
    void zoomIsClampedToTheSupportedRange();
    void fitHintsUseTheInjectedViewportSize();
    void rotationTransposesPageExtent();
    void panningDoesNotSupersedeDemand();
    void devicePixelRatioChangeSupersedesDemand();
    void visiblePagesFollowTheViewportRect();
    void activePagesAddTheLookAhead();
    void hitTestMapsViewportPointsIntoPageSpace();
    void hitTestOutsideEveryPageReportsNoPage();
    void documentGeometrySourceReadsPageSizesFromTheDocument();
    void destroyedContextDegradesToAnEmptyDocument();

private:
    /// Four A4 pages, one millimetre per pixel, in a viewport small enough that
    /// the block overflows on both axes.
    static void configure(pdfinteraction::ViewportController& controller, pdfinteraction::IPageGeometrySource* source);
};

void ViewportControllerTest::configure(pdfinteraction::ViewportController& controller, pdfinteraction::IPageGeometrySource* source)
{
    controller.setPixelPerMM(1.0);
    controller.setViewportSizePx(QSize(100, 100));
    controller.setPageLayout(pdfinteraction::PageLayout::OneColumn);
    controller.setGeometrySource(source);
}

void ViewportControllerTest::oneColumnStacksPagesAndCentresThem()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    QCOMPARE(controller.blockCount(), 1);
    QCOMPARE(controller.placements().size(), 4);

    // Pages are centred on the y axis and stacked with the vertical spacing
    // between them.
    QCOMPARE(controller.placements().at(0).placedRect, QRect(-105, 0, 210, 297));
    QCOMPARE(controller.placements().at(1).placedRect, QRect(-105, 302, 210, 297));
    QVERIFY(!controller.isBlockMode());
}

void ViewportControllerTest::singlePageLayoutIsOneBlockPerPage()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);
    controller.setPageLayout(pdfinteraction::PageLayout::SinglePage);

    QCOMPARE(controller.blockCount(), 4);
    QCOMPARE(controller.placements().size(), 1);
    QVERIFY(controller.isBlockMode());

    controller.setBlockIndex(2);
    QCOMPARE(controller.blockIndex(), 2);
    QCOMPARE(controller.placements().at(0).pageIndex, 2);

    // Out of range is bounded, not accepted.
    controller.setBlockIndex(99);
    QCOMPARE(controller.blockIndex(), 3);
}

void ViewportControllerTest::zoomAnchorKeepsTheAnchoredPointStill()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    controller.setOffset(QPoint(-50, -400));
    QCOMPARE(controller.offset(), QPoint(-50, -400));

    controller.setZoom(2.0, QPointF(50.0, 50.0));

    // The draw-space point under (50, 50) is unchanged: the offsets scale with
    // the zoom around the anchor rather than around the origin.
    QCOMPARE(controller.zoom(), 2.0);
    QCOMPARE(controller.offset(), QPoint(-150, -850));
}

void ViewportControllerTest::zoomIsClampedToTheSupportedRange()
{
    FakeGeometrySource geometry(1, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    controller.setZoom(1000.0);
    QCOMPARE(controller.zoom(), pdfinteraction::ViewportController::MaximumZoom);

    controller.setZoom(0.0001);
    QCOMPARE(controller.zoom(), pdfinteraction::ViewportController::MinimumZoom);
}

void ViewportControllerTest::fitHintsUseTheInjectedViewportSize()
{
    FakeGeometrySource geometry(1, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);
    controller.setViewportSizePx(QSize(420, 420));

    // 420 px at 1 px/mm, 5% margin, over a 210 mm wide page.
    QVERIFY(qFuzzyCompare(controller.zoomHint(pdfinteraction::ZoomHint::FitWidth), 420.0 * 0.95 / 210.0));
    QVERIFY(qFuzzyCompare(controller.zoomHint(pdfinteraction::ZoomHint::FitHeight), 420.0 * 0.95 / 297.0));

    // Fit is the smaller of the two, so the whole page is inside the viewport.
    QVERIFY(qFuzzyCompare(controller.zoomHint(pdfinteraction::ZoomHint::Fit), controller.zoomHint(pdfinteraction::ZoomHint::FitHeight)));
}

void ViewportControllerTest::rotationTransposesPageExtent()
{
    FakeGeometrySource geometry(1, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    const quint64 before = controller.requestGeneration();
    controller.setRotation(pdf::PageRotation::Rotate90);

    QCOMPARE(controller.placements().at(0).placedRect.size(), QSize(297, 210));
    QVERIFY(controller.requestGeneration() > before);
}

void ViewportControllerTest::panningDoesNotSupersedeDemand()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    const quint64 before = controller.requestGeneration();
    QSignalSpy demandSpy(&controller, &pdfinteraction::ViewportController::demandChanged);
    QSignalSpy placementSpy(&controller, &pdfinteraction::ViewportController::placementsChanged);

    const QPoint applied = controller.scrollByPixels(QPoint(0, -200));

    // A pan moves which pages are wanted. It does not make an in-flight render of
    // a still-visible page wrong, and cancelling one mid-drag is exactly what
    // issue #142 forbids.
    QCOMPARE(applied, QPoint(0, -200));
    QCOMPARE(controller.requestGeneration(), before);
    QCOMPARE(demandSpy.count(), 0);
    QCOMPARE(placementSpy.count(), 1);

    // Clamped at the end of the block rather than scrolling past it.
    const QPoint clamped = controller.scrollByPixels(QPoint(0, -100000));
    QCOMPARE(controller.offset().y(), controller.minimumOffset().y());
    QVERIFY(clamped.y() > -100000);
}

void ViewportControllerTest::devicePixelRatioChangeSupersedesDemand()
{
    FakeGeometrySource geometry(1, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    const quint64 before = controller.requestGeneration();
    const QRect placedBefore = controller.placements().at(0).placedRect;

    controller.setDevicePixelRatio(2.0);

    // No layout geometry changes, but every surface already rendered is now the
    // wrong resolution.
    QCOMPARE(controller.placements().at(0).placedRect, placedBefore);
    QVERIFY(controller.requestGeneration() > before);
}

void ViewportControllerTest::visiblePagesFollowTheViewportRect()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    QCOMPARE(controller.visiblePages(), QList<int>({ 0 }));
    QCOMPARE(controller.currentPage(), 0);

    controller.setOffset(QPoint(0, -302));
    QCOMPARE(controller.visiblePages(), QList<int>({ 1 }));

    // The pure projection agrees with the live one.
    QList<pdfinteraction::ViewportPlacement> placed;
    for (int pageIndex : { 0, 1 })
    {
        placed.push_back(pdfinteraction::ViewportPlacement{ pageIndex, controller.placedPageRect(pageIndex) });
    }
    QCOMPARE(pdfinteraction::ViewportController::visiblePagesFor(placed, controller.viewportRect()), QList<int>({ 1 }));
}

void ViewportControllerTest::activePagesAddTheLookAhead()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    QCOMPARE(controller.activePages(), QList<int>({ 0, 1, 2 }));

    // The look-ahead never runs off the end of the document.
    controller.setOffset(QPoint(0, -1103));
    QCOMPARE(controller.activePages(), QList<int>({ 3 }));
}

void ViewportControllerTest::hitTestMapsViewportPointsIntoPageSpace()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    QPointF pagePoint;
    QCOMPARE(controller.pageUnderPoint(QPoint(50, 50), &pagePoint), 0);

    // Page 0 is placed at (0, 0, 210, 297) in the viewport; the fake source maps a
    // 100x100 page box onto it with y flipped.
    QVERIFY(qAbs(pagePoint.x() - 50.0 * 100.0 / 210.0) < 0.001);
    QVERIFY(qAbs(pagePoint.y() - (297.0 - 50.0) * 100.0 / 297.0) < 0.001);

    // Round trip: the page point maps back onto the viewport point.
    const QPointF roundTrip = controller.pagePointToViewportMatrix(0).map(pagePoint);
    QVERIFY(qAbs(roundTrip.x() - 50.0) < 0.001);
    QVERIFY(qAbs(roundTrip.y() - 50.0) < 0.001);
}

void ViewportControllerTest::hitTestOutsideEveryPageReportsNoPage()
{
    FakeGeometrySource geometry(4, A4);
    pdfinteraction::ViewportController controller;
    configure(controller, &geometry);

    // Left of the block: page 0 starts at x == 0 in viewport coordinates.
    QCOMPARE(controller.pageUnderPoint(QPoint(-1, 50)), -1);

    // The inclusive edge belongs to the page, the pixel after it does not.
    QCOMPARE(controller.pageUnderPoint(QPoint(210, 50)), 0);
    QCOMPARE(controller.pageUnderPoint(QPoint(211, 50)), -1);
}

void ViewportControllerTest::documentGeometrySourceReadsPageSizesFromTheDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);

    pdfinteraction::PDFDocumentPageGeometrySource geometry(&context);
    QCOMPARE(geometry.pageCount(), 1);

    // 100 x 200 points is 35.28 x 70.56 millimetres.
    const QSizeF sizeMM = geometry.pageSizeMM(0, pdf::PageRotation::None);
    QVERIFY(qAbs(sizeMM.width() - 100.0 * 25.4 / 72.0) < 0.01);
    QVERIFY(qAbs(sizeMM.height() - 200.0 * 25.4 / 72.0) < 0.01);

    // Rotating by 90 degrees transposes it.
    const QSizeF rotatedMM = geometry.pageSizeMM(0, pdf::PageRotation::Rotate90);
    QVERIFY(qAbs(rotatedMM.width() - sizeMM.height()) < 0.01);

    // Out of range is empty rather than a crash or a guess.
    QVERIFY(geometry.pageSizeMM(7, pdf::PageRotation::None).isEmpty());
}

void ViewportControllerTest::destroyedContextDegradesToAnEmptyDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    auto context = std::make_unique<pdf::PDFDocumentContext>(&document);
    pdfinteraction::PDFDocumentPageGeometrySource geometry(context.get());
    QCOMPARE(geometry.pageCount(), 1);

    context.reset();

    // Same rule PDFDocumentContextSource follows: a destroyed context reads as
    // nothing, never as a dangling pointer.
    QCOMPARE(geometry.pageCount(), 0);
    QVERIFY(geometry.pagePointToDeviceMatrix(0, QRectF(0, 0, 10, 10), pdf::PageRotation::None).isIdentity());
}

QTEST_GUILESS_MAIN(ViewportControllerTest)

#include "tst_viewportcontrollertest.moc"
