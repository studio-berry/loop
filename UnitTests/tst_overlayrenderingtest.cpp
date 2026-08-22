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

#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentdrawinterface.h"
#include "pdfdrawwidget.h"
#include "pdfdrawspacecontroller.h"
#include "pdfinteractiontrace_p.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QPainter>

#include <memory>
#include <set>
#include <utility>

#include <QtTest>

namespace
{

class RecordingOverlay final : public pdf::IDocumentDrawInterface, public pdf::IDocumentOverlayInterface
{
public:
    RecordingOverlay(QString name,
                     pdf::PDFOverlayLayer layer,
                     QVector<QString>* callOrder,
                     QRectF geometry = QRectF(10.0, 10.0, 30.0, 30.0)) :
        m_name(std::move(name)),
        m_layer(layer),
        m_callOrder(callOrder),
        m_geometry(geometry)
    {
    }

    pdf::PDFOverlayLayer getOverlayLayer() const override { return m_layer; }

    void drawOverlay(QPainter* painter, const pdf::PDFOverlayContext& context) const override
    {
        ++m_calls;
        if (!context.renderable || !context.page || context.pageIndex < 0)
        {
            ++m_invalidContexts;
            return;
        }

        if (m_callOrder)
        {
            m_callOrder->push_back(m_name);
        }

        m_pageIndices.insert(context.pageIndex);
        m_lastMatrix = context.pagePointToDevicePointMatrix;
        m_lastContext = context;

        if (!m_geometry.isValid() || m_geometry.isEmpty())
        {
            ++m_notRenderableGeometry;
            return;
        }

        const QRectF deviceRect = context.pagePointToDevicePointMatrix.mapRect(m_geometry).normalized();
        if (!deviceRect.intersects(context.viewportRect))
        {
            ++m_clippedGeometry;
            return;
        }

        painter->setPen(Qt::NoPen);
        painter->setBrush(m_color);
        for (int index = 0; index < m_markerCount; ++index)
        {
            const qreal offset = static_cast<qreal>(index % 32) * 2.0;
            painter->drawRect(deviceRect.translated(offset, static_cast<qreal>(index / 32) * 2.0));
        }
    }

    void setColor(QColor color) const { m_color = std::move(color); }
    void setGeometry(QRectF geometry) const { m_geometry = geometry; }
    void setMarkerCount(int count) const { m_markerCount = qMax(0, count); }

    int calls() const { return m_calls; }
    int invalidContexts() const { return m_invalidContexts; }
    int notRenderableGeometry() const { return m_notRenderableGeometry; }
    int clippedGeometry() const { return m_clippedGeometry; }
    const QTransform& lastMatrix() const { return m_lastMatrix; }
    const pdf::PDFOverlayContext& lastContext() const { return m_lastContext; }
    const std::set<pdf::PDFInteger>& pageIndices() const { return m_pageIndices; }

private:
    QString m_name;
    pdf::PDFOverlayLayer m_layer;
    QVector<QString>* m_callOrder = nullptr;
    mutable QRectF m_geometry;
    mutable QColor m_color = QColor(255, 0, 0, 160);
    mutable int m_markerCount = 1;
    mutable int m_calls = 0;
    mutable int m_invalidContexts = 0;
    mutable int m_notRenderableGeometry = 0;
    mutable int m_clippedGeometry = 0;
    mutable QTransform m_lastMatrix;
    mutable pdf::PDFOverlayContext m_lastContext;
    mutable std::set<pdf::PDFInteger> m_pageIndices;
};

class OverlayRenderingTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void ordersAndTransformsIndependentOverlays();
    void overlayOnlyUpdatePreservesPageSurfaceCache();
    void invalidAndHighCountGeometryRemainBounded();
    void denyExtraGraphicsSuppressesOverlays();

private:
    void setDocument(int pageCount);

