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

// Architecture invariant I26, second half: the Quick canvas lays a document out
// where the Widgets path lays it out, and presents the pixels the Core renderer
// produced without moving or resampling them.
//
// This is the P4-S6 differential gate, and it is the only place in the tree that
// links both canvases. The oracle is deliberately narrow: it is reached through
// pdf::PDFDrawSpaceLayoutProbe, a migration-only free function in LoupeLibWidgets
// that hands back page rectangles in millimetres. This target therefore
// constructs no QWidget, opens no window belonging to the Widgets path, and
// cannot reach PDFDrawWidgetProxy at all. ADR-009 as amended prohibits
// QQuickWidget and WindowContainer as product architecture, and none of that is
// reachable from here -- what is reachable is the layout arithmetic the neutral
// ViewportController reimplemented, which is exactly the thing that could have
// drifted while nobody was comparing.
//
// The Widgets link is migration-only. It is not installed, it is named in the
// changelog fragment as temporary, and it goes away in Phase 5 with the library.
//
// Budgets live in testdata/canvas-parity/budgets.json and are read fail-closed:
// a case with no budget entry fails rather than falling back to a default,
// because a differential gate that invents its own tolerance is not a gate.
// Observed measurements are written next to the test binary as evidence; they
// are an output, not a baseline, so nothing here can pass by regenerating a
// golden.

#include <QtTest>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QQuickWindow>

#include <memory>

#include "loupecanvasitem.h"

#include "documentcontextsource.h"
#include "hittestsource.h"
#include "interactioncontroller.h"
#include "jobsubmitter.h"
#include "overlaybuilder.h"
#include "pagesurfacecoordinator.h"
#include "pagesurfacerenderer.h"
#include "viewportcontroller.h"

#include "pdfcatalog.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfprocessingbudget.h"

#include "pdfdrawspacelayoutprobe.h"

namespace
{

constexpr int ViewportSide = 420;
constexpr qreal PixelPerMM = 96.0 / 25.4;

QString budgetsPath()
{
    return QStringLiteral(LOUPE_UNITTEST_SOURCE_DIR "/testdata/canvas-parity/budgets.json");
}

/// Runs submitted work inline so a requested surface is admitted by the time
/// requestSurfaces() returns. This is a differential test, not a scheduling one;
/// the scheduling contract is UnitTestsPageSurface's.
class InlineJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        const QString jobId = spec.jobId.isEmpty() ? QStringLiteral("job-%1").arg(++m_sequence) : spec.jobId;

        auto token = std::make_shared<pdf::PDFJobCancellationToken>();
        pdf::PDFJobContext context(token, pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});
        work(context);

        return jobId;
    }

    bool cancel(const QString&) override { return false; }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = pdf::PDFJobStatus::Succeeded;
        return result;
    }

    void publishCurrentRevision(const QString&, const pdf::PDFRevisionIdentity&) override {}
    void clearCurrentRevision(const QString&) override {}

private:
    quint64 m_sequence = 0;
};

/// The Core layout enum for a neutral one. The neutral enum is deliberately the
/// Core set minus Custom, so this is a total mapping and not a default-carrying
/// switch: a new layout has to be handled here rather than silently laid out as
/// one column on the oracle side.
pdf::PageLayout toCoreLayout(pdfinteraction::PageLayout layout)
{
    switch (layout)
    {
        case pdfinteraction::PageLayout::SinglePage:
            return pdf::PageLayout::SinglePage;
        case pdfinteraction::PageLayout::OneColumn:
            return pdf::PageLayout::OneColumn;
        case pdfinteraction::PageLayout::TwoPagesLeft:
            return pdf::PageLayout::TwoPagesLeft;
        case pdfinteraction::PageLayout::TwoPagesRight:
            return pdf::PageLayout::TwoPagesRight;
        case pdfinteraction::PageLayout::TwoColumnLeft:
            return pdf::PageLayout::TwoColumnLeft;
        case pdfinteraction::PageLayout::TwoColumnRight:
            return pdf::PageLayout::TwoColumnRight;
    }

    return pdf::PageLayout::OneColumn;
}

