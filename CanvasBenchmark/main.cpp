// SPDX-License-Identifier: MIT

#include <QApplication>
#include <QAccessible>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFocusEvent>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QList>
#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickWidget>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtMath>

#include <algorithm>
#include <cstdio>
#include <memory>

namespace
{

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kResizeIterations = 12;
const QColor kExpectedColor(QStringLiteral("#264d73"));

enum class Candidate
{
    Widget,
    QuickWidget,
    WindowContainer,
    QuickItem,
};

QString candidateName(Candidate candidate)
{
    switch (candidate)
    {
        case Candidate::Widget:
            return QStringLiteral("widget-baseline");
        case Candidate::QuickWidget:
            return QStringLiteral("qquickwidget");
        case Candidate::WindowContainer:
            return QStringLiteral("window-container");
        case Candidate::QuickItem:
            return QStringLiteral("quick-item");
    }
    return QStringLiteral("unknown");
}

QString graphicsApiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api)
    {
        case QSGRendererInterface::Software:
            return QStringLiteral("software");
        case QSGRendererInterface::OpenGL:
            return QStringLiteral("opengl");
        case QSGRendererInterface::Direct3D11:
            return QStringLiteral("d3d11");
        case QSGRendererInterface::Direct3D12:
            return QStringLiteral("d3d12");
        case QSGRendererInterface::Vulkan:
            return QStringLiteral("vulkan");
        case QSGRendererInterface::Metal:
            return QStringLiteral("metal");
        case QSGRendererInterface::Null:
            return QStringLiteral("null");
        case QSGRendererInterface::Unknown:
            return QStringLiteral("unknown");
    }
    return QStringLiteral("unrecognized");
}

Candidate parseCandidate(const QString& value)
{
    if (value == QStringLiteral("widget-baseline"))
    {
        return Candidate::Widget;
    }
    if (value == QStringLiteral("qquickwidget"))
    {
        return Candidate::QuickWidget;
    }
    if (value == QStringLiteral("window-container"))
    {
        return Candidate::WindowContainer;
    }
    if (value == QStringLiteral("quick-item"))
    {
        return Candidate::QuickItem;
    }
    return Candidate::Widget;
}

class WidgetCanvas final : public QWidget
{
public:
    explicit WidgetCanvas(QWidget* parent = nullptr) :
        QWidget(parent)
    {
        setObjectName(QStringLiteral("widgetCanvas"));
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), kExpectedColor);
        painter.fillRect(QRect(112, 88, 96, 64), QColor(QStringLiteral("#d7e8f7")));
        painter.setPen(QColor(QStringLiteral("#102a43")));
        painter.drawRect(QRect(112, 88, 96, 64));
    }
};

class BenchmarkItem final : public QQuickItem
{
public:
    explicit BenchmarkItem(QQuickItem* parent = nullptr) :
        QQuickItem(parent)
    {
        setFlag(ItemHasContents, true);
        setFlag(ItemIsFocusScope, true);
        setFlag(ItemAcceptsInputMethod, false);
        setFocus(true);
        setActiveFocusOnTab(true);
    }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override
    {
        auto* node = static_cast<QSGSimpleRectNode*>(oldNode);
        if (!node)
        {
            node = new QSGSimpleRectNode;
        }
        node->setRect(boundingRect());
        node->setColor(kExpectedColor);
        return node;
    }
};

class InputProbe final : public QObject
{
public:
    bool received = false;

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress)
        {
            received = true;
        }
        return false;
    }
};

void processEvents()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::sendPostedEvents();
}

void sendTab(QWidget* receiver, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    if (!receiver)
    {
        return;
    }

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Tab, modifiers);
    QCoreApplication::sendEvent(receiver, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Tab, modifiers);
    QCoreApplication::sendEvent(receiver, &release);
    processEvents();
}