    std::unique_ptr<pdf::PDFCMSManager> m_cmsManager;
    std::unique_ptr<pdf::PDFWidget> m_widget;
    std::unique_ptr<pdf::PDFDocument> m_document;
};

void OverlayRenderingTest::init()
{
    m_cmsManager = std::make_unique<pdf::PDFCMSManager>(nullptr);
    m_widget = std::make_unique<pdf::PDFWidget>(m_cmsManager.get(), pdf::RendererEngine::Blend2D_MultiThread, nullptr);
    m_widget->resize(360, 280);
    m_widget->show();
    QCoreApplication::processEvents();
}

void OverlayRenderingTest::cleanup()
{
    m_widget.reset();
    m_document.reset();
    m_cmsManager.reset();
}

void OverlayRenderingTest::setDocument(int pageCount)
{
    pdf::PDFDocumentBuilder builder;
    for (int index = 0; index < pageCount; ++index)
    {
        builder.appendPage(QRectF(0, 0, 200, 200));
    }

    m_document = std::make_unique<pdf::PDFDocument>(builder.build());
    m_widget->setDocument(pdf::PDFModifiedDocument(m_document.get(), nullptr), {});
    m_widget->updateCacheLimits(4 * 1024 * 1024, 16384, 128, 128);
    pdf::PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    proxy->setFeatures(proxy->getFeatures() | pdf::PDFRenderer::DisplayTimes);
}

void OverlayRenderingTest::ordersAndTransformsIndependentOverlays()
{
    setDocument(2);

    QVector<QString> callOrder;
    RecordingOverlay guides(QStringLiteral("guides"), pdf::PDFOverlayLayer::Guides, &callOrder);
    RecordingOverlay selection(QStringLiteral("selection"), pdf::PDFOverlayLayer::Selection, &callOrder);
    pdf::PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    proxy->registerDrawInterface(&selection);
    proxy->registerDrawInterface(&guides);

    QWidget* canvas = m_widget->getDrawWidget()->getWidget();
    QVERIFY(canvas != nullptr);
    const QImage first = canvas->grab().toImage();
    QVERIFY(!first.isNull());
    QVERIFY(guides.calls() > 0);
    QVERIFY(selection.calls() > 0);
    QCOMPARE(guides.invalidContexts(), 0);
    QCOMPARE(selection.invalidContexts(), 0);

    for (qsizetype index = 0; index + 1 < callOrder.size(); index += 2)
    {
        QCOMPARE(callOrder.at(index), QStringLiteral("guides"));
        QCOMPARE(callOrder.at(index + 1), QStringLiteral("selection"));
    }

    QVERIFY(guides.lastContext().pageRect.isValid());
    QVERIFY(guides.lastContext().viewportRect.contains(guides.lastContext().pageRect.center())
            || guides.lastContext().pageRect.intersects(guides.lastContext().viewportRect));
    QVERIFY(!guides.lastMatrix().isIdentity());

    proxy->performOperation(pdf::PDFDrawWidgetProxy::RotateRight);
    const QImage rotated = canvas->grab().toImage();
    QVERIFY(!rotated.isNull());
    QVERIFY(!guides.lastMatrix().isIdentity());

    proxy->performOperation(pdf::PDFDrawWidgetProxy::NavigateNextPage);
    const QImage nextPage = canvas->grab().toImage();
    QVERIFY(!nextPage.isNull());
    QVERIFY(guides.pageIndices().contains(1));

    proxy->unregisterDrawInterface(&selection);
    proxy->unregisterDrawInterface(&guides);
}