/// Fills a builder with synthetic pages. `alpha` puts a partly transparent shape
/// over an opaque one, which is what makes the blend case a blend case rather
/// than a second flat fill.
void appendPages(pdf::PDFDocumentBuilder& builder, int pageCount, QRectF mediaBox, bool alpha = false, QRectF cropBox = QRectF())
{
    for (int index = 0; index < pageCount; ++index)
    {
        const pdf::PDFObjectReference page = builder.appendPage(mediaBox);

        if (cropBox.isValid())
        {
            builder.setPageCropBox(page, cropBox);
        }

        pdf::PDFPageContentStreamBuilder contentBuilder(&builder, pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
        if (QPainter* painter = contentBuilder.begin(page))
        {
            const QRectF inner = mediaBox.adjusted(mediaBox.width() * 0.1, mediaBox.height() * 0.1, -mediaBox.width() * 0.1, -mediaBox.height() * 0.1);
            painter->fillRect(inner, index % 2 == 0 ? Qt::darkBlue : Qt::darkGreen);

            if (alpha)
            {
                QColor translucent(Qt::red);
                translucent.setAlphaF(0.5f);
                painter->fillRect(inner.adjusted(inner.width() * 0.25, inner.height() * 0.25, 0.0, 0.0), translucent);
            }

            contentBuilder.end(painter);
        }
    }
}

/// Both sides of the comparison over one document.
///
/// The document itself stays on the caller's stack. pdf::PDFDocument is returned
/// by value from the builder and every existing caller relies on that being an
/// elided prvalue; copying one would copy a catalog built over the original
/// storage, so this holds a pointer and nothing more.
struct ParityFixture
{
    ParityFixture(pdf::PDFDocument* document, pdfinteraction::PageLayout layout, pdf::PageRotation rotation) :
        document(document),
        context(document),
        revisions(&context),
        geometry(&context),
        renderer(context),
        layout(layout),
        rotation(rotation)
    {
        viewport.setGeometrySource(&geometry);
        viewport.setPixelPerMM(PixelPerMM);
        viewport.setDevicePixelRatio(1.0);
        viewport.setPageLayout(layout);
        viewport.setRotation(rotation);
        viewport.setViewportSizePx(QSize(ViewportSide, ViewportSide));

        surfaces = std::make_unique<pdfinteraction::PageSurfaceCoordinator>(revisions, submitter, renderer, viewport);
        surfaces->setDocumentKey(QStringLiteral("canvas-parity"));

        overlays = std::make_unique<pdfinteraction::OverlayBuilder>(viewport);
        controller = std::make_unique<pdfinteraction::InteractionController>(revisions, viewport, hitTest, *overlays);

        window = std::make_unique<QQuickWindow>();
        window->resize(ViewportSide, ViewportSide);

        item = std::make_unique<pdfquick::LoupeCanvasItem>();
        item->setParentItem(window->contentItem());
        item->setSize(QSizeF(ViewportSide, ViewportSide));
        item->bind(&viewport, controller.get(), surfaces.get());

        window->show();
        QTest::qWaitForWindowExposed(window.get());

        surfaces->requestSurfaces();
        QCoreApplication::processEvents();
    }

    ~ParityFixture()
    {
        // The item first: it holds render-thread connections into the window.
        item.reset();
        window.reset();
    }

    int pageCount() const { return int(document->getCatalog()->getPageCount()); }

    QImage renderFrame()
    {
        QCoreApplication::processEvents();

        if (window && item)
        {
            item->update();
            QCoreApplication::processEvents();
        }

        const QImage frame = window ? window->grabWindow() : QImage();
        QCoreApplication::processEvents();
        return frame;
    }

    /// Page rectangles in millimetres, from each side.
    ///
    /// Millimetres rather than pixels on purpose: that is the unit the oracle
    /// computes in, and converting the oracle into pixels would mean
    /// reimplementing the very arithmetic under comparison.
    QList<QRectF> oracleRectsMM() const
    {
        // The Widgets draw-space layout, reached through the migration-only probe
        // in LoupeLibWidgets. No QWidget is constructed anywhere in this file and
        // none can be: the probe hands back rectangles.
        return pdf::PDFDrawSpaceLayoutProbe::layoutPageRectsMM(document, toCoreLayout(layout), rotation);
    }

    QList<QRectF> quickRectsMM() const
    {
        const qreal scale = viewport.pixelPerMM() * viewport.zoom();

        QList<QRectF> rects;
        for (int index = 0; index < pageCount(); ++index)
        {
            const QRect placed = viewport.placedPageRect(index);
            rects.append(placed.isEmpty() || scale <= 0.0
                             ? QRectF()
                             : QRectF(placed.x() / scale, placed.y() / scale, placed.width() / scale, placed.height() / scale));
        }
        return rects;
    }

    pdf::PDFDocument* document = nullptr;
    pdf::PDFDocumentContext context;

    pdfinteraction::PDFDocumentContextSource revisions;
    pdfinteraction::PDFDocumentPageGeometrySource geometry;
    pdfinteraction::ViewportController viewport;
    InlineJobSubmitter submitter;
    pdfinteraction::PDFSessionPageSurfaceRenderer renderer;
    pdfinteraction::HitTestDispatcher hitTest;

    std::unique_ptr<pdfinteraction::PageSurfaceCoordinator> surfaces;
    std::unique_ptr<pdfinteraction::OverlayBuilder> overlays;
    std::unique_ptr<pdfinteraction::InteractionController> controller;
    std::unique_ptr<pdfquick::LoupeCanvasItem> item;
    std::unique_ptr<QQuickWindow> window;

    pdfinteraction::PageLayout layout;
    pdf::PageRotation rotation;
};

}   // namespace

class CanvasParityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void singleColumnLayoutMatchesTheWidgetsOracle();
    void twoColumnLayoutMatchesTheWidgetsOracle();
    void rotationMatchesTheWidgetsOracle();
    void cropBoxIsTheLaidOutBoxInBothPaths();
    void interactionCoordinateMappingRoundTrips();

    void presentedPixelsMatchTheRenderedSurface();
    void alphaBlendedPageMatchesTheRenderedSurface();

private:
    void compareLayout(const QString& name, const ParityFixture& fixture);
    void comparePixels(const QString& name, ParityFixture& fixture, int pageIndex);

    QJsonObject budgetFor(const QString& name) const;
    void writeEvidence(const QString& name, const QJsonObject& measurement);
};

void CanvasParityTest::compareLayout(const QString& name, const ParityFixture& fixture)
{
    const QList<QRectF> oracle = fixture.oracleRectsMM();
    const QList<QRectF> quick = fixture.quickRectsMM();

    QCOMPARE(quick.size(), oracle.size());

    const QJsonObject budget = budgetFor(name);
    const qreal toleranceMM = budget.value(QStringLiteral("geometry_tolerance_mm")).toDouble(-1.0);
    QVERIFY2(toleranceMM > 0.0, qPrintable(QStringLiteral("no geometry tolerance recorded for %1").arg(name)));

    qreal worstExtent = 0.0;
    qreal worstOffset = 0.0;

    for (int index = 0; index < oracle.size(); ++index)
    {
        if (oracle.at(index).isEmpty() || quick.at(index).isEmpty())
        {
            // A page neither side laid out is agreement; a page only one side
            // laid out is the divergence this gate is looking for.
            QCOMPARE(oracle.at(index).isEmpty(), quick.at(index).isEmpty());
            continue;
        }

        worstExtent = qMax(worstExtent, qAbs(oracle.at(index).width() - quick.at(index).width()));
        worstExtent = qMax(worstExtent, qAbs(oracle.at(index).height() - quick.at(index).height()));

        if (index > 0 && !oracle.at(index - 1).isEmpty() && !quick.at(index - 1).isEmpty())
        {
            // Absolute position depends on centring and on the scroll offset,
            // which are host state rather than layout. The vector between two
            // pages is the layout, and it is what a wrong column arrangement or
            // a wrong inter-page gap actually moves.
            const QPointF oracleStep = oracle.at(index).topLeft() - oracle.at(index - 1).topLeft();
            const QPointF quickStep = quick.at(index).topLeft() - quick.at(index - 1).topLeft();

            worstOffset = qMax(worstOffset, qAbs(oracleStep.x() - quickStep.x()));
            worstOffset = qMax(worstOffset, qAbs(oracleStep.y() - quickStep.y()));
        }
    }

    QJsonObject measurement;
    measurement[QStringLiteral("pages")] = oracle.size();
    measurement[QStringLiteral("worst_extent_delta_mm")] = worstExtent;
    measurement[QStringLiteral("worst_offset_delta_mm")] = worstOffset;
    measurement[QStringLiteral("geometry_tolerance_mm")] = toleranceMM;
    writeEvidence(name, measurement);

    QVERIFY2(worstExtent <= toleranceMM,
             qPrintable(QStringLiteral("%1: page extent differs from the Widgets oracle by %2 mm").arg(name).arg(worstExtent)));
    QVERIFY2(worstOffset <= toleranceMM,
             qPrintable(QStringLiteral("%1: page placement differs from the Widgets oracle by %2 mm").arg(name).arg(worstOffset)));
}

