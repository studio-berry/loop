#include "editorhost.h"

#include "pdfapplicationidentity.h"
#include "loopcanvasitem.h"
#include "inspectormodel.h"
#include "preflightcontroller.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfsecurityhandler.h"
#include "preflightprofileresolver.h"

#include <QAccessible>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QSaveFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml/qqml.h>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QTemporaryDir>
#include <QStandardPaths>

#include <cstdio>
#include <functional>
#include <memory>

namespace
{

bool verifyPresentedText(QQuickWindow* window, const QString& objectName, const QString& expected)
{
    auto* item = window->findChild<QQuickItem*>(objectName);
    auto* accessible = item ? QAccessible::queryAccessibleInterface(item) : nullptr;
    return item && item->isVisible() && item->width() > 0 && item->height() > 0 &&
           !expected.trimmed().isEmpty() && item->property("text").toString() == expected &&
           accessible && accessible->text(QAccessible::Description) == expected;
}

void sendKey(QQuickWindow* window, QQuickItem* item, Qt::Key key)
{
    item->forceActiveFocus(Qt::TabFocusReason);
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QCoreApplication::sendEvent(window, &release);
}

void runEncryptedOpenFixture(QGuiApplication& application, EditorHost& host, QQuickWindow* window)
{
    auto directory = std::make_shared<QTemporaryDir>();
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    pdf::PDFSecurityHandlerFactory::SecuritySettings settings;
    settings.algorithm = pdf::PDFSecurityHandlerFactory::AES_256;
    settings.userPassword = QStringLiteral("fixture-user");
    settings.ownerPassword = QStringLiteral("fixture-owner");
    settings.id = builder.build().getIdPart(0);
    builder.setSecurityHandler(pdf::PDFSecurityHandlerFactory::createSecurityHandler(settings));
    const auto document = builder.build();
    const QString path = directory->filePath(QStringLiteral("quick-password-AES-256.pdf"));
    pdf::PDFDocumentWriter writer(nullptr);
    if (!directory->isValid() || !writer.write(path, &document, true))
    {
        application.exit(6);
        return;
    }
    QFile fixture(path);
    if (!fixture.open(QIODevice::ReadOnly))
    {
        application.exit(6);
        return;
    }
    fprintf(stdout, "encrypted-open-fixture AES-256 sha256=%s\n",
            QCryptographicHash::hash(fixture.readAll(), QCryptographicHash::Sha256).toHex().constData());
    auto* focusTarget = window->findChild<QQuickItem*>(QStringLiteral("openDocumentButton"));
    if (!focusTarget)
    {
        application.exit(6);
        return;
    }
    focusTarget->forceActiveFocus();
    host.openFileUrl(QUrl::fromLocalFile(path));
    auto* timer = new QTimer(&application);
    QObject::connect(timer, &QTimer::timeout, &application,
                     [&application, &host, window, directory, path, focusTarget, timer, phase = 0]() mutable
                     {
                         auto* field = window->findChild<QQuickItem*>(QStringLiteral("documentPasswordField"));
                         if (!field)
                         {
                             application.exit(6);
                             return;
                         }
                         const auto fail = [&application](const char* reason)
                         {
                             fprintf(stderr, "encrypted-open-fixture failed=%s\n", reason);
                             application.exit(6);
                         };
                         if (phase == 0 || phase == 2 || phase == 4)
                         {
                             if (!host.passwordRequestId() || !field->isVisible() || !field->hasActiveFocus())
                             {
                                 return;
                             }
                             auto* accessible = QAccessible::queryAccessibleInterface(field);
                             if (!accessible || accessible->role() != QAccessible::EditableText ||
                                 accessible->text(QAccessible::Name) != QStringLiteral("Document password") ||
                                 !accessible->state().passwordEdit)
                             {
                                 fail("password-accessibility");
                                 return;
                             }
                             if (phase == 4)
                             {
                                 field->setProperty("text", QStringLiteral("discarded-fixture-password"));
                                 sendKey(window, field, Qt::Key_Escape);
                             }
                             else
                             {
                                 const QString password = phase == 0 ? QStringLiteral("fixture-user") : QStringLiteral("wrong-fixture-password");
                                 field->setProperty("text", password);
                                 if (accessible->text(QAccessible::Value).contains(password))
                                 {
                                     fail("accessible-password-disclosure");
                                     return;
                                 }
                                 sendKey(window, field, Qt::Key_Return);
                             }
                             ++phase;
                             return;
                         }
                         if (phase == 1)
                         {
                             if (!host.hasDocument())
                             {
                                 return;
                             }
                             if (!field->property("text").toString().isEmpty() || host.passwordRequestId() ||
                                 host.sessionForTest()->facade().permissions().allowsCorrection() || host.planActionList())
                             {
                                 fail("restricted-user-open");
                                 return;
                             }
                         }
                         if (phase == 3)
                         {
                             if (host.documentState() != QLatin1String("error"))
                             {
                                 return;
                             }
                             if (host.typedError() != QLatin1String("document/password-incorrect") ||
                                 !field->property("text").toString().isEmpty())
                             {
                                 fail("wrong-password-terminal");
                                 return;
                             }
                         }
                         if (phase == 5)
                         {
                             if (host.documentState() != QLatin1String("empty") || host.passwordRequestId())
                             {
                                 return;
                             }
                             if (host.typedError() != QLatin1String("document/password-cancelled") ||
                                 !field->property("text").toString().isEmpty() || !focusTarget->hasActiveFocus())
                             {
                                 fail("cancel-or-focus-restoration");
                                 return;
                             }
                             timer->stop();
                             fprintf(stdout, "encrypted-open-fixture status=pass user-open=1 wrong-password=1 escape-cancel=1 protected-accessible-field=1 focus-restored=1\n");
                             fflush(stdout);
                             application.exit(0);
                             return;
                         }
                         focusTarget->forceActiveFocus();
                         host.openFileUrl(QUrl::fromLocalFile(path));
                         ++phase;
                     });
    timer->start(30);
}

void runFindingNavigationFixture(QGuiApplication& application,
                                 EditorHost& host,
                                 QQuickWindow* window,
                                 std::function<void()> onPassed)
{
    auto directory = std::make_shared<QTemporaryDir>();
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString original = directory->filePath(QStringLiteral("finding-region.pdf"));
    const QString replacement = directory->filePath(QStringLiteral("replacement.pdf"));
    if (!directory->isValid() || !writer.write(original, &document, true) || !writer.write(replacement, &document, true))
    {
        application.exit(6);
        return;
    }

    host.openFileUrl(QUrl::fromLocalFile(original));
    auto* timer = new QTimer(&application);
    QObject::connect(timer, &QTimer::timeout, &application,
                     [&application, &host, window, timer, directory, replacement, onPassed, phase = 0, originalRevision = QString(), findingId = QString()]() mutable
                     {
                         auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
                         auto* inspector = qobject_cast<pdfinteraction::InspectorModel*>(host.inspector());
                         if (!host.hasDocument() || !preflight || !inspector)
                         {
                             return;
                         }
                         if (phase == 0)
                         {
                             host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);
                             originalRevision = preflight->documentRevision();
                             preflight->beginRun(preflight->documentKey(), preflight->documentRevision(), QStringLiteral("source-profile"), QStringLiteral("navigation-fixture"));
                             pdf::PreflightFinding finding;
                             finding.checkId = QStringLiteral("bleed");
                             finding.scope = QStringLiteral("page");
                             finding.page = 2;
                             finding.severity = QStringLiteral("error");
                             finding.type = QStringLiteral("bleed");
                             finding.bbox = QRectF(100, 200, 60, 80);
                             finding.evidenceIds = { QStringLiteral("navigation-fixture-evidence") };
                             findingId = finding.stableId();
                             pdf::PreflightResult report;
                             report.errors = { finding };
                             report.profileName = QStringLiteral("Finding navigation fixture");
                             report.effectiveProfileDigest = QStringLiteral("effective-profile-fixture");
                             report.coverageScope = { { QStringLiteral("pages"), QStringLiteral("2") } };
                             pdf::PreflightCheckStatus checkStatus;
                             checkStatus.id = finding.checkId;
                             checkStatus.status = QStringLiteral("incomplete");
                             checkStatus.reason = QStringLiteral("Only the selected page region was inspected");
                             report.checkStatuses = { checkStatus };
                             if (!preflight->acceptResult(QStringLiteral("navigation-fixture"), preflight->documentRevision(), report))
                             {
                                 application.exit(6);
                                 return;
                             }
                             host.setWorkspace(EditorHost::Preflight);
                             phase = 1;
                             return;
                         }
                         if (phase == 1)
                         {
                             auto* findings = window->findChild<QQuickItem*>(QStringLiteral("preflightFindingsView"));
                             if (!findings || !findings->isVisible() || findings->property("count").toInt() != 1)
                             {
                                 fprintf(stderr, "finding-navigation-fixture findings_list_missing\n");
                                 application.exit(6);
                                 return;
                             }
                             const qreal zoomBefore = host.zoom();
                             sendKey(window, findings, Qt::Key_Down);
                             sendKey(window, findings, Qt::Key_Return);
                             bool profileExposed = false;
                             bool limitsExposed = false;
                             for (int row = 0; row < inspector->rowCount(); ++row)
                             {
                                 const QModelIndex index = inspector->index(row);
                                 const QString id = inspector->data(index, pdfinteraction::InspectorModel::PropertyIdRole).toString();
                                 const QString value = inspector->data(index, pdfinteraction::InspectorModel::ValueRole).toString();
                                 profileExposed |= id == QStringLiteral("profile-digest") && value == QStringLiteral("effective-profile-fixture");
                                 limitsExposed |= id == QStringLiteral("check-reason") && value == QStringLiteral("Only the selected page region was inspected");
                             }
                             if (host.currentPage() != 1 || host.zoom() <= zoomBefore || inspector->selectionId() != findingId ||
                                 !profileExposed || !limitsExposed)
                             {
                                 fprintf(stderr, "finding-navigation-fixture region_or_context_failed\n");
                                 application.exit(6);
                                 return;
                             }
                             phase = 2;
                             return;
                         }
                         if (phase == 2)
                         {
                             auto* canvas = window->findChild<pdfquick::LoopCanvasItem*>();
                             auto* inspectorView = window->findChild<QQuickItem*>(QStringLiteral("inspectorView"));
                             const auto* viewport = canvas ? canvas->viewport() : nullptr;
                             const QRectF region = viewport ? viewport->pagePointToViewportMatrix(1).mapRect(QRectF(100, 200, 60, 80)) : QRectF();
                             if (!viewport || canvas->currentPage() != 1 ||
                                 QLineF(region.center(), viewport->viewportRect().center()).length() >= 2.0 ||
                                 !inspectorView || inspectorView->property("count").toInt() != inspector->rowCount())
                             {
                                 fprintf(stderr, "finding-navigation-fixture quick_region_or_inspector_failed\n");
                                 application.exit(6);
                                 return;
                             }
                             phase = 3;
                             host.openFileUrl(QUrl::fromLocalFile(replacement));
                             return;
                         }
                         if (preflight->documentRevision() == originalRevision)
                         {
                             return;
                         }
                         const qreal zoomBefore = host.zoom();
                         const int pageBefore = host.currentPage();
                         host.selectFinding(findingId);
                         const bool passed = inspector->selectionKind() == pdfinteraction::InspectorModel::SelectionKind::EmptyCanvas &&
                                             host.zoom() == zoomBefore && host.currentPage() == pageBefore;
                         fprintf(stdout, "finding-navigation-fixture id=generated-two-page-region-and-identical-byte-replacement region=100,200,60,80 keyboard_selected=1 stale_rejected=%d\n", passed ? 1 : 0);
                         fflush(stdout);
                         timer->stop();
                         if (passed && onPassed)
                         {
                             onPassed();
                         }
                         else
                         {
                             application.exit(passed ? 0 : 6);
                         }
                     });
    timer->start(25);
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
                              bool requiresDescription);

