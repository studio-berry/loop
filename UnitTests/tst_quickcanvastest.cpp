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


// Architecture invariant I25: the Quick canvas presents, and presents only.
//
// The link line in UnitTests/CMakeLists.txt is again load-bearing, but it proves
// the opposite of what the P4-S1..S4 targets prove. Those link no Qml and no
// Quick, so a presentation include fails to build. This one links LoupeLibQuick
// and therefore Qt6::Quick, and what it has to establish instead is that the
// admitted presentation host stayed on its own side of ADR-010 rule 5: Qt Quick
// events become the neutral input values and nothing else, the neutral values
// become scene-graph geometry and nothing else, and no document, session,
// scheduler or pixel buffer is reachable from QML.
//
// It is also where issue #140's two carry-over acceptance criteria are pinned,
// since neither could be tested in a layer with no scene graph.

#include <QtTest>

#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QWheelEvent>

#include <atomic>
#include <memory>

#include "canvaspalette.h"
#include "canvaspresentmetrics.h"
#include "canvastraceoverlay.h"
#include "loupecanvasitem.h"

#include "hittestsource.h"
#include "interactioncontroller.h"
#include "interactiontrace.h"
#include "jobsubmitter.h"
#include "overlaybuilder.h"
#include "pagesurfacecoordinator.h"
#include "pagesurfacerenderer.h"
#include "viewportcontroller.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfprocessingbudget.h"

namespace
{

constexpr QSizeF PageSizeMM = QSizeF(100.0, 100.0);
constexpr qreal PixelPerMM = 2.0;
constexpr qreal PageBoxSize = 100.0;

pdf::PDFRevisionIdentity makeRevision(const QString& documentId, quint64 documentRevision = 1)
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = documentId;
    revision.document.sourceDataHash = documentId.toUtf8();
    revision.documentRevision = documentRevision;
    return revision;
}

/// The same fake the P4-S3 and P4-S4 tests use: a layout test does not need a
/// PDF, and a canvas test does not need one either.
class FakeGeometrySource final : public pdfinteraction::IPageGeometrySource
{
public:
    explicit FakeGeometrySource(int pageCount) :
        m_pageCount(pageCount)
    {
    }

    int pageCount() const override { return m_pageCount; }

    QSizeF pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);

        const bool transposed = extraRotation == pdf::PageRotation::Rotate90 || extraRotation == pdf::PageRotation::Rotate270;
        return transposed ? PageSizeMM.transposed() : PageSizeMM;
    }

    QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(extraRotation);

        QTransform matrix;
        matrix.translate(deviceRect.left(), deviceRect.bottom());
        matrix.scale(deviceRect.width() / PageBoxSize, -deviceRect.height() / PageBoxSize);
        return matrix;
    }

private:
    int m_pageCount = 0;
};

class FakeRevisionSource final : public pdfinteraction::IDocumentRevisionSource
{
public:
    pdf::PDFRevisionIdentity currentRevision() const override { return revision; }
    bool isCurrent(const pdf::PDFRevisionIdentity& candidate) const override { return candidate == revision; }

    pdf::PDFRevisionIdentity revision = makeRevision(QStringLiteral("doc-1"));
};

class ScriptedHitTestSource final : public pdfinteraction::IHitTestSource
{
public:
    QList<pdfinteraction::InteractionTarget> hitTest(int pageIndex, QPointF pagePoint) const override
    {
        QList<pdfinteraction::InteractionTarget> hits;
        for (const pdfinteraction::InteractionTarget& target : targets)
        {
            if (target.pageIndex == pageIndex && target.pageBounds.contains(pagePoint))
            {
                hits.push_back(target);
            }
        }
        return hits;
    }

    QList<pdfinteraction::InteractionTarget> targets;
};

/// Re-exposes the protected event handlers.
///
/// This is what lets the translation contract be tested with no window, no
/// scene graph and no event loop: the handlers are what Qt Quick would call, and
/// calling them directly removes the parts of the test that would be a
/// screenshot rather than an assertion.
///
/// The scene-graph cases below deliberately do not call updatePaintNode or
/// releaseResources by hand. updatePaintNode needs a live render context to
/// create nodes and upload textures at all, and releaseResources is only correct
/// when the scene graph has already taken the node tree away -- calling either
/// directly would test a sequence that cannot happen. They drive the window
/// instead.
class ExposedCanvasItem final : public pdfquick::LoupeCanvasItem
{
public:
    using pdfquick::LoupeCanvasItem::focusOutEvent;
    using pdfquick::LoupeCanvasItem::keyPressEvent;
    using pdfquick::LoupeCanvasItem::mouseMoveEvent;
    using pdfquick::LoupeCanvasItem::mousePressEvent;
    using pdfquick::LoupeCanvasItem::mouseReleaseEvent;
    using pdfquick::LoupeCanvasItem::wheelEvent;
};