void CanvasParityTest::comparePixels(const QString& name, ParityFixture& fixture, int pageIndex)
{
    const QImage frame = fixture.renderFrame();
    QVERIFY2(!frame.isNull(), "the Qt Quick scene graph produced no frame");

    const pdfinteraction::CanvasTile* tile = fixture.surfaces->snapshot().tileForPage(pageIndex);
    QVERIFY2(tile != nullptr, "no admitted surface for the compared page");
    QVERIFY(tile->exact);
    QVERIFY(tile->pixels);

    const QRect placed = tile->placedRect;
    QVERIFY(!placed.isEmpty());

    // Inset by one pixel. The page edge is where the scene graph antialiases the
    // quad against the canvas background, and a one-pixel seam says nothing
    // about whether the page itself was placed or resampled correctly.
    const QRect interior = placed.adjusted(1, 1, -1, -1).intersected(QRect(QPoint(0, 0), frame.size()));
    QVERIFY(interior.width() > 2 && interior.height() > 2);

    const QImage presented = frame.copy(interior).convertToFormat(QImage::Format_RGBA8888);

    const QRect sourceRect = interior.translated(-placed.topLeft());
    const QImage rendered = tile->pixels->image.copy(sourceRect).convertToFormat(QImage::Format_RGBA8888);

    QCOMPARE(presented.size(), rendered.size());

    int differingPixels = 0;
    int observedMaxDelta = 0;

    for (int y = 0; y < presented.height(); ++y)
    {
        const uchar* presentedLine = presented.constScanLine(y);
        const uchar* renderedLine = rendered.constScanLine(y);

        for (int x = 0; x < presented.width(); ++x)
        {
            const int offset = x * 4;
            int pixelMaxDelta = 0;

            // Alpha is compared too: a page composited onto the wrong background
            // is exactly the kind of difference a colour-only comparison misses.
            for (int channel = 0; channel < 4; ++channel)
            {
                pixelMaxDelta = qMax(pixelMaxDelta, qAbs(int(presentedLine[offset + channel]) - int(renderedLine[offset + channel])));
            }

            observedMaxDelta = qMax(observedMaxDelta, pixelMaxDelta);
            if (pixelMaxDelta > 2)
            {
                ++differingPixels;
            }
        }
    }

    const QJsonObject budget = budgetFor(name);
    const int maxDeltaBudget = budget.value(QStringLiteral("max_channel_delta_budget")).toInt(-1);
    const int differingBudget = budget.value(QStringLiteral("differing_pixel_budget")).toInt(-1);

    QJsonObject measurement;
    measurement[QStringLiteral("width")] = presented.width();
    measurement[QStringLiteral("height")] = presented.height();
    measurement[QStringLiteral("max_channel_delta")] = observedMaxDelta;
    measurement[QStringLiteral("differing_pixels")] = differingPixels;
    measurement[QStringLiteral("max_channel_delta_budget")] = maxDeltaBudget;
    measurement[QStringLiteral("differing_pixel_budget")] = differingBudget;
    writeEvidence(name, measurement);

    QVERIFY2(maxDeltaBudget >= 0 && differingBudget >= 0, qPrintable(QStringLiteral("no pixel budget recorded for %1").arg(name)));
    QVERIFY2(observedMaxDelta <= maxDeltaBudget,
             qPrintable(QStringLiteral("%1: presented pixels differ from the rendered surface by %2").arg(name).arg(observedMaxDelta)));
    QVERIFY2(differingPixels <= differingBudget,
             qPrintable(QStringLiteral("%1: %2 pixels exceed the channel budget").arg(name).arg(differingPixels)));
}

QJsonObject CanvasParityTest::budgetFor(const QString& name) const
{
    QFile file(budgetsPath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return QJsonObject();
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    return root.value(QStringLiteral("cases")).toObject().value(name).toObject();
}

void CanvasParityTest::writeEvidence(const QString& name, const QJsonObject& measurement)
{
    // Next to the test binary, not next to the source. These are observations of
    // this run on this backend, and a differential gate whose evidence can be
    // committed back over the budget is a gate that can be argued with.
    QFile file(QDir::current().filePath(QStringLiteral("canvas-parity-%1.json").arg(name)));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(measurement).toJson(QJsonDocument::Indented));
    }
}

void CanvasParityTest::singleColumnLayoutMatchesTheWidgetsOracle()
{
    pdf::PDFDocumentBuilder builder;
    appendPages(builder, 3, QRectF(0, 0, 200, 300));
    pdf::PDFDocument document = builder.build();

    ParityFixture fixture(&document, pdfinteraction::PageLayout::OneColumn, pdf::PageRotation::None);
    compareLayout(QStringLiteral("layout-one-column"), fixture);
}