/// Object name of the workspace content currently presented by the shell stack.
QString visibleWorkspacePaneName(QQuickWindow* window)
{
    if (!window)
    {
        return QString();
    }

    QQuickItem* stack = window->findChild<QQuickItem*>(QStringLiteral("workspaceStack"));
    if (!stack)
    {
        return QString();
    }

    const QList<QQuickItem*> children = stack->childItems();
    for (QQuickItem* child : children)
    {
        if (child && child->isVisible())
        {
            return child->objectName();
        }
    }
    return QString();
}

/// #586 acceptance: every workspace destination resolves to a real surface (never the
/// retired placeholder pane) carrying a screen-reader name and role.
bool verifyWorkspaceSurfaces(QQuickWindow* window, EditorHost& host)
{
    struct Entry
    {
        EditorHost::LoopWorkspace workspace;
        const char* expectedObjectName;
        QAccessible::Role expectedRole;
    };
    // Every destination must resolve to real content carrying a screen-reader name and
    // role. Compare is a real surface now: it renders the comparison facts ComparePane
    // projects from EditorHost, not placeholder content.
    static const Entry entries[] = {
        { EditorHost::Document, "documentPane", QAccessible::Pane },
        { EditorHost::Preflight, "preflightPane", QAccessible::Grouping },
        { EditorHost::ProductionPreview, "productionPreviewPane", QAccessible::Grouping },
        { EditorHost::Pages, "pagesProductionPane", QAccessible::Grouping },
        { EditorHost::Inspect, "inspectPane", QAccessible::Grouping },
        { EditorHost::Fix, "actionListPane", QAccessible::Grouping },
        { EditorHost::Compare, "comparePane", QAccessible::Grouping },
    };

    bool passed = true;
    for (const Entry& entry : entries)
    {
        host.setWorkspace(entry.workspace);
        QCoreApplication::processEvents();

        const QString visible = visibleWorkspacePaneName(window);
        const bool enabled = host.isWorkspaceEnabled(entry.workspace);
        const bool nameMatches = visible == QString::fromLatin1(entry.expectedObjectName);
        const bool reachable = enabled && nameMatches;
        const bool accessible = reachable && verifyNamedAccessibility(window, visible, entry.expectedRole, true);

        if (!reachable || !accessible)
        {
            fprintf(stderr,
                    "product-quick-a11y-smoke workspace_surface_failed workspace=%d enabled=%d visible=%s expected=%s accessible=%d\n",
                    static_cast<int>(entry.workspace),
                    enabled ? 1 : 0,
                    visible.toLocal8Bit().constData(),
                    entry.expectedObjectName,
                    accessible ? 1 : 0);
            passed = false;
        }
    }

    // Compare must be enterable and resolve to its own pane, never placeholder content.
    host.setWorkspace(EditorHost::Fix);
    QCoreApplication::processEvents();
    const bool beforeCompare = visibleWorkspacePaneName(window) == QStringLiteral("actionListPane");
    host.setWorkspace(EditorHost::Compare);
    QCoreApplication::processEvents();
    const bool compareEnabled = host.isWorkspaceEnabled(EditorHost::Compare);
    const bool compareReachable = visibleWorkspacePaneName(window) == QStringLiteral("comparePane");
    fprintf(stdout,
            "product-quick-a11y-smoke compare_workspace_enabled=%d reachable=%d before=%d\n",
            compareEnabled ? 1 : 0,
            compareReachable ? 1 : 0,
            beforeCompare ? 1 : 0);
    if (!beforeCompare || !compareEnabled || !compareReachable)
    {
        fprintf(stderr,
                "product-quick-a11y-smoke compare_workspace_unreachable enabled=%d visible=%s\n",
                compareEnabled ? 1 : 0,
                visibleWorkspacePaneName(window).toLocal8Bit().constData());
        passed = false;
    }

    host.setWorkspace(EditorHost::Fix);
    QCoreApplication::processEvents();
    return passed;
}