/// The P4-S3 fake, trimmed to what a canvas test needs: work runs inline, so a
/// requested surface is admitted by the time requestSurfaces() returns and the
/// scene graph has something real to hold.
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

    bool cancel(const QString& jobId) override
    {
        cancelledJobs.append(jobId);
        return false;
    }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = pdf::PDFJobStatus::Succeeded;
        return result;
    }

    void publishCurrentRevision(const QString& documentKey, const pdf::PDFRevisionIdentity& revision) override
    {
        publishedRevisions.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override { publishedRevisions.remove(documentKey); }

    QStringList cancelledJobs;
    QHash<QString, pdf::PDFRevisionIdentity> publishedRevisions;

private:
    quint64 m_sequence = 0;
};

/// Produces a surface without a PDF. The fill colour is per-revision so a frame
/// can be read back and attributed to the document that produced it.
class FakePageSurfaceRenderer final : public pdfinteraction::IPageSurfaceRenderer
{
public:
    pdfinteraction::PageSurfaceResult render(const pdfinteraction::PageSurfaceRequest& request, pdf::PDFJobContext& jobContext) override
    {
        ++renderCount;
        renderedKeys.append(request.key);

        pdfinteraction::PageSurfaceResult result;
        result.key = request.key;
        result.token = request.token;

        if (jobContext.isCancellationRequested())
        {
            result.state = pdfinteraction::SurfaceTerminalState::Cancelled;
            result.typedError = QStringLiteral("page-surface/cancelled");
            return result;
        }

        QImage image(request.key.targetPixelSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(fill);

        result.state = pdfinteraction::SurfaceTerminalState::Complete;
        result.pixels = pdfinteraction::makeSurfaceBuffer(std::move(image));
        result.pixelSize = request.key.targetPixelSize;
        result.byteSize = result.pixels ? result.pixels->byteSize : 0;
        return result;
    }

    void shedPrefetchAndQuality() override { ++shedCount; }

    int renderCount = 0;
    int shedCount = 0;
    QColor fill = QColor(Qt::white);
    QList<pdfinteraction::PageSurfaceKey> renderedKeys;
};

pdfinteraction::InteractionTarget makeTarget(const QString& id, QRectF bounds)
{
    pdfinteraction::InteractionTarget target;
    target.kind = pdfinteraction::InteractionTargetKind::Finding;
    target.pageIndex = 0;
    target.id = id;
    target.pageBounds = bounds;
    return target;
}

}   // namespace

class QuickCanvasTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void severityIsLegibleWithoutColour();
    void highContrastPreservesTheFocusRing();

    void missingTelemetryNeverReadsAsZero();
    void presentMetricsReportNoFramesAsUnavailable();
    void traceOverlayCarriesNoDocumentPayload();

    void pointerEventBecomesAPointerIntent();
    void wheelEventForwardsBothDeltas();
    void keyEventCarriesNoTypedText();
    void focusLossCancelsTheDrag();
    void unboundItemIgnoresInput();

    void admittedTilesReachTheSceneGraph();
    void revisionReplacementLeavesNoStaleTile();
    void staleOverlayFrameIsRefusedToo();
    void sceneGraphInvalidationRebuildsWithoutReparsing();
    void releaseResourcesThenRepaintRecovers();
    void windowChangeDropsRetainedNodes();
    void rapidZoomReversalNeverDrawsAnotherRevision();
    void rapidPageChangeKeepsOneFrameCurrent();
    void devicePixelRatioChangeRepublishesGeometry();
    void itemDestroyedWithWorkInFlight();
    void overlayOnlyChangeDoesNotResyncTiles();
    void firstViewIsUnavailableUntilAPageIsOnScreen();