void CanvasParityTest::twoColumnLayoutMatchesTheWidgetsOracle()
{
    pdf::PDFDocumentBuilder builder;
    appendPages(builder, 4, QRectF(0, 0, 200, 300));
    pdf::PDFDocument document = builder.build();

    ParityFixture fixture(&document, pdfinteraction::PageLayout::TwoColumnLeft, pdf::PageRotation::None);
    compareLayout(QStringLiteral("layout-two-column-left"), fixture);
}

void CanvasParityTest::rotationMatchesTheWidgetsOracle()
{
    pdf::PDFDocumentBuilder builder;
    appendPages(builder, 2, QRectF(0, 0, 200, 300));
    pdf::PDFDocument document = builder.build();

    // A rotation that transposes the page on one side and not the other is a
    // layout that looks plausible until the first landscape document.
    ParityFixture fixture(&document, pdfinteraction::PageLayout::OneColumn, pdf::PageRotation::Rotate90);
    compareLayout(QStringLiteral("layout-rotate-90"), fixture);
}

void CanvasParityTest::cropBoxIsTheLaidOutBoxInBothPaths()
{
    pdf::PDFDocumentBuilder builder;
    appendPages(builder, 2, QRectF(0, 0, 200, 300), false, QRectF(20, 30, 160, 240));
    pdf::PDFDocument document = builder.build();

    ParityFixture fixture(&document, pdfinteraction::PageLayout::OneColumn, pdf::PageRotation::None);

    // Laying a page out by its media box while rendering it by its crop box is a
    // silent misalignment: the page is the right shape and everything on it is
    // in the wrong place.
    compareLayout(QStringLiteral("layout-crop-box"), fixture);

    const QRectF laidOut = fixture.quickRectsMM().at(0);
    QVERIFY(!laidOut.isEmpty());

    // And it really is the crop box being laid out, not the media box that
    // happens to agree with it.
    QVERIFY(laidOut.width() < 200.0);
}

void CanvasParityTest::interactionCoordinateMappingRoundTrips()
{
    pdf::PDFDocumentBuilder builder;
    appendPages(builder, 1, QRectF(0, 0, 200, 300));
    pdf::PDFDocument document = builder.build();

    ParityFixture fixture(&document, pdfinteraction::PageLayout::OneColumn, pdf::PageRotation::None);

    const QRect placed = fixture.viewport.placedPageRect(0);
    QVERIFY(!placed.isEmpty());

    for (const QPointF& fraction : { QPointF(0.15, 0.15), QPointF(0.5, 0.5), QPointF(0.85, 0.7) })
    {
        const QPoint viewportPoint(placed.left() + qRound(placed.width() * fraction.x()), placed.top() + qRound(placed.height() * fraction.y()));

        const std::optional<QPointF> pagePoint = fixture.viewport.viewportToPagePoint(viewportPoint, 0);
        QVERIFY(pagePoint.has_value());

        const QPointF backToViewport = fixture.viewport.pagePointToViewportMatrix(0).map(pagePoint.value());

        // A hit test that lands a pixel away is a finding highlighted next to
        // the thing it is about. One viewport pixel is the whole budget.
        QVERIFY(qAbs(backToViewport.x() - viewportPoint.x()) <= 1.0);
        QVERIFY(qAbs(backToViewport.y() - viewportPoint.y()) <= 1.0);
    }
}

void CanvasParityTest::presentedPixelsMatchTheRenderedSurface()
{
    pdf::PDFDocumentBuilder builder;
    appendPages(builder, 1, QRectF(0, 0, 200, 300));
    pdf::PDFDocument document = builder.build();

    ParityFixture fixture(&document, pdfinteraction::PageLayout::OneColumn, pdf::PageRotation::None);
    comparePixels(QStringLiteral("pixels-flat"), fixture, 0);
}

void CanvasParityTest::alphaBlendedPageMatchesTheRenderedSurface()
{
    pdf::PDFDocumentBuilder builder;
    appendPages(builder, 1, QRectF(0, 0, 200, 300), true);
    pdf::PDFDocument document = builder.build();

    ParityFixture fixture(&document, pdfinteraction::PageLayout::OneColumn, pdf::PageRotation::None);
    comparePixels(QStringLiteral("pixels-alpha"), fixture, 0);
}

QTEST_MAIN(CanvasParityTest)

#include "tst_canvasparitytest.moc"