/// #586 acceptance: the governed-correction surface presents its lifecycle with words and
/// a shape, never colour alone, and its review controls are keyboard reachable.
bool verifyFixLifecyclePresentation(QQuickWindow* window, EditorHost& host)
{
    if (!window)
    {
        return false;
    }

    const QVariantMap visual = host.fixLifecycleVisual();
    const QString accessibleName = visual.value(QStringLiteral("accessibleName")).toString().trimmed();
    const QString icon = visual.value(QStringLiteral("icon")).toString().trimmed();
    const QString kind = visual.value(QStringLiteral("kind")).toString().trimmed();
    const bool lifecycleNamed = !accessibleName.isEmpty() && !icon.isEmpty() && !kind.isEmpty();
    if (!lifecycleNamed)
    {
        fprintf(stderr, "product-quick-a11y-smoke fix_lifecycle_visual_missing\n");
    }

    const bool lifecycleSummary = !host.fixLifecycleSummary().trimmed().isEmpty();
    const bool summaryCarriesState = host.hasDocument() ? lifecycleSummary : true;
    if (!summaryCarriesState)
    {
        fprintf(stderr, "product-quick-a11y-smoke fix_lifecycle_summary_missing\n");
    }

    bool controlsReachable = true;
    for (const char* objectName : { "fixApprovePlanButton", "fixRejectPlanButton", "fixExecutePlanButton",
                                    "fixReplanButton", "fixRollbackSummary" })
    {
        QQuickItem* item = window->findChild<QQuickItem*>(QString::fromLatin1(objectName));
        if (!item || item->objectName().isEmpty())
        {
            fprintf(stderr, "product-quick-a11y-smoke fix_control_missing control=%s\n", objectName);
            controlsReachable = false;
        }
    }

    const bool singleExecutionControl =
        window->findChildren<QQuickItem*>(QStringLiteral("fixExecutePlanButton")).size() == 1 &&
        !window->findChild<QQuickItem*>(QStringLiteral("confirmActionListButton"));
    if (!singleExecutionControl)
    {
        fprintf(stderr, "product-quick-a11y-smoke fix_execution_control_not_unique\n");
    }
    const bool executionAccessible = verifyNamedAccessibility(
        window, QStringLiteral("fixExecutePlanButton"), QAccessible::PushButton, false);

    return lifecycleNamed && summaryCarriesState && controlsReachable &&
           singleExecutionControl && executionAccessible;
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

/// Issue #103: Select and Hand are a single-select bound to the host's real
/// active tool, not two independent check boxes. This drives the host and reads
/// each button's `checked` state back from the live QML objects.
bool verifyToolSelection(QQuickWindow* window, EditorHost& host)
{
    if (!window)
    {
        return false;
    }

    QQuickItem* selectButton = window->findChild<QQuickItem*>(QStringLiteral("selectToolButton"));
    QQuickItem* handButton = window->findChild<QQuickItem*>(QStringLiteral("handToolButton"));
    if (!selectButton || !handButton)
    {
        fprintf(stderr, "product-quick-a11y-smoke tool_button_missing select=%d hand=%d\n",
                selectButton ? 1 : 0,
                handButton ? 1 : 0);
        return false;
    }

    const auto checked = [](QQuickItem* item)
    { return item->property("checked").toBool(); };

    const bool initialSelect = host.activeTool() == QStringLiteral("select") &&
                               checked(selectButton) && !checked(handButton);

    const bool handApplied = host.setActiveTool(QStringLiteral("hand")) &&
                             host.activeTool() == QStringLiteral("hand") &&
                             checked(handButton) && !checked(selectButton);
    if (!handApplied)
    {
        fprintf(stderr, "product-quick-a11y-smoke tool_selection_mismatch hand=%d select=%d active=%s\n",
                checked(handButton) ? 1 : 0,
                checked(selectButton) ? 1 : 0,
                host.activeTool().toLocal8Bit().constData());
    }

    const bool selectRestored = host.setActiveTool(QStringLiteral("select")) &&
                                host.activeTool() == QStringLiteral("select") &&
                                checked(selectButton) && !checked(handButton);

    // A name outside the vocabulary is refused and changes nothing.
    const bool unknownRefused = !host.setActiveTool(QStringLiteral("bogus")) &&
                                host.activeTool() == QStringLiteral("select") &&
                                checked(selectButton) && !checked(handButton);

    fprintf(stdout,
            "product-quick-a11y-smoke tool_selection initial=%d hand=%d select=%d unknown_refused=%d\n",
            initialSelect ? 1 : 0,
            handApplied ? 1 : 0,
            selectRestored ? 1 : 0,
            unknownRefused ? 1 : 0);

    return initialSelect && handApplied && selectRestored && unknownRefused;
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

void runIncompleteInspectionFixture(QGuiApplication& application, EditorHost& host, QQuickWindow* window)
{
    auto directory = std::make_shared<QTemporaryDir>();
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString fixture = directory->filePath(QStringLiteral("partial-inspection.pdf"));
    if (!directory->isValid() || !writer.write(fixture, &document, true))
    {
        application.exit(6);
        return;
    }

    const QJsonObject profile = pdf::exportPreflightProfile(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("loop-smoke-partial-inspection") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Partial inspection smoke") },
        { QStringLiteral("restrictions"), QJsonObject{ { QStringLiteral("pages"), QStringLiteral("2") } } },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("bleed") }, { QStringLiteral("amount_pt"), 9 } } } } });
    const QString profilePath = directory->filePath(QFileInfo(directory->path()).fileName() + QStringLiteral("-partial.json"));
    QSaveFile profileFile(profilePath);
    const QByteArray profileBytes = QJsonDocument(profile).toJson();
    if (!profileFile.open(QIODevice::WriteOnly) || profileFile.write(profileBytes) != profileBytes.size() ||
        !profileFile.commit() || !host.importPreflightProfileFileUrl(QUrl::fromLocalFile(profilePath)))
    {
        fprintf(stderr, "incomplete-inspection-fixture profile_import_failed\n");
        application.exit(6);
        return;
    }
    const QString importedProfile = host.selectedPreflightProfileId();
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &application, [importedProfile]()
                     { QFile::remove(importedProfile); });

    host.openFileUrl(QUrl::fromLocalFile(fixture));
    auto* timer = new QTimer(&application);
    QObject::connect(timer, &QTimer::timeout, &application,
                     [&application, &host, window, timer, directory, fixture, phase = 0]() mutable
                     {
                         auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
                         if (!host.hasDocument() || !preflight)
                         {
                             return;
                         }
                         if (phase == 0)
                         {
                             host.setWorkspace(EditorHost::Preflight);
                             host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);
                             phase = 1;
                             return;
                         }
                         if (phase == 1)
                         {
                             auto* runButton = window->findChild<QQuickItem*>(QStringLiteral("runPreflightButton"));
                             auto* progress = window->findChild<QQuickItem*>(QStringLiteral("preflightProgress"));
                             if (host.preflightStateName() != QStringLiteral("not-checked") || host.hasPreflightReport() ||
                                 !verifyPresentedText(window, QStringLiteral("preflightVerdict"), preflight->verdictDescription()) ||
                                 !verifyPresentedText(window, QStringLiteral("preflightLimitations"), preflight->limitationDescription()) ||
                                 !runButton || !runButton->isVisible() || !runButton->isEnabled() || !progress || !progress->isVisible())
                             {
                                 fprintf(stderr, "incomplete-inspection-fixture missing_inspection_not_presented\n");
                                 application.exit(6);
                                 return;
                             }
                             sendKey(window, runButton, Qt::Key_Space);
                             if (host.preflightStateName() != QStringLiteral("running") || !progress->isEnabled() ||
                                 progress->property("value").toInt() != preflight->progress() ||
                                 !verifyPresentedText(window, QStringLiteral("preflightJobStatus"), preflight->jobDescription()))
                             {
                                 fprintf(stderr, "incomplete-inspection-fixture running_progress_not_presented\n");
                                 application.exit(6);
                                 return;
                             }
                             phase = 2;
                             return;
                         }
                         if (host.preflightStateName() == QStringLiteral("running"))
                         {
                             return;
                         }

                         const QString stateName = host.preflightStateName();
                         const QVariantMap visual = host.preflightStateVisual();
                         const QJsonObject report = QJsonDocument::fromJson(preflight->serializedReport(fixture)).object();
                         const bool incomplete =
                             stateName == QStringLiteral("incomplete") &&
                             preflight->progress() == 100 &&
                             report.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("reason_code")).toString() == QStringLiteral("unsupported-scope") &&
                             !host.preflightOperatorSummary().trimmed().isEmpty() &&
                             !preflight->limitationDescription().trimmed().isEmpty() &&
                             visual.value(QStringLiteral("kind")).toString() == QStringLiteral("Incomplete") &&
                             visual.value(QStringLiteral("accessibleName")).toString() == QStringLiteral("Incomplete") &&
                             visual.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success") &&
                             visual.value(QStringLiteral("icon")).toString() != QStringLiteral("Checkmark");
                         auto* progress = window->findChild<QQuickItem*>(QStringLiteral("preflightProgress"));
                         auto* icon = window->findChild<QQuickItem*>(QStringLiteral("preflightStateLabel"));
                         const bool presented =
                             verifyPresentedText(window, QStringLiteral("preflightVerdict"), preflight->verdictDescription()) &&
                             verifyPresentedText(window, QStringLiteral("preflightLimitations"), preflight->limitationDescription()) &&
                             verifyPresentedText(window, QStringLiteral("preflightJobStatus"), preflight->jobDescription()) &&
                             progress && progress->isVisible() && progress->property("value").toInt() == 100 &&
                             icon && icon->isVisible() && !icon->property("text").toString().isEmpty() &&
                             icon->property("text").toString() != QStringLiteral("\u2713");
                         fprintf(stdout,
                                 "incomplete-inspection-fixture id=generated-one-page-restriction-pages-2 state=%s incomplete=%d qml_presented=%d missing_presented=1 keyboard_run=1\n",
                                 stateName.toLocal8Bit().constData(),
                                 incomplete ? 1 : 0, presented ? 1 : 0);
                         fflush(stdout);
                         timer->stop();
                         application.exit(incomplete && presented ? 0 : 6);
                     });
    timer->start(25);
}

}   // namespace