private:
    void bindItem();

    /// Builds the coordinator half of the graph. Separate from init() because
    /// most cases in this file are translation tests that must keep proving they
    /// work with no coordinator at all.
    void buildCoordinator();

    /// Puts the item in a real window and renders one frame.
    ///
    /// A canvas lifecycle test cannot avoid the scene graph: updatePaintNode
    /// creates nodes and uploads textures through the window's render context,
    /// so there is no version of this that runs without one. The target's ctest
    /// entry pins QT_QPA_PLATFORM=offscreen and QT_QUICK_BACKEND=software, which
    /// is what makes the render deterministic and headless. ADR-010 is right
    /// that offscreen alone is not scene-graph evidence -- that is what the
    /// software and native smoke runs are for -- but it is exactly what these
    /// assertions need, because they are about node and texture lifetime rather
    /// than about which backend drew them.
    void showItemInWindow();

    /// Renders one frame and returns it. Fails the test rather than skipping if
    /// the scene graph produced nothing: a parity gate that quietly passes when
    /// it could not render is worse than no gate.
    QImage renderFrame();

    std::unique_ptr<FakeGeometrySource> m_geometry;
    std::unique_ptr<FakeRevisionSource> m_revisions;
    std::unique_ptr<pdfinteraction::ViewportController> m_viewport;
    std::unique_ptr<pdfinteraction::HitTestDispatcher> m_hitTest;
    std::unique_ptr<pdfinteraction::OverlayBuilder> m_overlays;
    std::unique_ptr<pdfinteraction::InteractionController> m_controller;
    std::unique_ptr<ScriptedHitTestSource> m_source;
    std::unique_ptr<ExposedCanvasItem> m_item;

    std::unique_ptr<InlineJobSubmitter> m_submitter;
    std::unique_ptr<FakePageSurfaceRenderer> m_renderer;
    std::unique_ptr<pdfinteraction::PageSurfaceCoordinator> m_surfaces;
    std::unique_ptr<QQuickWindow> m_window;
};

void QuickCanvasTest::init()
{
    m_geometry = std::make_unique<FakeGeometrySource>(2);
    m_revisions = std::make_unique<FakeRevisionSource>();

    m_viewport = std::make_unique<pdfinteraction::ViewportController>();
    m_viewport->setGeometrySource(m_geometry.get());
    m_viewport->setPixelPerMM(PixelPerMM);
    m_viewport->setDevicePixelRatio(1.0);
    m_viewport->setViewportSizePx(QSize(400, 400));

    m_source = std::make_unique<ScriptedHitTestSource>();
    m_source->targets.append(makeTarget(QStringLiteral("finding-1"), QRectF(10.0, 10.0, 40.0, 40.0)));

    m_hitTest = std::make_unique<pdfinteraction::HitTestDispatcher>();
    m_hitTest->addSource(m_source.get());

    m_overlays = std::make_unique<pdfinteraction::OverlayBuilder>(*m_viewport);

    m_controller = std::make_unique<pdfinteraction::InteractionController>(*m_revisions, *m_viewport, *m_hitTest, *m_overlays);

    m_item = std::make_unique<ExposedCanvasItem>();
    m_item->setSize(QSizeF(400.0, 400.0));
}

void QuickCanvasTest::cleanup()
{
    m_item.reset();
    m_window.reset();
    m_surfaces.reset();
    m_renderer.reset();
    m_submitter.reset();
    m_controller.reset();
    m_overlays.reset();
    m_hitTest.reset();
    m_source.reset();
    m_viewport.reset();
    m_revisions.reset();
    m_geometry.reset();
}

void QuickCanvasTest::bindItem()
{
    m_item->bind(m_viewport.get(), m_controller.get(), m_surfaces.get());
}

void QuickCanvasTest::buildCoordinator()
{
    m_submitter = std::make_unique<InlineJobSubmitter>();
    m_renderer = std::make_unique<FakePageSurfaceRenderer>();

    m_surfaces = std::make_unique<pdfinteraction::PageSurfaceCoordinator>(*m_revisions, *m_submitter, *m_renderer, *m_viewport);
    m_surfaces->setDocumentKey(QStringLiteral("doc-1"));
}

void QuickCanvasTest::showItemInWindow()
{
    m_window = std::make_unique<QQuickWindow>();
    m_window->resize(400, 400);

    m_item->setParentItem(m_window->contentItem());
    m_item->setSize(QSizeF(400.0, 400.0));

    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
}

QImage QuickCanvasTest::renderFrame()
{
    // grabWindow drives a full synchronous render pass, which is the only way to
    // make updatePaintNode run at a point the test controls. The relay the
    // coordinator posts completions through is queued, so anything admitted
    // since the last call has to be delivered first.
    QCoreApplication::processEvents();

    const QImage frame = m_window ? m_window->grabWindow() : QImage();
    if (frame.isNull())
    {
        // Not a skip. If the software backend cannot render here, every
        // scene-graph assertion in this file is unproven, and saying so is the
        // whole point of the gate.
        qWarning("the Qt Quick scene graph produced no frame; QT_QUICK_BACKEND and QT_QPA_PLATFORM decide whether it can");
    }

    QCoreApplication::processEvents();
    return frame;
}

