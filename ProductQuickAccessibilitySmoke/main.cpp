#include "editorhost.h"

#include "loupecanvasitem.h"

#include <QAccessible>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <cstdio>

namespace
{

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

bool verifyCanvasAccessibility(QQuickWindow* window)
{
    if (!window)
    {
        return false;
    }

    const QList<pdfquick::LoupeCanvasItem*> canvases = window->findChildren<pdfquick::LoupeCanvasItem*>();
    if (canvases.isEmpty())
    {
        fprintf(stderr, "product-quick-a11y-smoke canvas item not found\n");
        return false;
    }

    pdfquick::LoupeCanvasItem* canvas = canvases.front();
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(canvas);
    if (!iface)
    {
        fprintf(stderr, "product-quick-a11y-smoke missing canvas accessible interface\n");
        return false;
    }

    const bool hasName = !iface->text(QAccessible::Name).trimmed().isEmpty();
    const bool hasDescription = !iface->text(QAccessible::Description).trimmed().isEmpty();
    const bool canvasRole = iface->role() == QAccessible::Canvas;
    const bool noTileChildren = iface->childCount() == 0;

    fprintf(stdout,
            "product-quick-a11y-smoke canvas_accessible name=%d description=%d role_canvas=%d child_count=%d\n",
            hasName ? 1 : 0,
            hasDescription ? 1 : 0,
            canvasRole ? 1 : 0,
            iface->childCount());

    return hasName && hasDescription && canvasRole && noTileChildren;
}

}   // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    EditorHost host;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("editorHost"), &host);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &application,
                     [&application](QObject* object, const QUrl& url)
                     {
                         if (object)
                         {
                             return;
                         }

                         fprintf(stderr, "product-quick-a11y-smoke qml_load_failed url=%s\n",
                                 url.toString().toLocal8Bit().constData());
                         application.exit(2);
                     });

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &application,
                     [&application, &host](QObject* object, const QUrl&)
                     {
                         auto* window = qobject_cast<QQuickWindow*>(object);
                         if (!window)
                         {
                             return;
                         }

                         QObject::connect(
                             window, &QQuickWindow::sceneGraphInitialized, &application,
                             [window, &application, &host]()
                             {
                                 const auto* renderer = window->rendererInterface();
                                 const auto api = renderer ? renderer->graphicsApi() : QSGRendererInterface::Unknown;
                                 fprintf(stdout,
                                         "product-quick-a11y-smoke scene_graph_initialized graphics_api=%s native_accessibility_backend_active=%d\n",
                                         graphicsApiName(api).toLocal8Bit().constData(),
                                         QAccessible::isActive() ? 1 : 0);
                                 fflush(stdout);

                                 const bool focusHelper = host.focusRestoration() != nullptr;
                                 const bool canvasAccessible = verifyCanvasAccessibility(window);
                                 const bool passed = api != QSGRendererInterface::Unknown && focusHelper && canvasAccessible;

                                 fprintf(stdout, "product-quick-a11y-smoke status=%s\n", passed ? "pass" : "fail");
                                 fflush(stdout);
                                 application.exit(passed ? 0 : 5);
                             },
                             Qt::DirectConnection);
                     });

    engine.loadFromModule(QStringLiteral("Loupe.Quick"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty())
    {
        return 2;
    }

    QTimer::singleShot(10000, &application, [&application]()
                       { application.exit(4); });

    return application.exec();
}
