#include "editorhost.h"

#include "pdfapplicationidentity.h"
#include "loopcanvasitem.h"

#include <QAccessible>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml/qqml.h>
#include <QQuickStyle>
#include <QQuickItem>
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

    const QList<pdfquick::LoopCanvasItem*> canvases = window->findChildren<pdfquick::LoopCanvasItem*>();
    if (canvases.isEmpty())
    {
        fprintf(stderr, "product-quick-a11y-smoke canvas item not found\n");
        return false;
    }

    pdfquick::LoopCanvasItem* canvas = canvases.front();
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

bool verifyPreflightAccessibility(QQuickWindow* window)
{
    if (!window)
    {
        return false;
    }

    // #195 acceptance 1: the preflight workflow surface must be reachable and named. The pane owns
    // the objectName; everything the operator reads off it comes from EditorHost.
    QQuickItem* pane = window->findChild<QQuickItem*>(QStringLiteral("preflightPane"));
    if (!pane)
    {
        fprintf(stderr, "product-quick-a11y-smoke preflight_pane_missing\n");
        return false;
    }

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(pane);
    if (!iface)
    {
        fprintf(stderr, "product-quick-a11y-smoke preflight_pane_has_no_accessible_interface\n");
        return false;
    }

    const bool hasName = !iface->text(QAccessible::Name).trimmed().isEmpty();
    const bool hasDescription = !iface->text(QAccessible::Description).trimmed().isEmpty();
    const bool groupingRole = iface->role() == QAccessible::Grouping;

    fprintf(stdout,
            "product-quick-a11y-smoke preflight_accessible name=%d description=%d role_grouping=%d\n",
            hasName ? 1 : 0,
            hasDescription ? 1 : 0,
            groupingRole ? 1 : 0);

    // Every boolean above is folded into the result: a pane that loses its description or its
    // Grouping role must fail this smoke, not merely print a 0.
    if (!hasName)
    {
        fprintf(stderr, "product-quick-a11y-smoke preflight_pane_not_accessible\n");
    }
    if (!hasDescription)
    {
        fprintf(stderr, "product-quick-a11y-smoke preflight_pane_description_missing\n");
    }
    if (!groupingRole)
    {
        fprintf(stderr, "product-quick-a11y-smoke preflight_pane_not_grouping_role\n");
    }

    return hasName && hasDescription && groupingRole;
}

bool verifyNamedAccessibility(QQuickWindow* window,
                              const QString& objectName,
                              QAccessible::Role expectedRole,
                              bool requiresDescription)
{
    if (!window)
    {
        return false;
    }

    QQuickItem* item = window->findChild<QQuickItem*>(objectName);
    if (!item)
    {
        fprintf(stderr, "product-quick-a11y-smoke object_missing name=%s\n",
                objectName.toLocal8Bit().constData());
        return false;
    }

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(item);
    if (!iface)
    {
        fprintf(stderr, "product-quick-a11y-smoke accessible_interface_missing name=%s\n",
                objectName.toLocal8Bit().constData());
        return false;
    }

    const bool hasName = !iface->text(QAccessible::Name).trimmed().isEmpty();
    const bool hasDescription = !iface->text(QAccessible::Description).trimmed().isEmpty();
    const bool roleMatches = iface->role() == expectedRole;
    const bool passed = hasName && roleMatches && (!requiresDescription || hasDescription);

    fprintf(stdout,
            "product-quick-a11y-smoke accessible name=%s has_name=%d has_description=%d role=%d expected_role=%d pass=%d\n",
            objectName.toLocal8Bit().constData(),
            hasName ? 1 : 0,
            hasDescription ? 1 : 0,
            static_cast<int>(iface->role()),
            static_cast<int>(expectedRole),
            passed ? 1 : 0);
    return passed;
}