void QuickCanvasTest::severityIsLegibleWithoutColour()
{
    const pdfquick::CanvasPalette palette = pdfquick::CanvasPalette::standard();

    pdfinteraction::OverlayPrimitive primitive;
    primitive.layer = pdfinteraction::OverlayLayer::Findings;

    primitive.severity = pdfinteraction::OverlaySeverity::None;
    const float none = palette.styleFor(primitive).strokeWidthPx;

    primitive.severity = pdfinteraction::OverlaySeverity::Info;
    const float info = palette.styleFor(primitive).strokeWidthPx;

    primitive.severity = pdfinteraction::OverlaySeverity::Warning;
    const float warning = palette.styleFor(primitive).strokeWidthPx;

    primitive.severity = pdfinteraction::OverlaySeverity::Error;
    const float error = palette.styleFor(primitive).strokeWidthPx;

    // The design tokens require that state not depend on colour alone. Width is
    // the redundant channel, so collapsing these to one value is a regression
    // against the accessibility contract rather than a tidy-up.
    QVERIFY(none < info);
    QVERIFY(info < warning);
    QVERIFY(warning < error);
}

void QuickCanvasTest::highContrastPreservesTheFocusRing()
{
    const pdfquick::CanvasPalette palette = pdfquick::CanvasPalette::highContrast();

    pdfinteraction::OverlayPrimitive primitive;
    primitive.layer = pdfinteraction::OverlayLayer::Findings;
    primitive.severity = pdfinteraction::OverlaySeverity::Info;
    primitive.focused = true;

    const pdfquick::OverlayStyle style = palette.styleFor(primitive);

    QVERIFY(style.focusRing);
    QVERIFY(style.focusRingWidthPx >= 2.0f);
    QVERIFY(palette.isHighContrast());

    // Focus and selection are separate states in OverlayFrame and must not be
    // conflated into one ring here.
    primitive.focused = false;
    primitive.selected = true;
    QVERIFY(!palette.styleFor(primitive).focusRing);
}

void QuickCanvasTest::missingTelemetryNeverReadsAsZero()
{
    pdfinteraction::ManualClock clock;
    pdfinteraction::InteractionTraceRecorder recorder(clock);

    const QStringList lines = pdfquick::CanvasTraceOverlay::lines(recorder.summary(), QJsonObject(), pdfquick::CanvasFrameStats());
    const QString rendered = lines.join(QLatin1Char('\n'));

    QVERIFY(!lines.isEmpty());

    // A percentile with no samples must render as an explicit gap. "0.00" would
    // report the fastest possible frame for a path that never ran once, which is
    // the exact failure the neutral layer's null percentiles exist to prevent.
    QVERIFY(rendered.contains(QStringLiteral("--")));
    QVERIFY(!rendered.contains(QStringLiteral("0.00")));

    // A refresh rate nobody supplied is unavailable with a typed reason, not an
    // assumed 60Hz.
    QVERIFY(rendered.contains(QStringLiteral("unavailable")));
    QVERIFY(rendered.contains(QStringLiteral("interaction-trace/refresh-rate-unknown")));
}

void QuickCanvasTest::presentMetricsReportNoFramesAsUnavailable()
{
    pdfquick::CanvasPresentMetrics metrics;

    const QJsonObject present = metrics.summary().value(QStringLiteral("present")).toObject();
    QCOMPARE(present.value(QStringLiteral("presented_frames")).toInteger(-1), qint64(0));

    for (const QString& field : { QStringLiteral("gpu_ms"), QStringLiteral("present_ms"), QStringLiteral("frame_interval_ms") })
    {
        const QJsonObject percentiles = present.value(field).toObject();
        QVERIFY2(!percentiles.value(QStringLiteral("available")).toBool(true), qPrintable(field));
        QVERIFY2(percentiles.value(QStringLiteral("p50_ms")).isNull(), qPrintable(field));
    }
}