void OverlayRenderingTest::overlayOnlyUpdatePreservesPageSurfaceCache()
{
    setDocument(1);

    QVector<QString> callOrder;
    RecordingOverlay overlay(QStringLiteral("overlay"), pdf::PDFOverlayLayer::Findings, &callOrder);
    pdf::PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    proxy->registerDrawInterface(&overlay);

    QWidget* canvas = m_widget->getDrawWidget()->getWidget();
    QVERIFY(canvas != nullptr);
    QSignalSpy pageSpy(proxy, &pdf::PDFDrawWidgetProxy::pageImageChanged);
    const QImage first = canvas->grab().toImage();
    QVERIFY(!first.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(pageSpy.count() > 0, 10000);
    const QImage warm = canvas->grab().toImage();
    QVERIFY(!warm.isNull());

    QObject* recorderObject = canvas->findChild<QObject*>(QStringLiteral("LoupeInteractionTraceRecorder"));
    QVERIFY(recorderObject != nullptr);
    const auto* recorder = static_cast<const pdf::PDFInteractionTraceRecorder*>(recorderObject);
    const QJsonObject before = recorder->summary();
    const QJsonObject beforeCache = before.value(QStringLiteral("cache")).toObject();
    const int missesBefore = beforeCache.value(QStringLiteral("surface_misses")).toInt();
    QVERIFY(before.value(QStringLiteral("stage_time_ms")).toObject().contains(QStringLiteral("overlays")));

    overlay.setColor(QColor(0, 0, 255, 180));
    canvas->update();
    const QImage changed = canvas->grab().toImage();
    QVERIFY(!changed.isNull());

    const QJsonObject afterCache = recorder->summary().value(QStringLiteral("cache")).toObject();
    QCOMPARE(afterCache.value(QStringLiteral("surface_misses")).toInt(), missesBefore);
    QVERIFY(afterCache.value(QStringLiteral("surface_hits")).toInt() > 0);
    QVERIFY(overlay.calls() >= 3);

    proxy->unregisterDrawInterface(&overlay);
}

void OverlayRenderingTest::invalidAndHighCountGeometryRemainBounded()
{
    setDocument(1);

    QVector<QString> callOrder;
    RecordingOverlay overlay(QStringLiteral("markers"), pdf::PDFOverlayLayer::Findings, &callOrder, QRectF());
    overlay.setMarkerCount(2048);
    pdf::PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    proxy->registerDrawInterface(&overlay);

    QWidget* canvas = m_widget->getDrawWidget()->getWidget();
    QVERIFY(canvas != nullptr);
    const QImage invalid = canvas->grab().toImage();
    QVERIFY(!invalid.isNull());
    QVERIFY(overlay.calls() > 0);
    QVERIFY(overlay.notRenderableGeometry() > 0);
    QCOMPARE(overlay.clippedGeometry(), 0);

    overlay.setGeometry(QRectF(1000, 1000, 30, 30));
    const QImage offPage = canvas->grab().toImage();
    QVERIFY(!offPage.isNull());
    QVERIFY(overlay.clippedGeometry() > 0);

    overlay.setGeometry(QRectF(10, 10, 30, 30));
    const QImage manyMarkers = canvas->grab().toImage();
    QVERIFY(!manyMarkers.isNull());
    QVERIFY(overlay.calls() > overlay.notRenderableGeometry());

    proxy->unregisterDrawInterface(&overlay);
}

void OverlayRenderingTest::denyExtraGraphicsSuppressesOverlays()
{
    setDocument(1);

    RecordingOverlay overlay(QStringLiteral("suppressed"), pdf::PDFOverlayLayer::Findings, nullptr);
    pdf::PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    proxy->setFeatures(proxy->getFeatures() | pdf::PDFRenderer::DenyExtraGraphics);
    proxy->registerDrawInterface(&overlay);

    QWidget* canvas = m_widget->getDrawWidget()->getWidget();
    QVERIFY(canvas != nullptr);
    const QImage image = canvas->grab().toImage();
    QVERIFY(!image.isNull());
    QCOMPARE(overlay.calls(), 0);

    proxy->unregisterDrawInterface(&overlay);
}

}   // namespace

QTEST_MAIN(OverlayRenderingTest)

#include "tst_overlayrenderingtest.moc"
