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

#include <QJsonDocument>
#include <QJsonObject>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWheelEvent>

#include <memory>

#include "canvaspalette.h"
#include "canvaspresentmetrics.h"
#include "canvastraceoverlay.h"
#include "loupecanvasitem.h"

#include "hittestsource.h"
#include "interactioncontroller.h"
#include "interactiontrace.h"
#include "overlaybuilder.h"
#include "viewportcontroller.h"

#include "pdfdocumentcontext.h"

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

private:
    void bindItem();

    std::unique_ptr<FakeGeometrySource> m_geometry;
    std::unique_ptr<FakeRevisionSource> m_revisions;
    std::unique_ptr<pdfinteraction::ViewportController> m_viewport;
    std::unique_ptr<pdfinteraction::HitTestDispatcher> m_hitTest;
    std::unique_ptr<pdfinteraction::OverlayBuilder> m_overlays;
    std::unique_ptr<pdfinteraction::InteractionController> m_controller;
    std::unique_ptr<ScriptedHitTestSource> m_source;
    std::unique_ptr<ExposedCanvasItem> m_item;
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
    m_item->bind(m_viewport.get(), m_controller.get(), nullptr);
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
    const QPointF dragged = start + QPointF(40.0, 0.0);

    QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mousePressEvent(&press);

    QMouseEvent release(QEvent::MouseButtonRelease, start, start, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    m_item->mouseReleaseEvent(&release);

    QMouseEvent secondPress(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mousePressEvent(&secondPress);

    QMouseEvent move(QEvent::MouseMove, dragged, dragged, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    m_item->mouseMoveEvent(&move);

    QVERIFY(m_controller->state().drag().has_value());

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

QTEST_MAIN(QuickCanvasTest)

#include "tst_quickcanvastest.moc"