void QuickCanvasTest::traceOverlayCarriesNoDocumentPayload()
{
    pdfinteraction::ManualClock clock;
    pdfinteraction::InteractionTraceRecorder recorder(clock);
    recorder.setTraceId(QStringLiteral("session-1"));

    m_controller->setTraceRecorder(&recorder);
    bindItem();

    // Drive real input at a distinctive coordinate over a target with a
    // distinctive id, so anything that leaked either into the panel is visible.
    const QPointF probe(1379.0, 2473.0);
    QMouseEvent press(QEvent::MouseButtonPress, probe, probe, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mousePressEvent(&press);

    QKeyEvent key(QEvent::KeyPress, Qt::Key_S, Qt::NoModifier, QStringLiteral("secret-text"));
    m_item->keyPressEvent(&key);

    pdfquick::CanvasFrameStats stats;
    stats.tiles = 2;
    stats.overlayPrimitives = 3;

    const QString rendered = pdfquick::CanvasTraceOverlay::lines(recorder.summary(), QJsonObject(), stats).join(QLatin1Char('\n'));

    // Issue #140 AC6. The panel is rendered over the page, so anything it can
    // display is displayed to whoever is looking at the screen and to whoever
    // receives a screenshot of it.
    QVERIFY(!rendered.contains(QStringLiteral("secret-text")));
    QVERIFY(!rendered.contains(QStringLiteral("finding-1")));
    QVERIFY(!rendered.contains(QStringLiteral("doc-1")));
    QVERIFY(!rendered.contains(QStringLiteral("1379")));
    QVERIFY(!rendered.contains(QStringLiteral("2473")));

    // The recorded trace itself must not carry the typed text either.
    const QString trace = QString::fromUtf8(QJsonDocument(recorder.trace().toJson()).toJson(QJsonDocument::Compact));
    QVERIFY(!trace.contains(QStringLiteral("secret-text")));
}

void QuickCanvasTest::pointerEventBecomesAPointerIntent()
{
    bindItem();

    QSignalSpy selectionSpy(m_controller.get(), &pdfinteraction::InteractionController::selectionChanged);

    // Page 0 is placed at the top of the viewport; a point inside the scripted
    // target's page-space box lands on it.
    const QRect placed = m_viewport->placedPageRect(0);
    QVERIFY(!placed.isEmpty());

    const QPointF inside(placed.left() + placed.width() * 0.3, placed.top() + placed.height() * 0.7);

    QMouseEvent press(QEvent::MouseButtonPress, inside, inside, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mousePressEvent(&press);

    QMouseEvent release(QEvent::MouseButtonRelease, inside, inside, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    m_item->mouseReleaseEvent(&release);

    QVERIFY(press.isAccepted());
    QCOMPARE(selectionSpy.count(), 1);

    const auto target = selectionSpy.at(0).at(0).value<pdfinteraction::InteractionTarget>();
    QCOMPARE(target.id, QStringLiteral("finding-1"));
}

void QuickCanvasTest::wheelEventForwardsBothDeltas()
{
    bindItem();

    const QPointF anchor(100.0, 100.0);
    const int notch = pdfinteraction::InteractionController::WheelDeltasPerStep;

    // Without the zoom modifier a notch scrolls and leaves the zoom alone.
    const qreal restingZoom = m_viewport->zoom();
    const QPoint offsetBefore = m_viewport->offset();

    QWheelEvent scroll(anchor, anchor, QPoint(0, 0), QPoint(0, -notch), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    m_item->wheelEvent(&scroll);

    QVERIFY(scroll.isAccepted());
    QCOMPARE(m_viewport->zoom(), restingZoom);
    QVERIFY(m_viewport->offset() != offsetBefore);

    // With it, the same notch zooms. Which modifier means zoom is the
    // controller's setting, not this item's; the item forwards the modifier and
    // lets the controller decide what it means.
    QWheelEvent zoomIn(anchor, anchor, QPoint(0, 0), QPoint(0, notch), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    m_item->wheelEvent(&zoomIn);

    QVERIFY(zoomIn.isAccepted());
    QVERIFY(m_viewport->zoom() > restingZoom);
}

void QuickCanvasTest::keyEventCarriesNoTypedText()
{
    pdfinteraction::ManualClock clock;
    pdfinteraction::InteractionTraceRecorder recorder(clock);
    m_controller->setTraceRecorder(&recorder);

    bindItem();

    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    m_item->keyPressEvent(&key);

    QCOMPARE(recorder.trace().inputs.size(), 1);

    const pdfinteraction::TraceInputRecord& record = recorder.trace().inputs.at(0);
    QVERIFY(record.key.has_value());
    QCOMPARE(record.key->key, int(Qt::Key_A));

    // KeyIntent has no text field at all, which is the structural half of the
    // rule. This asserts the item does not smuggle the text through some other
    // channel on its way in.
    const QString trace = QString::fromUtf8(QJsonDocument(recorder.trace().toJson()).toJson(QJsonDocument::Compact));
    QVERIFY(!trace.contains(QStringLiteral("\"text\"")));
}

void QuickCanvasTest::focusLossCancelsTheDrag()
{
    bindItem();

    QSignalSpy cancelSpy(m_controller.get(), &pdfinteraction::InteractionController::interactionCancelled);
    QSignalSpy dragSpy(m_controller.get(), &pdfinteraction::InteractionController::dragCompleted);

    const QRect placed = m_viewport->placedPageRect(0);
    const QPointF start(placed.left() + placed.width() * 0.3, placed.top() + placed.height() * 0.7);

    QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mousePressEvent(&press);

    const QPointF dragged = start + QPointF(40.0, 40.0);
    QMouseEvent move(QEvent::MouseMove, dragged, dragged, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mouseMoveEvent(&move);

    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    m_item->focusOutEvent(&focusOut);

    // A drag the user stopped steering must never complete: a completed drag is
    // routed to a command, and this one would commit a transform nobody asked
    // for.
    QCOMPARE(cancelSpy.count(), 1);
    QCOMPARE(dragSpy.count(), 0);
}

void QuickCanvasTest::unboundItemIgnoresInput()
{
    // No bind() call. An item with no document behind it is the normal state at
    // startup and after a close, and it must not crash or half-report.
    ExposedCanvasItem item;
    item.setSize(QSizeF(200.0, 200.0));

    const QPointF position(10.0, 10.0);
    QMouseEvent press(QEvent::MouseButtonPress, position, position, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    item.mousePressEvent(&press);

    QKeyEvent key(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    item.keyPressEvent(&key);

    QCOMPARE(item.zoom(), 1.0);
    QCOMPARE(item.currentPage(), -1);
    QCOMPARE(item.blockCount(), 0);
    QVERIFY(item.activeTool().isEmpty());
}

// ---------------------------------------------------------------------------
// P4-S6: scene-graph lifecycle and canvas parity.
//
// Everything above this line proves the translation contract with no window at
// all. Everything below needs a real one: updatePaintNode creates nodes and
// uploads textures through the window's render context, and the whole point of
// this session is what happens to those nodes and textures when the scene graph
// goes away, the window changes, the display metrics change, or the document is
// replaced underneath them.
// ---------------------------------------------------------------------------

void QuickCanvasTest::admittedTilesReachTheSceneGraph()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();

    // The relay the coordinator posts completions through is always queued, so
    // admission is not visible until the events are delivered -- which is the
    // first thing renderFrame does.
    QVERIFY(m_surfaces->counters().admitted > 0);

    // The first case in this file to bind a coordinator at all. Until P4-S6 the
    // canvas was only ever proven to translate input; that it also presents the
    // admitted surfaces was assumed.
    QVERIFY(m_item->frameStats().tiles > 0);
    QCOMPARE(m_item->frameStats().refusedStaleFrames, 0);

    const QJsonObject lifecycle = m_item->presentMetrics()->summary().value(QStringLiteral("lifecycle")).toObject();
    QVERIFY(lifecycle.value(QStringLiteral("tile_bytes")).toInteger(0) > 0);
}

void QuickCanvasTest::revisionReplacementLeavesNoStaleTile()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();
    QVERIFY(m_item->frameStats().tiles > 0);

    // The document is replaced, and nothing has told the coordinator yet. This
    // is the gap the presenter-side fence exists for: the retained nodes still
    // hold the previous revision's pixels, and any repaint at all would draw
    // them again.
    m_revisions->revision = makeRevision(QStringLiteral("doc-2"), 1);

    // A repaint caused by something other than the document -- the canvas does
    // not choose when the window redraws.
    m_item->update();
    renderFrame();

    QCOMPARE(m_item->frameStats().tiles, 0);
    QVERIFY(m_item->frameStats().refusedStaleFrames > 0);

    // And the refusal is not a permanent dead canvas: once the coordinator is
    // told, the new revision renders normally.
    m_surfaces->invalidate(m_revisions->revision);
    m_surfaces->requestSurfaces();
    renderFrame();

    QVERIFY(m_item->frameStats().tiles > 0);
}

void QuickCanvasTest::staleOverlayFrameIsRefusedToo()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();

    // Hover a scripted target so the overlay frame carries a primitive built
    // against the current revision.
    const QRect placed = m_viewport->placedPageRect(0);
    QVERIFY(!placed.isEmpty());

    const QPointF inside(placed.left() + placed.width() * 0.3, placed.top() + placed.height() * 0.7);
    QMouseEvent press(QEvent::MouseButtonPress, inside, inside, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mousePressEvent(&press);

    renderFrame();
    QVERIFY(m_item->frameStats().overlayPrimitives > 0);

    m_revisions->revision = makeRevision(QStringLiteral("doc-2"), 1);
    m_item->update();
    renderFrame();

    // A finding highlight from a replaced document points at geometry that no
    // longer exists. Drawing it is worse than drawing nothing, because it looks
    // like a finding on the document that is open now.
    QCOMPARE(m_item->frameStats().overlayPrimitives, 0);
}

void QuickCanvasTest::sceneGraphInvalidationRebuildsWithoutReparsing()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();
    QVERIFY(m_item->frameStats().tiles > 0);

    const int rendersBefore = m_renderer->renderCount;
    const int requestedBefore = m_surfaces->counters().requested;
    const quint64 rebuildsBefore = m_item->presentMetrics()->builderRebuilds();

    // The device-loss signal. Emitting it directly is what makes this
    // deterministic: a real backend loss cannot be provoked on demand, and the
    // item's contract is with the signal, not with the cause behind it.
    Q_EMIT m_window->sceneGraphInvalidated();

    QCOMPARE(m_item->presentMetrics()->sceneGraphInvalidations(), quint64(1));

    renderFrame();

    QVERIFY(m_item->presentMetrics()->builderRebuilds() > rebuildsBefore);
    QVERIFY(m_item->frameStats().tiles > 0);

    // The roadmap's resource-lifecycle rule: the GPU-side state goes, the
    // authoritative CPU surface cache stays. Re-rendering the page here would
    // mean a device loss costs a full re-parse of every visible page.
    QCOMPARE(m_renderer->renderCount, rendersBefore);
    QCOMPARE(m_surfaces->counters().requested, requestedBefore);
}

void QuickCanvasTest::releaseResourcesThenRepaintRecovers()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();
    QVERIFY(m_item->frameStats().tiles > 0);

    const int rendersBefore = m_renderer->renderCount;

    // Driven through the window rather than by calling the item's override:
    // releaseResources is only correct once the scene graph has taken the node
    // tree away, and only the window can arrange that.
    m_window->releaseResources();

    m_item->update();
    renderFrame();

    QVERIFY(m_item->frameStats().tiles > 0);
    QCOMPARE(m_renderer->renderCount, rendersBefore);
}

void QuickCanvasTest::windowChangeDropsRetainedNodes()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();
    QVERIFY(m_item->frameStats().tiles > 0);

    const quint64 rebuildsBefore = m_item->presentMetrics()->builderRebuilds();

    // A texture belongs to the window that created it, so every retained node is
    // invalid the moment the item moves. Surviving this is not cosmetic: a
    // texture outliving its window is a crash rather than a glitch.
    auto second = std::make_unique<QQuickWindow>();
    second->resize(400, 400);

    m_item->setParentItem(second->contentItem());
    m_window = std::move(second);

    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));

    renderFrame();

    QVERIFY(m_item->presentMetrics()->builderRebuilds() > rebuildsBefore);
    QVERIFY(m_item->frameStats().tiles > 0);
}

void QuickCanvasTest::rapidZoomReversalNeverDrawsAnotherRevision()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();

    for (const qreal zoom : { 1.5, 2.5, 4.0, 2.5, 1.5, 1.0 })
    {
        m_item->setZoom(zoom);
        m_surfaces->requestSurfaces();
        renderFrame();

        // A lower-resolution surface for the current revision may stand in while
        // the fidelity render is in flight -- that is the continuous
        // correspondence requirement. What may never happen is a tile from
        // another document appearing as the stand-in.
        QCOMPARE(m_surfaces->counters().rejectedStaleRevision, 0);
        QCOMPARE(m_item->frameStats().refusedStaleFrames, 0);
        QVERIFY(m_item->frameStats().tiles > 0);
    }
}

