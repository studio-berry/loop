#include "pdfapplicationidentity.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
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

QString environmentValue(const char* name)
{
    const QByteArray value = qgetenv(name);
    return value.isEmpty() ? QStringLiteral("<unset>") : QString::fromLocal8Bit(value);
}

}   // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::QuickShellSmoke);
    QQmlApplicationEngine engine;

    const QUrl qmlUrl(QStringLiteral("qrc:/qt/qml/Loop/QuickShellSmoke/QuickShellSmoke.qml"));

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                     [&app](QObject* object, const QUrl& url)
                     {
                         if (object)
                         {
                             return;
                         }

                         fprintf(stderr, "quick-shell-smoke qml_load_failed url=%s\n",
                                 url.toString().toLocal8Bit().constData());
                         qCritical().noquote()
                             << "quick-shell-smoke qml_load_failed url=" << url.toString();
                         app.exit(2);
                     });

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                     [&app](QObject* object, const QUrl&)
                     {
                         auto* window = qobject_cast<QQuickWindow*>(object);
                         if (!window)
                         {
                             return;
                         }

                         QObject::connect(
                             window, &QQuickWindow::sceneGraphInitialized, &app,
                             [window, &app]()
                             {
                                 const auto* renderer = window->rendererInterface();
                                 const auto api = renderer
                                                      ? renderer->graphicsApi()
                                                      : QSGRendererInterface::Unknown;
                                 const QByteArray apiText = graphicsApiName(api).toLocal8Bit();
                                 const QByteArray quickBackend =
                                     environmentValue("QT_QUICK_BACKEND").toLocal8Bit();
                                 const QByteArray warpPreference =
                                     environmentValue("QSG_RHI_PREFER_SOFTWARE_RENDERER").toLocal8Bit();
                                 const QByteArray qpaPlatform =
                                     environmentValue("QT_QPA_PLATFORM").toLocal8Bit();

                                 fprintf(stdout,
                                         "quick-shell-smoke scene_graph_initialized graphics_api=%s "
                                         "graphics_api_value=%d QT_QUICK_BACKEND=%s "
                                         "QSG_RHI_PREFER_SOFTWARE_RENDERER=%s QT_QPA_PLATFORM=%s\n",
                                         apiText.constData(), static_cast<int>(api),
                                         quickBackend.constData(), warpPreference.constData(),
                                         qpaPlatform.constData());
                                 fflush(stdout);

                                 qInfo().noquote()
                                     << "quick-shell-smoke scene_graph_initialized"
                                     << "graphics_api=" << graphicsApiName(api)
                                     << "graphics_api_value=" << static_cast<int>(api)
                                     << "QT_QUICK_BACKEND="
                                     << environmentValue("QT_QUICK_BACKEND")
                                     << "QSG_RHI_PREFER_SOFTWARE_RENDERER="
                                     << environmentValue("QSG_RHI_PREFER_SOFTWARE_RENDERER")
                                     << "QT_QPA_PLATFORM=" << environmentValue("QT_QPA_PLATFORM");

                                 if (api == QSGRendererInterface::Unknown)
                                 {
                                     qCritical() << "quick-shell-smoke scene graph has no selected renderer";
                                     QMetaObject::invokeMethod(
                                         &app, [&app]()
                                         { app.exit(3); }, Qt::QueuedConnection);
                                     return;
                                 }

                                 QMetaObject::invokeMethod(
                                     &app, [&app]()
                                     { app.exit(0); }, Qt::QueuedConnection);
                             },
                             Qt::DirectConnection);
                     });

    engine.load(qmlUrl);

    if (engine.rootObjects().isEmpty())
    {
        qCritical() << "quick-shell-smoke has no QML root object";
        return 2;
    }

    QTimer::singleShot(10000, &app, [&app]()
                       {
        qCritical() << "quick-shell-smoke timed out before scene graph initialization";
        app.exit(4); });

    return app.exec();
}
