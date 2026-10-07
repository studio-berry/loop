#include "editorhost.h"

#include "pdfapplicationidentity.h"
#include "loopcanvasitem.h"
#include "inspectormodel.h"
#include "preflightcontroller.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pagesurfacecoordinator.h"

#include <QAccessible>
#include <QDir>
#include <QFile>
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

#include <cstdio>
#include <functional>
#include <memory>

namespace
{

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
                             const qreal zoomBefore = host.zoom();
                             host.selectFinding(findingId);
                             bool profileExposed = false;
                             bool limitsExposed = false;
                             for (int row = 0; row < inspector->rowCount(); ++row)
                             {
                                 const QModelIndex index = inspector->index(row);
                                 const QString id = inspector->data(index, pdfinteraction::InspectorModel::PropertyIdRole).toString();
                                 const QString value = inspector->data(index, pdfinteraction::InspectorModel::ValueRole).toString();
                                 profileExposed |= id == QStringLiteral("profile-digest") && value == report.effectiveProfileDigest;
                                 limitsExposed |= id == QStringLiteral("check-reason") && value == checkStatus.reason;
                             }
                             if (host.currentPage() != 1 || host.zoom() <= zoomBefore || inspector->selectionId() != findingId ||
                                 !profileExposed || !limitsExposed)
                             {
                                 fprintf(stderr, "finding-navigation-fixture region_or_context_failed\n");
                                 application.exit(6);
                                 return;
                             }
                             phase = 1;
                             return;
                         }
                         if (phase == 1)
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
                             phase = 2;
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
                         fprintf(stdout, "finding-navigation-fixture id=generated-two-page-region-and-identical-byte-replacement region=100,200,60,80 stale_rejected=%d\n", passed ? 1 : 0);
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

    return lifecycleNamed && summaryCarriesState && controlsReachable;
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

/// #27 acceptance failure case: a partial inspection must stay visible in the one-document
/// Quick operator shell and must never look like a clear document. Drives EditorHost through a
/// real generated one-page fixture, observes mid-run progress, then accepts Core's own
/// fail-closed `unsupported-scope` result (a partially inspected document: no blocking finding,
/// but the enabled check produced no evidence). Prints a result line naming the fixture so the
/// software-backend smoke run carries the incomplete fixture next to the representative one.
void runIncompleteInspectionFixture(QGuiApplication& application, EditorHost& host)
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

    host.openFileUrl(QUrl::fromLocalFile(fixture));
    auto* timer = new QTimer(&application);
    QObject::connect(timer, &QTimer::timeout, &application,
                     [&application, &host, timer, directory, phase = 0]() mutable
                     {
                         auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
                         if (!host.hasDocument() || !preflight)
                         {
                             return;
                         }
                         const QString jobId = QStringLiteral("partial-inspection-job");
                         if (phase == 0)
                         {
                             host.setWorkspace(EditorHost::Preflight);
                             host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);
                             const QString revision = preflight->documentRevision();
                             preflight->beginRun(preflight->documentKey(), revision,
                                                 QStringLiteral("partial-inspection-profile"), jobId);
                             const bool progressVisible =
                                 preflight->updateProgress(jobId, revision, 40) &&
                                 preflight->property("progress").toInt() == 40 &&
                                 host.preflightStateName() == QStringLiteral("running");

                             pdf::PreflightResult result;
                             result.errorCode = QStringLiteral("unsupported-scope");
                             result.errorMessage =
                                 QStringLiteral("The selected restriction excludes the inspected page");
                             result.coverageScope = { { QStringLiteral("pages"), QJsonArray{ 2 } } };
                             pdf::PreflightCheckStatus check;
                             check.id = QStringLiteral("bleed");
                             check.status = QStringLiteral("not_applicable");
                             check.reason = QStringLiteral("restriction_excluded_all_content");
                             result.checkStatuses = { check };
                             if (!progressVisible || !preflight->acceptResult(jobId, revision, result))
                             {
                                 fprintf(stderr, "incomplete-inspection-fixture progress_or_accept_failed\n");
                                 application.exit(6);
                                 return;
                             }
                             phase = 1;
                             return;
                         }

                         const QString stateName = host.preflightStateName();
                         const QVariantMap visual = host.preflightStateVisual();
                         const bool incomplete =
                             stateName == QStringLiteral("incomplete") &&
                             stateName != QStringLiteral("pass") &&
                             preflight->property("progress").toInt() == 100 &&
                             !host.preflightOperatorSummary().trimmed().isEmpty() &&
                             !preflight->limitationDescription().trimmed().isEmpty() &&
                             visual.value(QStringLiteral("kind")).toString() == QStringLiteral("Incomplete") &&
                             visual.value(QStringLiteral("accessibleName")).toString() == QStringLiteral("Incomplete") &&
                             visual.value(QStringLiteral("colorRole")).toString() != QStringLiteral("Success") &&
                             visual.value(QStringLiteral("icon")).toString() != QStringLiteral("Checkmark");
                         fprintf(stdout,
                                 "incomplete-inspection-fixture id=generated-one-page-unsupported-scope state=%s incomplete=%d\n",
                                 stateName.toLocal8Bit().constData(),
                                 incomplete ? 1 : 0);
                         fflush(stdout);
                         timer->stop();
                         application.exit(incomplete ? 0 : 6);
                     });
    timer->start(25);
}