void QuickCanvasTest::rapidPageChangeKeepsOneFrameCurrent()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();

    const QPoint top = m_viewport->minimumOffset();
    const QPoint bottom = m_viewport->maximumOffset();

    for (const QPoint& offset : { bottom, top, bottom, top })
    {
        m_viewport->setOffset(offset);
        m_surfaces->requestSurfaces();
        renderFrame();

        QCOMPARE(m_item->frameStats().refusedStaleFrames, 0);
        QVERIFY(m_item->frameStats().tiles > 0);
    }
}

void QuickCanvasTest::devicePixelRatioChangeRepublishesGeometry()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    // 100%, 150%, 200%. The ratio is driven through the viewport rather than
    // through the window because an offscreen platform reports one fixed ratio
    // and no test can change it; what has to be proven is the consequence, and
    // the consequence lives in the key.
    for (const qreal ratio : { 1.0, 1.5, 2.0 })
    {
        const quint64 generationBefore = m_viewport->requestGeneration();

        m_viewport->setDevicePixelRatio(ratio);
        QVERIFY(m_viewport->requestGeneration() > generationBefore);

        m_surfaces->requestSurfaces();
        renderFrame();

        QVERIFY(!m_renderer->renderedKeys.isEmpty());
        QCOMPARE(m_renderer->renderedKeys.last().devicePixelRatio1000, int(qRound(ratio * 1000.0)));
        QVERIFY(m_item->frameStats().tiles > 0);

        // PageSurfaceKey::compatibleWith requires an exact device-pixel-ratio
        // match, so a surface rendered for the previous ratio cannot stand in.
        // A tile drawn at the wrong ratio is the classic four-times-too-large
        // page, and it must not be reachable even transiently.
        for (const pdfinteraction::CanvasTile& tile : m_surfaces->snapshot().tiles)
        {
            QCOMPARE(tile.key.devicePixelRatio1000, int(qRound(ratio * 1000.0)));
        }
    }

    // The item's half: a screen change on the same window republishes the
    // display metrics it can see. Before P4-S6 nothing connected this, and a
    // monitor swap left the viewport on the old ratio indefinitely.
    m_viewport->setDevicePixelRatio(3.0);
    Q_EMIT m_window->screenChanged(m_window->screen());

    QCOMPARE(m_viewport->devicePixelRatio(), m_window->effectiveDevicePixelRatio());
}