namespace
{

bool writeProbeSnapshot(const QString& directory, int stage, QQuickWindow* window,
                        EditorHost& host, pdfinteraction::PreflightController& controller)
{
    const QList<QPair<QString, QString>> descriptions = {
        { QStringLiteral("preflightVerdict"), controller.verdictDescription() },
        { QStringLiteral("preflightLimitations"), controller.limitationDescription() },
        { QStringLiteral("preflightSelectedFinding"), controller.selectedFindingDescription() },
        { QStringLiteral("preflightJobStatus"), controller.jobDescription() }
    };
    QJsonArray nodes;
    for (const auto& entry : descriptions)
    {
        auto* item = window->findChild<QQuickItem*>(entry.first);
        auto* accessible = item ? QAccessible::queryAccessibleInterface(item) : nullptr;
        if (!accessible || !item->activeFocusOnTab() || !accessible->state().focusable ||
            accessible->text(QAccessible::Description) != entry.second)
        {
            fprintf(stderr, "operator-probe inaccessible_status=%s\n", qPrintable(entry.first));
            return false;
        }
        nodes.append(QJsonObject{ { QStringLiteral("name"), accessible->text(QAccessible::Name) },
                                  { QStringLiteral("description"), entry.second },
                                  { QStringLiteral("focusable"), true } });
    }
    for (const QString& objectName : { QStringLiteral("runPreflightButton"), QStringLiteral("cancelPreflightButton"), QStringLiteral("exportPreflightReportButton") })
    {
        auto* item = window->findChild<QQuickItem*>(objectName);
        auto* accessible = item ? QAccessible::queryAccessibleInterface(item) : nullptr;
        if (!accessible || accessible->text(QAccessible::Description).isEmpty())
        {
            return false;
        }
        nodes.append(QJsonObject{ { QStringLiteral("name"), accessible->text(QAccessible::Name) },
                                  { QStringLiteral("description"), accessible->text(QAccessible::Description) },
                                  { QStringLiteral("enabled"), item->isEnabled() } });
    }
    QJsonObject snapshot{ { QStringLiteral("stage"), stage },
                          { QStringLiteral("window_handle"), QString::number(qulonglong(window->winId())) },
                          { QStringLiteral("nodes"), nodes },
                          { QStringLiteral("has_document"), host.hasDocument() },
                          { QStringLiteral("native_accessibility_active"), QAccessible::isActive() },
                          { QStringLiteral("graphics_api"), graphicsApiName(window->rendererInterface()->graphicsApi()) } };
    QSaveFile output(QDir(directory).filePath(QStringLiteral("stage-%1.json").arg(stage)));
    const QByteArray bytes = QJsonDocument(snapshot).toJson();
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}

void startOperatorProbe(QGuiApplication& application, QQuickWindow* window, EditorHost& host,
                        const QString& directory)
{
    auto* controller = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    auto* timer = new QTimer(&application);
    host.setWorkspace(EditorHost::Preflight);
    window->hide();
    window->show();
    QObject::connect(timer, &QTimer::timeout, &application,
                     [&application, window, &host, controller, directory, applied = -1, opening = false]() mutable
                     {
                         QFile input(QDir(directory).filePath(QStringLiteral("stage.command")));
                         if (!input.open(QIODevice::ReadOnly))
                         {
                             return;
                         }
                         bool valid = false;
                         const int stage = input.readAll().trimmed().toInt(&valid);
                         if (!valid || stage < 0 || stage > 6 || stage > applied + 1)
                         {
                             application.exit(6);
                             return;
                         }
                         if (stage <= applied)
                         {
                             return;
                         }
                         if (stage == 1 && !host.hasDocument())
                         {
                             if (!opening)
                             {
                                 opening = true;
                                 host.openInitialPath(QDir(directory).filePath(QStringLiteral("operator-fixture.pdf")));
                             }
                             return;
                         }
                         const QString key = controller->documentKey();
                         const QString revision = controller->documentRevision();
                         switch (stage)
                         {
                             case 0:
                                 controller->clear();
                                 break;
                             case 1:
                                 controller->beginRun(key, revision, QStringLiteral("operator-fixture-profile"), QStringLiteral("operator-fixture-job-1"));
                                 break;
                             case 2:
                             {
                                 pdf::PreflightResult result;
                                 result.errorCode = QStringLiteral("evidence-incomplete");
                                 result.errorMessage = QStringLiteral("Font evidence is unavailable");
                                 result.coverageScope = { { QStringLiteral("pages"), QJsonArray{ 1 } } };
                                 pdf::PreflightCheckStatus check;
                                 check.id = QStringLiteral("fonts");
                                 check.status = QStringLiteral("incomplete");
                                 check.reason = QStringLiteral("Font evidence is unavailable");
                                 result.checkStatuses = { check };
                                 pdf::PreflightFinding finding;
                                 finding.checkId = QStringLiteral("bleed");
                                 finding.scope = QStringLiteral("page");
                                 finding.page = 1;
                                 finding.severity = QStringLiteral("warning");
                                 finding.message = QStringLiteral("Bleed needs inspection");
                                 finding.bbox = QRectF(1, 2, 3, 4);
                                 result.warnings = { finding };
                                 if (!controller->acceptResult(controller->jobId(), revision, result))
                                 {
                                     application.exit(6);
                                     return;
                                 }
                                 host.selectFinding(finding.stableId());
                                 break;
                             }
                             case 3:
                                 controller->beginRun(key, revision, {}, QStringLiteral("operator-fixture-job-2"));
                                 controller->cancelRun(controller->jobId());
                                 break;
                             case 4:
                                 controller->beginRun(key, revision, {}, QStringLiteral("operator-fixture-job-3"));
                                 controller->failRun(controller->jobId(), revision, QStringLiteral("Worker evidence is unavailable"));
                                 break;
                             case 5:
                                 controller->markProfileStale();
                                 break;
                             case 6:
                                 fprintf(stdout, "operator-probe status=pass native_accessibility_active=%d\n", QAccessible::isActive() ? 1 : 0);
                                 application.exit(0);
                                 return;
                         }
                         applied = stage;
                         host.setWorkspace(EditorHost::Preflight);
                         QTimer::singleShot(100, &application, [&application, window, &host, controller, directory, stage]()
                                            {
                                                if (!writeProbeSnapshot(directory, stage, window, host, *controller))
                                                {
                                                    application.exit(6);
                                                } });
                     });
    timer->start(100);
}

}   // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::ProductQuickAccessibilitySmoke);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    const QStringList arguments = application.arguments();
    const bool encryptedFixture = arguments.contains(QStringLiteral("--encrypted-fixture"));
    const int probeArgument = arguments.indexOf(QStringLiteral("--operator-native-probe"));
    const bool nativeProbe = probeArgument >= 0;
    if (!nativeProbe)
    {
        QStandardPaths::setTestModeEnabled(true);
    }
    QString probeDirectory;
    if (nativeProbe)
    {
        if (probeArgument + 1 >= arguments.size() || !QDir(arguments.at(probeArgument + 1)).exists())
        {
            return 6;
        }
        probeDirectory = arguments.at(probeArgument + 1);
        pdf::PDFDocumentBuilder builder;
        builder.appendPage(QRectF(0, 0, 100, 100));
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        if (!writer.write(QDir(probeDirectory).filePath(QStringLiteral("operator-fixture.pdf")), &document, true))
        {
            return 6;
        }
    }

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
                     [&application, &host, nativeProbe, probeDirectory, encryptedFixture](QObject* object, const QUrl&)
                     {
                         auto* window = qobject_cast<QQuickWindow*>(object);
                         if (!window)
                         {
                             return;
                         }

                         if (encryptedFixture)
                         {
                             QTimer::singleShot(0, &application, [&application, &host, window]()
                                                { runEncryptedOpenFixture(application, host, window); });
                             return;
                         }

                         if (nativeProbe)
                         {
                             QTimer::singleShot(0, &application, [&application, window, &host, probeDirectory]()
                                                { startOperatorProbe(application, window, host, probeDirectory); });
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
                                 const bool workspaceSurfaces = verifyWorkspaceSurfaces(window, host);
                                 const bool fixLifecycle = verifyFixLifecyclePresentation(window, host);
                                 const bool toolSelection = verifyToolSelection(window, host);

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
                                                     keyboardSurface && workspaceSurfaces && fixLifecycle &&
                                                     preflightFresh && truthfulVisual && toolSelection;

                                 fprintf(stdout, "product-quick-a11y-smoke status=%s\n", passed ? "pass" : "fail");
                                 fflush(stdout);
                                 if (passed && qEnvironmentVariable("QT_QUICK_BACKEND") == QStringLiteral("software"))
                                 {
                                     runFindingNavigationFixture(application, host, window,
                                                                 [&application, &host, window]()
                                                                 { runIncompleteInspectionFixture(application, host, window); });
                                 }
                                 else
                                 {
                                     application.exit(passed ? 0 : 5);
                                 }
                             },
                             Qt::QueuedConnection);
                     });

    engine.loadFromModule(QStringLiteral("Loop.Quick"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty())
    {
        return 2;
    }

    QTimer::singleShot(nativeProbe ? 120000 : (qEnvironmentVariable("QT_QUICK_BACKEND") == QStringLiteral("software") ? 30000 : 10000), &application, [&application]()
                       { application.exit(4); });

    return application.exec();
}