bool hasExpectedColor(const QImage& image)
{
    if (image.isNull())
    {
        return false;
    }
    const QColor sample = image.pixelColor(8, 8);
    return qAbs(sample.red() - kExpectedColor.red()) <= 2 &&
           qAbs(sample.green() - kExpectedColor.green()) <= 2 &&
           qAbs(sample.blue() - kExpectedColor.blue()) <= 2;
}

QJsonObject runWidgetBaseline()
{
    WidgetCanvas canvas;
    canvas.resize(kWidth, kHeight);
    canvas.show();
    processEvents();

    InputProbe probe;
    canvas.installEventFilter(&probe);
    canvas.setFocus();
    QKeyEvent keyEvent(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &keyEvent);
    processEvents();

    QElapsedTimer timer;
    timer.start();
    for (int index = 0; index < kResizeIterations; ++index)
    {
        canvas.resize(kWidth + index * 7, kHeight + index * 5);
        processEvents();
    }

    QJsonObject result;
    result.insert(QStringLiteral("candidate"), candidateName(Candidate::Widget));
    result.insert(QStringLiteral("resize_ms"), timer.elapsed());
    result.insert(QStringLiteral("input_received"), probe.received);
    result.insert(QStringLiteral("focus_reached"), canvas.hasFocus());
    result.insert(QStringLiteral("color_match"), hasExpectedColor(canvas.grab().toImage()));
    result.insert(QStringLiteral("dpi"), canvas.devicePixelRatioF());
    result.insert(QStringLiteral("graphics_api"), QStringLiteral("widgets"));
    result.insert(QStringLiteral("status"), probe.received && canvas.hasFocus() &&
                                                    result.value(QStringLiteral("color_match")).toBool() &&
                                                    canvas.devicePixelRatioF() > 0.0
                                                ? QStringLiteral("pass")
                                                : QStringLiteral("partial"));
    return result;
}