bool verifyPreviewText(QQuickWindow* window, const QString& name, const QString& expected)
{
    auto* item = window->findChild<QQuickItem*>(name);
    auto* accessible = item ? QAccessible::queryAccessibleInterface(item) : nullptr;
    return item && item->isVisible() && item->width() > 0 && item->height() > 0 &&
           !expected.isEmpty() && item->property("text").toString() == expected &&
           accessible && accessible->text(QAccessible::Description) == expected;
}

void runPreviewFidelityFixtures(QGuiApplication& application, EditorHost& host,
                                QQuickWindow* window, const QString& fixtureDirectory)
{
    const QStringList fixtures{ QStringLiteral("overprint-cmyk-mode1-on.pdf"),
                                QStringLiteral("transparency-normal-cmyk.pdf") };
    host.setWorkspace(EditorHost::Document);
    host.openFileUrl(QUrl::fromLocalFile(QDir(fixtureDirectory).filePath(fixtures.first())));
    auto* timer = new QTimer(&application);
    QObject::connect(timer, &QTimer::timeout, &application,
                     [&application, &host, window, timer, fixtureDirectory, fixtures,
                      fixtureIndex = 0, phase = 0, ticks = 0, previousRevision = QString()]() mutable
                     {
                         auto* canvas = window->findChild<pdfquick::LoopCanvasItem*>();
                         auto* surfaces = canvas ? canvas->surfaces() : nullptr;
                         const QString revision = host.previewIdentity().value(QStringLiteral("documentRevision")).toString();
                         if (++ticks >= 400)
                         {
                             fprintf(stderr, "preview-fidelity-fixture timeout fixture=%s phase=%d document=%s fidelity=%s canvas=%d surfaces=%d admitted=%d failed=%d budget=%d\n",
                                     qPrintable(fixtures.at(fixtureIndex)), phase, qPrintable(host.documentState()), qPrintable(host.previewFidelityStateName()),
                                     canvas ? 1 : 0, surfaces ? 1 : 0, surfaces ? surfaces->counters().admitted : 0,
                                     surfaces ? surfaces->counters().failed : 0, surfaces ? surfaces->counters().budgetExhausted : 0);
                             if (canvas && canvas->viewport() && surfaces)
                             {
                                 const QRect viewport = canvas->viewport()->viewportRect();
                                 fprintf(stderr, "preview-fidelity-fixture geometry canvas=%g,%g viewport=%d,%d requests=%d in_flight=%d\n",
                                         canvas->width(), canvas->height(), viewport.width(), viewport.height(), surfaces->counters().requested, surfaces->counters().inFlight);
                                 const auto& counts = surfaces->counters();
                                 fprintf(stderr, "preview-fidelity-fixture rejected superseded=%d revision=%d demand=%d oversize=%d stale=%d cancelled=%d\n",
                                         counts.rejectedSuperseded, counts.rejectedStaleRevision, counts.rejectedDemand, counts.rejectedOversize, counts.stale, counts.cancelled);
                             }
                             timer->stop();
                             application.exit(6);
                             return;
                         }
                         if (!host.hasDocument() || !surfaces || revision == previousRevision)
                         {
                             return;
                         }
                         const QString expectedFastState = fixtureIndex == 0 ? QStringLiteral("approximate") : QStringLiteral("exact");
                         if (phase == 0)
                         {
                             if (host.previewFidelityStateName() == QStringLiteral("unavailable"))
                             {
                                 return;
                             }
                             const auto* tile = surfaces->snapshot().tileForPage(0);
                             if (host.previewFidelityStateName() != expectedFastState ||
                                 host.previewFidelityOriginName() != QStringLiteral("fast-canvas") ||
                                 !tile || !tile->pixels || tile->pixels->image.isNull() ||
                                 pdfinteraction::hasAuthoritativeOverprintMarker(tile->key.colorOutputIdentity) ||
                                 !verifyPreviewText(window, QStringLiteral("renderFidelityMessage"), host.previewFidelitySummary()))
                             {
                                 fprintf(stderr, "preview-fidelity-fixture fast_canvas_failed fixture=%s state=%s\n",
                                         qPrintable(fixtures.at(fixtureIndex)), qPrintable(host.previewFidelityStateName()));
                                 application.exit(6);
                                 return;
                             }
                             host.setWorkspace(EditorHost::ProductionPreview);
                             phase = 1;
                             ticks = 0;
                             return;
                         }
                         if (phase == 1)
                         {
                             const QString fidelityText = QStringLiteral("Fidelity %1, origin %2.")
                                                              .arg(host.previewFidelityStateName(), host.previewFidelityOriginName());
                             auto* button = window->findChild<QQuickItem*>(fixtureIndex == 0
                                                                               ? QStringLiteral("productionPreviewProveButton")
                                                                               : QStringLiteral("productionPreviewFidelityToggle"));
                             if (!verifyPreviewText(window, QStringLiteral("productionPreviewFidelity"), fidelityText) ||
                                 !verifyPreviewText(window, QStringLiteral("productionPreviewFidelityBadgeText"), host.previewFidelitySummary()) ||
                                 (fixtureIndex == 0 && !host.previewFidelitySummary().contains(QStringLiteral("cannot stand as proof"))) ||
                                 !button || !button->isVisible() || !button->isEnabled())
                             {
                                 fprintf(stderr, "preview-fidelity-fixture production_preview_failed\n");
                                 application.exit(6);
                                 return;
                             }
                             button->forceActiveFocus(Qt::TabFocusReason);
                             QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
                             QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
                             QCoreApplication::sendEvent(window, &press);
                             QCoreApplication::sendEvent(window, &release);
                             if (!host.pageFidelityIsAuthoritative() || host.previewFidelityStateName() != QStringLiteral("unavailable") ||
                                 host.previewFidelityOriginName() != QStringLiteral("none") ||
                                 !verifyPreviewText(window, QStringLiteral("productionPreviewFidelityBadgeText"), host.previewFidelitySummary()))
                             {
                                 fprintf(stderr, "preview-fidelity-fixture pending_render_claimed_evidence\n");
                                 application.exit(6);
                                 return;
                             }
                             phase = 2;
                             ticks = 0;
                             return;
                         }
                         if (phase == 2)
                         {
                             if (host.previewFidelityStateName() == QStringLiteral("unavailable"))
                             {
                                 return;
                             }
                             const auto* tile = surfaces->snapshot().tileForPage(0);
                             if (host.previewFidelityStateName() != QStringLiteral("authoritative") ||
                                 host.previewFidelityOriginName() != QStringLiteral("output-preview") ||
                                 !tile || !tile->exact || !tile->pixels || tile->pixels->image.isNull() ||
                                 !pdfinteraction::hasAuthoritativeOverprintMarker(tile->key.colorOutputIdentity) ||
                                 !verifyPreviewText(window, QStringLiteral("productionPreviewFidelityBadgeText"), host.previewFidelitySummary()))
                             {
                                 fprintf(stderr, "preview-fidelity-fixture authoritative_render_failed\n");
                                 application.exit(6);
                                 return;
                             }
                             host.setWorkspace(EditorHost::Document);
                             phase = 3;
                             ticks = 0;
                             return;
                         }
                         if (!verifyPreviewText(window, QStringLiteral("renderFidelityMessage"), host.previewFidelitySummary()))
                         {
                             fprintf(stderr, "preview-fidelity-fixture authoritative_canvas_failed\n");
                             application.exit(6);
                             return;
                         }
                         fprintf(stdout, "preview-fidelity-fixture id=%s fast=%s explicit_switch=1 pending_unavailable=1 authoritative_pixels=1 qml_presented=1\n",
                                 qPrintable(fixtures.at(fixtureIndex)), qPrintable(expectedFastState));
                         fflush(stdout);
                         if (++fixtureIndex == fixtures.size())
                         {
                             timer->stop();
                             application.exit(0);
                             return;
                         }
                         previousRevision = revision;
                         phase = 0;
                         ticks = 0;
                         host.openFileUrl(QUrl::fromLocalFile(QDir(fixtureDirectory).filePath(fixtures.at(fixtureIndex))));
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
    const int probeArgument = arguments.indexOf(QStringLiteral("--operator-native-probe"));
    const bool nativeProbe = probeArgument >= 0;
    const int previewArgument = arguments.indexOf(QStringLiteral("--preview-fixtures"));
    QString previewFixtures;
    if (previewArgument >= 0)
    {
        if (nativeProbe || previewArgument + 1 >= arguments.size())
        {
            return 6;
        }
        previewFixtures = QDir(arguments.at(previewArgument + 1)).absolutePath();
        for (const QString& fixture : { QStringLiteral("overprint-cmyk-mode1-on.pdf"), QStringLiteral("transparency-normal-cmyk.pdf") })
        {
            if (!QFile::exists(QDir(previewFixtures).filePath(fixture)))
            {
                fprintf(stderr, "preview-fidelity-fixture missing_fixture=%s\n", qPrintable(fixture));
                return 6;
            }
        }
        if (qEnvironmentVariable("QT_QUICK_BACKEND") != QStringLiteral("software"))
        {
            fprintf(stderr, "preview-fidelity-fixture requires_software_backend\n");
            return 6;
        }
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
                     [&application, &host, nativeProbe, probeDirectory, previewFixtures](QObject* object, const QUrl&)
                     {
                         auto* window = qobject_cast<QQuickWindow*>(object);
                         if (!window)
                         {
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
                             [window, &application, &host, previewFixtures]()
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
                                 if (passed && !previewFixtures.isEmpty())
                                 {
                                     runPreviewFidelityFixtures(application, host, window, previewFixtures);
                                     return;
                                 }
                                 if (passed && qEnvironmentVariable("QT_QUICK_BACKEND") == QStringLiteral("software"))
                                 {
                                     runFindingNavigationFixture(application, host, window,
                                                                 [&application, &host]()
                                                                 { runIncompleteInspectionFixture(application, host); });
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