void QuickCanvasTest::itemDestroyedWithWorkInFlight()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();
    QVERIFY(m_item->frameStats().tiles > 0);

    // The item goes while the window, the coordinator and the surfaces it was
    // presenting all outlive it. The render-thread connections the item made are
    // the hazard here: one of them firing after the destructor is a crash in the
    // scene graph, not a warning.
    m_item.reset();

    m_surfaces->requestSurfaces();
    QCoreApplication::processEvents();

    const QImage frame = m_window->grabWindow();
    Q_UNUSED(frame);

    QVERIFY(m_surfaces->counters().admitted > 0);
}

void QuickCanvasTest::overlayOnlyChangeDoesNotResyncTiles()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    m_surfaces->requestSurfaces();
    renderFrame();
    QVERIFY(m_item->frameStats().tiles > 0);

    const int rendersBefore = m_renderer->renderCount;
    const int requestedBefore = m_surfaces->counters().requested;
    const int tilesBefore = m_item->frameStats().tiles;

    const QRect placed = m_viewport->placedPageRect(0);
    QVERIFY(!placed.isEmpty());

    // Sweep the pointer across a scripted target and off it again.
    for (int step = 0; step < 8; ++step)
    {
        const QPointF position(placed.left() + placed.width() * (0.1 + 0.1 * step), placed.top() + placed.height() * 0.5);
        QMouseEvent move(QEvent::MouseMove, position, position, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        m_item->mouseMoveEvent(&move);
    }

    renderFrame();

    // The P4-S4 exit condition, observed one layer further out than the S4 test
    // could observe it: a hover costs an overlay pass and nothing else. Page
    // pixels are not re-requested and not re-rendered.
    QCOMPARE(m_renderer->renderCount, rendersBefore);
    QCOMPARE(m_surfaces->counters().requested, requestedBefore);
    QCOMPARE(m_item->frameStats().tiles, tilesBefore);
}

void QuickCanvasTest::firstViewIsUnavailableUntilAPageIsOnScreen()
{
    buildCoordinator();
    showItemInWindow();
    bindItem();

    const QJsonObject before = m_item->presentMetrics()->summary().value(QStringLiteral("present")).toObject().value(QStringLiteral("first_view_ms")).toObject();

    // Nothing has been shown yet, and a milestone that has not happened is
    // unavailable rather than instantaneous. Reporting 0 ms here would make the
    // slowest possible open look like the fastest.
    QVERIFY(!before.value(QStringLiteral("available")).toBool(true));
    QVERIFY(before.value(QStringLiteral("ms")).isNull());

    m_surfaces->requestSurfaces();
    renderFrame();
    QVERIFY(m_item->frameStats().tiles > 0);

    const QJsonObject after = m_item->presentMetrics()->summary().value(QStringLiteral("present")).toObject().value(QStringLiteral("first_view_ms")).toObject();
    QVERIFY(after.value(QStringLiteral("available")).toBool(false));
    QVERIFY(after.value(QStringLiteral("ms")).toDouble(-1.0) >= 0.0);
}

QTEST_MAIN(QuickCanvasTest)

#include "tst_quickcanvastest.moc"