QJsonObject runQuickCandidate(Candidate candidate)
{
    std::unique_ptr<QWidget> widgetHost;
    std::unique_ptr<QQuickWidget> quickWidget;
    std::unique_ptr<QQuickWindow> quickWindow;
    QQuickItem* focusItem = nullptr;
    QWidget* focusWidget = nullptr;
    QObject* inputTarget = nullptr;
    QQuickWindow* window = nullptr;
    QImage captured;
    qreal dpi = 1.0;
    QString graphicsApi = QStringLiteral("unknown");

    if (candidate == Candidate::QuickWidget)
    {
        widgetHost = std::make_unique<QWidget>();
        widgetHost->setObjectName(QStringLiteral("qquickWidgetHost"));
        quickWidget = std::make_unique<QQuickWidget>(widgetHost.get());
        quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        quickWidget->setSource(QUrl(QStringLiteral("qrc:/qt/qml/Loop/CanvasBenchmark/CanvasBenchmark.qml")));
        quickWidget->setGeometry(0, 0, kWidth, kHeight);
        widgetHost->resize(kWidth, kHeight);
        widgetHost->show();
        focusWidget = quickWidget.get();
        inputTarget = quickWidget.get();
    }
    else
    {
        quickWindow = std::make_unique<QQuickWindow>();
        window = quickWindow.get();
        quickWindow->setColor(Qt::transparent);
        auto* item = new BenchmarkItem(quickWindow->contentItem());
        item->setSize(QSize(kWidth, kHeight));
        focusItem = item;
        inputTarget = quickWindow.get();

        if (candidate == Candidate::WindowContainer)
        {
            widgetHost = std::make_unique<QWidget>();
            auto* container = QWidget::createWindowContainer(quickWindow.get(), widgetHost.get());
            container->setFocusPolicy(Qt::StrongFocus);
            container->setGeometry(0, 0, kWidth, kHeight);
            widgetHost->resize(kWidth, kHeight);
            widgetHost->show();
            focusWidget = container;
            quickWindow.release();
        }
        else
        {
            quickWindow->resize(kWidth, kHeight);
            quickWindow->show();
        }
    }

    processEvents();
    if (quickWidget)
    {
        window = quickWidget->quickWindow();
    }
    if (window && window->rendererInterface())
    {
        graphicsApi = graphicsApiName(window->rendererInterface()->graphicsApi());
    }
    dpi = window ? window->devicePixelRatio() : 1.0;

    InputProbe probe;
    if (inputTarget)
    {
        inputTarget->installEventFilter(&probe);
    }
    if (focusItem)
    {
        focusItem->forceActiveFocus();
    }
    else if (quickWidget)
    {
        quickWidget->setFocus();
    }
    else if (focusWidget)
    {
        focusWidget->setFocus();
    }
    QKeyEvent keyEvent(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    if (inputTarget)
    {
        QCoreApplication::sendEvent(inputTarget, &keyEvent);
    }
    processEvents();

    QElapsedTimer timer;
    timer.start();
    for (int index = 0; index < kResizeIterations; ++index)
    {
        const QSize size(kWidth + index * 7, kHeight + index * 5);
        if (quickWidget)
        {
            widgetHost->resize(size);
            quickWidget->resize(size);
        }
        else if (candidate == Candidate::WindowContainer)
        {
            widgetHost->resize(size);
        }
        else if (window)
        {
            window->resize(size);
        }
        processEvents();
    }

    if (quickWidget)
    {
        captured = quickWidget->grabFramebuffer();
    }
    else if (window)
    {
        captured = window->grabWindow();
    }

    QJsonObject result;
    result.insert(QStringLiteral("candidate"), candidateName(candidate));
    result.insert(QStringLiteral("resize_ms"), timer.elapsed());
    result.insert(QStringLiteral("input_received"), probe.received);
    const bool focusReached = focusItem ? focusItem->hasActiveFocus()
                                        : (focusWidget && focusWidget->hasFocus());
    const bool colorMatch = hasExpectedColor(captured);
    const bool graphicsApiKnown = graphicsApi != QStringLiteral("unknown") &&
                                  graphicsApi != QStringLiteral("unrecognized");
    result.insert(QStringLiteral("focus_reached"), focusReached);
    result.insert(QStringLiteral("color_match"), colorMatch);
    result.insert(QStringLiteral("dpi"), dpi);
    result.insert(QStringLiteral("graphics_api"), graphicsApi);
    result.insert(QStringLiteral("status"), probe.received && focusReached && colorMatch &&
                                                    dpi > 0.0 && graphicsApiKnown
                                                ? QStringLiteral("pass")
                                                : QStringLiteral("partial"));
    return result;
}

QJsonObject runFocusBridgeProbe()
{
    QWidget host;
    host.setObjectName(QStringLiteral("focusBridgeHost"));
    host.setFocusPolicy(Qt::StrongFocus);

    QLineEdit before;
    before.setObjectName(QStringLiteral("widgetBeforeQuick"));
    before.setAccessibleName(QStringLiteral("Widget before Quick"));

    QQuickWidget quickWidget;
    quickWidget.setObjectName(QStringLiteral("quickBridge"));
    quickWidget.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quickWidget.setFocusPolicy(Qt::StrongFocus);
    quickWidget.setSource(QUrl(QStringLiteral("qrc:/qt/qml/Loop/CanvasBenchmark/CanvasBenchmark.qml")));

    QLineEdit after;
    after.setObjectName(QStringLiteral("widgetAfterQuick"));
    after.setAccessibleName(QStringLiteral("Widget after Quick"));

    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(&before);
    layout->addWidget(&quickWidget);
    layout->addWidget(&after);
    QWidget::setTabOrder(&before, &quickWidget);
    QWidget::setTabOrder(&quickWidget, &after);

    host.resize(kWidth, kHeight + 96);
    host.show();
    host.activateWindow();
    processEvents();

    QObject* rootObject = quickWidget.rootObject();
    const bool qmlLoaded = quickWidget.status() == QQuickWidget::Ready && rootObject;
    const QString accessibleName = rootObject
                                       ? rootObject->property("bridgeAccessibleName").toString()
                                       : QString();
    const QString accessibleDescription = rootObject
                                              ? rootObject->property("bridgeAccessibleDescription").toString()
                                              : QString();
    const int accessibleRole = rootObject
                                   ? rootObject->property("bridgeAccessibleRole").toInt()
                                   : static_cast<int>(QAccessible::NoRole);

    before.setFocus(Qt::OtherFocusReason);
    processEvents();
    const bool initialWidgetFocus = before.hasFocus();

    sendTab(&before);
    const bool widgetToQuick = quickWidget.hasFocus() && rootObject && rootObject->property("bridgeProbeActiveFocus").toBool();

    sendTab(&quickWidget);
    const bool quickToWidget = after.hasFocus();

    after.setFocus(Qt::OtherFocusReason);
    processEvents();
    sendTab(&after, Qt::ShiftModifier);
    const bool widgetToQuickReverse = quickWidget.hasFocus() && rootObject && rootObject->property("bridgeProbeActiveFocus").toBool();

    sendTab(&quickWidget, Qt::ShiftModifier);
    const bool quickToWidgetReverse = before.hasFocus();

    const bool accessibilityContract = qmlLoaded && accessibleName == QStringLiteral("Quick action") && accessibleDescription == QStringLiteral("Activate the Quick action.") && accessibleRole == static_cast<int>(QAccessible::Button);
    const bool roundTrip = initialWidgetFocus && widgetToQuick && quickToWidget && widgetToQuickReverse && quickToWidgetReverse;

    QJsonObject result;
    result.insert(QStringLiteral("probe"), QStringLiteral("widget-quick-widget-focus-accessibility"));
    result.insert(QStringLiteral("qml_loaded"), qmlLoaded);
    result.insert(QStringLiteral("widget_to_quick"), widgetToQuick);
    result.insert(QStringLiteral("quick_to_widget"), quickToWidget);
    result.insert(QStringLiteral("widget_to_quick_reverse"), widgetToQuickReverse);
    result.insert(QStringLiteral("quick_to_widget_reverse"), quickToWidgetReverse);
    result.insert(QStringLiteral("accessibility_name"), accessibleName);
    result.insert(QStringLiteral("accessibility_description"), accessibleDescription);
    result.insert(QStringLiteral("accessibility_role"), accessibleRole);
    result.insert(QStringLiteral("accessibility_contract"), accessibilityContract);
    result.insert(QStringLiteral("native_accessibility_backend_active"), QAccessible::isActive());
    result.insert(QStringLiteral("status"), roundTrip && accessibilityContract
                                                ? QStringLiteral("pass")
                                                : QStringLiteral("partial"));
    return result;
}

}   // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QString requested = QStringLiteral("all");
    for (int index = 1; index < argc; ++index)
    {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QStringLiteral("--focus-bridge"))
        {
            requested = QStringLiteral("focus-bridge");
        }
        else if (argument.startsWith(QStringLiteral("--candidate=")))
        {
            requested = argument.sliced(QStringLiteral("--candidate=").size());
        }
    }

    QList<QJsonObject> results;
    if (requested == QStringLiteral("focus-bridge"))
    {
        results.append(runFocusBridgeProbe());
    }
    else if (requested == QStringLiteral("all"))
    {
        results.append(runWidgetBaseline());
        results.append(runQuickCandidate(Candidate::QuickWidget));
        results.append(runQuickCandidate(Candidate::WindowContainer));
        results.append(runQuickCandidate(Candidate::QuickItem));
    }
    else if (requested == QStringLiteral("widget-baseline"))
    {
        results.append(runWidgetBaseline());
    }
    else
    {
        results.append(runQuickCandidate(parseCandidate(requested)));
    }

    bool complete = true;
    for (const QJsonObject& result : results)
    {
        const QByteArray line = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
        std::fwrite("\n", 1, 1, stdout);
        complete = complete && result.value(QStringLiteral("status")).toString() == QStringLiteral("pass");
    }
    std::fflush(stdout);
    return complete ? 0 : 2;
}