bool verifyKeyboardSurface(QQuickWindow* window, EditorHost& host)
{
    if (!window)
    {
        return false;
    }

    const QStringList focusTargets = {
        QStringLiteral("shellToolBar"),
        QStringLiteral("openDocumentButton"),
        QStringLiteral("workspaceRail"),
        QStringLiteral("pagesView"),
        QStringLiteral("inspectorView"),
        QStringLiteral("preflightFindingsView"),
    };

    bool allTabReachable = true;
    for (const QString& name : focusTargets)
    {
        QQuickItem* item = window->findChild<QQuickItem*>(name);
        const bool reachable = item && item->activeFocusOnTab();
        fprintf(stdout,
                "product-quick-a11y-smoke focus_target name=%s active_focus_on_tab=%d\n",
                name.toLocal8Bit().constData(),
                reachable ? 1 : 0);
        allTabReachable = allTabReachable && reachable;
    }

    QQuickItem* first = window->findChild<QQuickItem*>(QStringLiteral("openDocumentButton"));
    QQuickItem* second = window->findChild<QQuickItem*>(QStringLiteral("pagesView"));
    if (!first || !second)
    {
        return false;
    }

    first->forceActiveFocus(Qt::TabFocusReason);
    const bool firstFocused = first->hasActiveFocus();
    host.focusRestoration()->remember(first);
    second->forceActiveFocus(Qt::TabFocusReason);
    const bool focusMoved = second->hasActiveFocus() && !first->hasActiveFocus();
    host.focusRestoration()->restore();
    const bool focusRestored = first->hasActiveFocus();

    fprintf(stdout,
            "product-quick-a11y-smoke focus_restore first=%d moved=%d restored=%d\n",
            firstFocused ? 1 : 0,
            focusMoved ? 1 : 0,
            focusRestored ? 1 : 0);
    return allTabReachable && firstFocused && focusMoved && focusRestored;
}

}   // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::ProductQuickAccessibilitySmoke);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    EditorHost host;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("editorHost"), &host);
    qmlRegisterUncreatableType<EditorHost>("Loop.Quick",
                                           1,
                                           0,
                                           "EditorHost",
                                           QStringLiteral("EditorHost is provided by the shell context"));

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
                                 const bool preflightAccessible = verifyPreflightAccessibility(window);
                                 const bool railAccessible = verifyNamedAccessibility(
                                     window, QStringLiteral("workspaceRail"), QAccessible::Grouping, false);
                                 const bool findingsAccessible = verifyNamedAccessibility(
                                     window, QStringLiteral("preflightFindingsView"), QAccessible::List, true);
                                 const bool runButtonAccessible = verifyNamedAccessibility(
                                     window, QStringLiteral("runPreflightButton"), QAccessible::PushButton, true);
                                 const bool keyboardSurface = verifyKeyboardSurface(window, host);

                                 // #195 acceptance 1 + 7: the shell starts on a freshly opened
                                 // document, so the preflight surface must present its not-checked
                                 // state - never a pass - before any run has been accepted.
                                 const bool preflightFresh =
                                     host.preflightStateName() == QStringLiteral("not-checked");
                                 if (!preflightFresh)
                                 {
                                     fprintf(stderr,
                                             "product-quick-a11y-smoke preflight_not_checked_missing state=%s\n",
                                             host.preflightStateName().toLocal8Bit().constData());
                                 }

                                 const QVariantMap visual = host.preflightStateVisual();
                                 const bool truthfulVisual = visual.value(QStringLiteral("kind")).toString().size() > 0 &&
                                                             visual.value(QStringLiteral("accessibleName")).toString().trimmed().size() > 0;
                                 if (!truthfulVisual)
                                 {
                                     fprintf(stderr, "product-quick-a11y-smoke preflight_visual_missing\n");
                                 }

                                 const bool softwareRequested = qEnvironmentVariableIsSet("QT_QUICK_BACKEND") &&
                                                                qEnvironmentVariable("QT_QUICK_BACKEND").compare(QStringLiteral("software"), Qt::CaseInsensitive) == 0;
                                 const bool backendHonoured = !softwareRequested || api == QSGRendererInterface::Software;
                                 if (!backendHonoured)
                                 {
                                     fprintf(stderr,
                                             "product-quick-a11y-smoke software_backend_not_honoured graphics_api=%s\n",
                                             graphicsApiName(api).toLocal8Bit().constData());
                                 }

                                 const bool passed = api != QSGRendererInterface::Unknown && backendHonoured &&
                                                     focusHelper && canvasAccessible && preflightAccessible &&
                                                     railAccessible && findingsAccessible && runButtonAccessible &&
                                                     keyboardSurface && preflightFresh && truthfulVisual;

                                 fprintf(stdout, "product-quick-a11y-smoke status=%s\n", passed ? "pass" : "fail");
                                 fflush(stdout);
                                 application.exit(passed ? 0 : 5);
                             },
                             Qt::DirectConnection);
                     });

    engine.loadFromModule(QStringLiteral("Loop.Quick"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty())
    {
        return 2;
    }

    QTimer::singleShot(10000, &application, [&application]()
                       { application.exit(4); });

    return application.exec();
}
