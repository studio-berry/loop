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

#include "editorhost.h"

#include "focusrestoration.h"
#include "hittestsource.h"
#include "interactioncontroller.h"
#include "interactionstate.h"
#include "interactiontarget.h"
#include "loopcanvasitem.h"
#include "loopstatevisual.h"
#include "looptokens.h"
#include "pagesurfacecoordinator.h"
#include "actionlistcontroller.h"
#include "actionlistrunsubmitter.h"
#include "repairparameterschema.h"
#include "preflightcontroller.h"
#include "preflightclirun.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"
#include "previewstatemodel.h"
#include "productionmodel.h"
#include "interactiontarget.h"

#include "pdfdocumentsession.h"
#include "pdfdocumentwriter.h"
#include "pdfoperationhistorystore.h"
#include "pdfpreflightaudit.h"
#include "pdfpreflightcertificate.h"
#include "pdfpreflightverdict.h"
#include "pdfsafefilewriter.h"

#include "pdfblockingthreadguard.h"
#include "pdfpage.h"
#include "pdftransparencyrenderer.h"

#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QAccessibilityHints>
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QMetaEnum>
#include <QScreen>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace
{

const QString QuitCommandId = QStringLiteral("actionQuit");

QString actionListBindingsHash(const QJsonObject& bindings)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(bindings).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}

QJsonValue actionListEditorValue(const QVariant& value, const QJsonObject& schema, bool* omit)
{
    if (omit)
    {
        *omit = false;
    }
    const QString type = schema.value(QStringLiteral("type")).toString();
    const QString text = value.toString().trimmed();
    if (type == QStringLiteral("boolean"))
    {
        if (value.metaType().id() == QMetaType::Bool)
        {
            return value.toBool();
        }
        if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
        {
            return true;
        }
        if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
        {
            return false;
        }
    }
    else if (type == QStringLiteral("integer"))
    {
        bool ok = false;
        const qlonglong integer = text.toLongLong(&ok);
        if (ok)
        {
            return integer;
        }
    }
    else if (type == QStringLiteral("number"))
    {
        bool ok = false;
        const double number = text.toDouble(&ok);
        if (ok)
        {
            return number;
        }
    }
    else if (type == QStringLiteral("string"))
    {
        return value.toString();
    }

    if (text.isEmpty() && omit)
    {
        *omit = true;
    }
    return QJsonValue::fromVariant(value);
}

int rotationToDegrees(pdf::PageRotation rotation)
{
    switch (rotation)
    {
        case pdf::PageRotation::None:
            return 0;
        case pdf::PageRotation::Rotate90:
            return 90;
        case pdf::PageRotation::Rotate180:
            return 180;
        case pdf::PageRotation::Rotate270:
            return 270;
    }

    return 0;
}

QString actionListStateToString(pdfinteraction::ActionListController::State state)
{
    switch (state)
    {
        case pdfinteraction::ActionListController::State::Idle:
            return QStringLiteral("idle");
        case pdfinteraction::ActionListController::State::Validating:
            return QStringLiteral("validating");
        case pdfinteraction::ActionListController::State::Planning:
            return QStringLiteral("planning");
        case pdfinteraction::ActionListController::State::Running:
            return QStringLiteral("running");
        case pdfinteraction::ActionListController::State::Planned:
            return QStringLiteral("planned");
        case pdfinteraction::ActionListController::State::Succeeded:
            return QStringLiteral("succeeded");
        case pdfinteraction::ActionListController::State::Failed:
            return QStringLiteral("failed");
        case pdfinteraction::ActionListController::State::Cancelled:
            return QStringLiteral("cancelled");
    }
    return QStringLiteral("idle");
}

QString preflightStateToString(pdfinteraction::PreflightController::State state)
{
    switch (state)
    {
        case pdfinteraction::PreflightController::State::NotChecked:
            return QStringLiteral("not-checked");
        case pdfinteraction::PreflightController::State::Running:
            return QStringLiteral("running");
        case pdfinteraction::PreflightController::State::Cancelled:
            return QStringLiteral("cancelled");
        case pdfinteraction::PreflightController::State::Pass:
            return QStringLiteral("pass");
        case pdfinteraction::PreflightController::State::Findings:
            return QStringLiteral("findings");
        case pdfinteraction::PreflightController::State::Stale:
            return QStringLiteral("stale");
        case pdfinteraction::PreflightController::State::Incomplete:
            return QStringLiteral("incomplete");
        case pdfinteraction::PreflightController::State::Error:
            return QStringLiteral("error");
    }
    return QStringLiteral("not-checked");
}

QString shellMenuGroupForAction(const QString& id, const QString& target)
{
    static const QStringList fileActions = {
        QStringLiteral("actionOpen"),
        QStringLiteral("actionClose"),
        QStringLiteral("actionSave"),
        QStringLiteral("actionSave_As"),
        QStringLiteral("actionQuit"),
        QStringLiteral("actionPrint"),
        QStringLiteral("actionSendByEmail"),
        QStringLiteral("actionRenderToImages"),
        QStringLiteral("actionClearRecentFileHistory"),
        QStringLiteral("actionAutomaticDocumentRefresh"),
    };
    if (fileActions.contains(id))
    {
        return QStringLiteral("File");
    }

    if (id.startsWith(QStringLiteral("actionCopy")) || id.startsWith(QStringLiteral("actionCut")) ||
        id.startsWith(QStringLiteral("actionPaste")) || id == QStringLiteral("actionUndo") ||
        id == QStringLiteral("actionRedo"))
    {
        return QStringLiteral("Edit");
    }

    if (id.startsWith(QStringLiteral("actionZoom")) || id.startsWith(QStringLiteral("actionFit")) ||
        id.startsWith(QStringLiteral("actionRotate")) || id.startsWith(QStringLiteral("actionPageLayout")) ||
        id.startsWith(QStringLiteral("actionGoTo")) || id.startsWith(QStringLiteral("actionFind")) ||
        id == QStringLiteral("actionFullscreenMode"))
    {
        return QStringLiteral("View");
    }

    if (id == QStringLiteral("actionAbout") || id == QStringLiteral("actionBecomeASponsor") ||
        id == QStringLiteral("actionGet_Source"))
    {
        return QStringLiteral("Help");
    }

    if (target == QStringLiteral("Preflight"))
    {
        return QStringLiteral("Preflight");
    }
    if (target == QStringLiteral("Production") || target == QStringLiteral("Pages") || target == QStringLiteral("Fix"))
    {
        return QStringLiteral("Production");
    }
    if (target == QStringLiteral("Document") || target == QStringLiteral("Inspect"))
    {
        return QStringLiteral("Document");
    }

    return QStringLiteral("Document");
}

QVariantMap descriptorToVariant(const pdfinteraction::CommandDescriptor& descriptor, bool enabled)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("id"), descriptor.id);
    entry.insert(QStringLiteral("labelKey"), descriptor.labelKey);
    entry.insert(QStringLiteral("implemented"), descriptor.isImplemented());
    entry.insert(QStringLiteral("enabled"), enabled);
    entry.insert(QStringLiteral("target"), descriptor.target);
    entry.insert(QStringLiteral("disposition"), descriptor.disposition);
    entry.insert(QStringLiteral("menuGroup"), shellMenuGroupForAction(descriptor.id, descriptor.target));

    QVariantMap shortcut;
    shortcut.insert(QStringLiteral("standardKey"), descriptor.shortcut.standardKey);
    shortcut.insert(QStringLiteral("sequence"), descriptor.shortcut.sequence);
    entry.insert(QStringLiteral("shortcut"), shortcut);
    entry.insert(QStringLiteral("shortcutText"),
                 descriptor.shortcut.sequence.isEmpty() ? descriptor.shortcut.standardKey : descriptor.shortcut.sequence);
    return entry;
}

}   // namespace

struct EditorHost::PreflightWorkerOutcome
{
    pdf::PreflightResult result;
};

EditorHost::EditorHost(QObject* parent) :
    QObject(parent),
    m_session(std::make_unique<DocumentViewSession>(this)),
    m_findingNavigator(std::make_unique<pdfinteraction::FindingCanvasNavigator>(
        *m_session->revisionSource(),
        m_session->viewport(),
        *m_session->interaction(),
        *m_session->overlays(),
        pdfinteraction::FindingTargetingCapabilityRegistry::defaultRegistry(),
        this)),
    m_preflight(&m_session->scheduler(), this),
    m_actionListController(&m_session->scheduler(), this)
{
    // Registers this constructing thread -- the one QML dispatches pointer
    // and frame callbacks on -- as the thread blocking service adapters
    // (PreflightEngine::run, and future OCR/AI/file-I/O adapters) must
    // refuse to run on (issue #144).
    pdf::PDFBlockingThreadGuard::registerInteractiveThread();

    m_preflightProfileWatcher = new QFileSystemWatcher(this);
    connect(m_preflightProfileWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&)
            { reloadPreflightProfiles(); });
    connect(m_preflightProfileWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&)
            { reloadPreflightProfiles(); });
    reloadPreflightProfiles();

    m_actionListRecipeWatcher = new QFileSystemWatcher(this);
    connect(m_actionListRecipeWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&)
            { reloadActionListRecipes(); });
    connect(m_actionListRecipeWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&)
            { reloadActionListRecipes(); });
    reloadActionListRecipes();

    connectFacade();
    connectViewport();
    connectCatalog();
    connectInteraction();
    connectSurfaces();
    registerShellHandlers();
    registerFeatureHandlers();

    m_preflightOverlayBridge.setFindingsModel(m_preflight.findingsModel());
    m_preflightOverlayBridge.setOverlayBuilder(m_session->overlays());
    m_preflightOverlayBridge.setInteractionController(m_session->interaction());

    connect(&m_preflight, &pdfinteraction::PreflightController::stateChanged, this,
            [this](pdfinteraction::PreflightController::State state)
            {
                if (state == pdfinteraction::PreflightController::State::Stale ||
                    state == pdfinteraction::PreflightController::State::NotChecked ||
                    state == pdfinteraction::PreflightController::State::Running ||
                    state == pdfinteraction::PreflightController::State::Cancelled ||
                    state == pdfinteraction::PreflightController::State::Error)
                {
                    if (m_findingNavigator)
                    {
                        m_findingNavigator->invalidate();
                    }
                    m_preflight.findingsModel()->setSelectedFinding({});
                    if (m_inspector.selectionKind() == pdfinteraction::InspectorModel::SelectionKind::Finding)
                    {
                        applyEmptyCanvasInspectorSelection();
                    }
                }
                refreshHitTestSources();
                bumpPresentation();
            });
    connect(&m_preflight, &pdfinteraction::PreflightController::progressChanged, this, &EditorHost::bumpPresentation);
    connect(&m_actionListController, &pdfinteraction::ActionListController::stateChanged, this, &EditorHost::bumpPresentation);
    connect(&m_actionListController, &pdfinteraction::ActionListController::progressChanged, this, &EditorHost::bumpPresentation);
    connect(&m_actionListController, &pdfinteraction::ActionListController::resultChanged, this, &EditorHost::bumpPresentation);
    connect(m_preflight.findingsModel(), &pdfinteraction::PreflightFindingsModel::findingsReplaced, this, &EditorHost::refreshHitTestSources);
    connect(&m_preflight, &pdfinteraction::PreflightController::navigationRequested, this, &EditorHost::onPreflightNavigation);
    connect(m_findingNavigator.get(), &pdfinteraction::FindingCanvasNavigator::inspectionModeRequested,
            this, [this](const pdfinteraction::FindingInspectionModeRequest& request)
            { setInspectionMode(QString::fromLatin1(pdfinteraction::getFindingInspectionModeName(request.mode))); });
    connect(m_findingNavigator.get(), &pdfinteraction::FindingCanvasNavigator::inspectionModeReset,
            this, [this]
            { setInspectionMode(QStringLiteral("page")); });
    connect(m_findingNavigator.get(), &pdfinteraction::FindingCanvasNavigator::navigationApplied,
            this, [this](const pdfinteraction::FindingNavigationResult& result)
            {
                if (result.inspectionMode == pdfinteraction::FindingInspectionMode::None)
                {
                    setInspectionMode(QStringLiteral("page"));
                } });
    connect(&m_inspector, &pdfinteraction::InspectorModel::selectionChanged, this, &EditorHost::bumpPresentation);
    connect(&m_preview, &pdfinteraction::PreviewStateModel::stateChanged, this, &EditorHost::bumpPresentation);
    connect(&m_production, &pdfinteraction::ProductionModel::stateChanged, this, &EditorHost::bumpPresentation);
    connect(&m_documentModel, &QuickDocumentModel::searchChanged, this, [this]
            {
                refreshFeatureAvailability();
                bumpPresentation();
                bumpCommandEpoch(); });

    connect(&m_session->scheduler(), &pdf::PDFJobScheduler::jobQueued, this, [this](const pdf::PDFJobSnapshot& snapshot)
            {
                m_activeAsyncJobs.insert(snapshot.jobId, snapshot.kind);
                refreshCanvasTrace(); });
    connect(&m_session->scheduler(), &pdf::PDFJobScheduler::jobProgress, this, [this](const pdf::PDFJobSnapshot& snapshot)
            {
                m_preflight.updateProgress(snapshot.jobId, snapshot.documentRevision, snapshot.progress);
                m_actionListController.updateProgress(snapshot.jobId, snapshot.documentRevision, snapshot.progress); });
    connect(&m_session->scheduler(), &pdf::PDFJobScheduler::jobFinished, this, [this](const pdf::PDFJobSnapshot& snapshot)
            {
                m_activeAsyncJobs.remove(snapshot.jobId);
                finishPreflightJob(snapshot);
                finishActionListJob(snapshot);
                refreshCanvasTrace(); });
}

EditorHost::~EditorHost()
{
    m_acceptPreflightResults = false;
    m_acceptActionListResults = false;
    cancelPreflight();
    cancelActionList();
    QObject::disconnect(&m_session->scheduler(), nullptr, this, nullptr);
    unbindCanvas();

    // The guard registration is global process state owned by the thread that
    // built this host, so pair it with the host's lifetime: a host destroyed and
    // recreated in one process must not leave a registration behind that keeps
    // refusing synchronous blocking work on that thread.
    if (pdf::PDFBlockingThreadGuard::isCurrentThreadInteractive())
    {
        pdf::PDFBlockingThreadGuard::clearInteractiveThread();
    }
}

QString EditorHost::documentState() const
{
    return QString::fromLatin1(pdfinteraction::getDocumentStateName(m_session->facade().state()));
}

bool EditorHost::hasDocument() const
{
    return m_session->facade().state() == pdfinteraction::DocumentState::Ready;
}

QString EditorHost::displayTitle() const
{
    return m_session->facade().source().displayLabel();
}

QString EditorHost::typedError() const
{
    return m_session->facade().typedError();
}

int EditorHost::pageCount() const
{
    return m_session->viewport().pageCount();
}

int EditorHost::currentPage() const
{
    return m_session->viewport().currentPage();
}

qreal EditorHost::zoom() const
{
    return m_session->viewport().zoom();
}

int EditorHost::rotationDegrees() const
{
    return rotationToDegrees(m_session->viewport().rotation());
}

bool EditorHost::incomplete() const
{
    return m_session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Incomplete);
}

bool EditorHost::cancelled() const
{
    return m_session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Cancelled);
}

bool EditorHost::unsupported() const
{
    return m_session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Unsupported);
}

QObject* EditorHost::preflight()
{
    return &m_preflight;
}

QObject* EditorHost::actionList()
{
    return &m_actionListController;
}

QObject* EditorHost::inspector()
{
    return &m_inspector;
}

QObject* EditorHost::preview()
{
    return &m_preview;
}

void EditorHost::goToPage(int pageIndex)
{
    if (!hasDocument())
    {
        return;
    }

    m_session->commandBridge().goToPage(pageIndex);
    bumpPresentation();
}

void EditorHost::goToOutlinePage(int pageIndex)
{
    // Outline navigation reuses the implemented viewport path; separate entry
    // keeps QML from depending on an unimplemented goToOutlineIndex.
    goToPage(pageIndex);
}

void EditorHost::setWorkspace(LoopWorkspace workspace)
{
    if (!isWorkspaceEnabled(workspace) || workspace == m_workspace)
    {
        return;
    }

    const LoopWorkspace previous = m_workspace;
    m_workspace = workspace;
    m_workspaceRequest = -1;
    Q_EMIT workspaceChanged(previous, workspace);
    bumpPresentation();
}

void EditorHost::acknowledgeWorkspaceRequest()
{
    if (m_workspaceRequest < 0)
    {
        return;
    }

    const LoopWorkspace requested = static_cast<LoopWorkspace>(m_workspaceRequest);
    m_workspaceRequest = -1;
    setWorkspace(requested);
}

QString EditorHost::documentShellStatus() const
{
    return QString::fromLatin1(
        pdfinteraction::getShellDocumentStatusName(m_session->facade().shellDocumentStatus()));
}

QString EditorHost::productionStateName() const
{
    if (!hasDocument())
    {
        return QStringLiteral("NOT_READY");
    }

    switch (m_session->facade().outputState())
    {
        case pdfinteraction::DocumentOutputState::Pending:
            return QStringLiteral("OPERATION_PENDING");
        case pdfinteraction::DocumentOutputState::Saved:
            return QStringLiteral("OUTPUT_WRITTEN");
        case pdfinteraction::DocumentOutputState::None:
            break;
    }

    return pdfinteraction::ProductionModel::stateName(m_production.state());
}

bool EditorHost::allowDeveloperDiagnostics() const
{
#ifdef LOOP_LOOP_DISTRIBUTION_BUILD
    return false;
#else
    return true;
#endif
}

void EditorHost::acknowledgeSearchPanel()
{
    if (!m_searchPanelVisible)
    {
        return;
    }

    m_searchPanelVisible = false;
    Q_EMIT presentationChanged();
}

QString EditorHost::preflightStateName() const
{
    return preflightStateToString(m_preflight.state());
}

QVariantMap EditorHost::preflightStateVisual() const
{
    const pdfquick::tokens::LoopStateVisual visual = pdfquick::tokens::resolvePreflightStateVisual(preflightStateName());

    QVariantMap result;
    result.insert(QStringLiteral("kind"), pdfquick::tokens::stateKindName(visual.kind));
    result.insert(QStringLiteral("colorRole"), pdfquick::tokens::colorRoleName(visual.colorRole));
    result.insert(QStringLiteral("icon"), pdfquick::tokens::stateIconName(visual.icon));
    result.insert(QStringLiteral("accessibleName"), visual.accessibleName);
    return result;
}

QColor EditorHost::preflightStateColor() const
{
    const pdfquick::tokens::LoopStateVisual visual = pdfquick::tokens::resolvePreflightStateVisual(preflightStateName());
    const pdfquick::tokens::LoopTheme theme =
        highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    return pdfquick::tokens::color(visual.colorRole, theme);
}

QString EditorHost::preflightOperatorSummary() const
{
    return m_preflight.operatorSummary();
}

QVariantMap EditorHost::preflightCertificateStateVisual() const
{
    const pdfquick::tokens::LoopStateVisual visual =
        pdfquick::tokens::resolvePreflightStateVisual(m_preflightCertificateStateName);

    QVariantMap result;
    result.insert(QStringLiteral("kind"), pdfquick::tokens::stateKindName(visual.kind));
    result.insert(QStringLiteral("colorRole"), pdfquick::tokens::colorRoleName(visual.colorRole));
    result.insert(QStringLiteral("icon"), pdfquick::tokens::stateIconName(visual.icon));
    if (m_preflightCertificateStateName == QLatin1String("certified"))
        result.insert(QStringLiteral("accessibleName"), tr("Certified preflight"));
    else if (m_preflightCertificateStateName == QLatin1String("certificate-invalid"))
        result.insert(QStringLiteral("accessibleName"), tr("Certified preflight invalid"));
    else
        result.insert(QStringLiteral("accessibleName"), tr("Not certified"));
    return result;
}

QColor EditorHost::preflightCertificateStateColor() const
{
    const pdfquick::tokens::LoopStateVisual visual =
        pdfquick::tokens::resolvePreflightStateVisual(m_preflightCertificateStateName);
    const pdfquick::tokens::LoopTheme theme =
        highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    return pdfquick::tokens::color(visual.colorRole, theme);
}

QVariantList EditorHost::preflightProfiles() const
{
    QVariantList profiles;
    profiles.reserve(m_preflightProfiles.size());
    for (const PreflightProfileChoice& profile : m_preflightProfiles)
    {
        QVariantMap item;
        item.insert(QStringLiteral("id"), profile.id);
        item.insert(QStringLiteral("name"), profile.name);
        item.insert(QStringLiteral("version"), profile.version);
        item.insert(QStringLiteral("source"), profile.source.startsWith(QLatin1Char(':'))
                                                  ? tr("Bundled")
                                                  : tr("Local"));
        item.insert(QStringLiteral("digest"), profile.digest);
        item.insert(QStringLiteral("valid"), profile.valid);
        item.insert(QStringLiteral("diagnostic"), profile.diagnostic);
        profiles.append(item);
    }
    return profiles;
}

QVariantList EditorHost::preflightVariables() const
{
    const auto it = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                 [this](const PreflightProfileChoice& profile)
                                 { return profile.id == m_selectedPreflightProfileId; });
    if (it == m_preflightProfiles.cend())
    {
        return {};
    }

    QVariantList variables;
    const QStringList names = it->variables.keys();
    for (const QString& name : names)
    {
        const QJsonObject declaration = it->variables.value(name).toObject();
        QVariantMap item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("type"), declaration.value(QStringLiteral("type")).toString());
        item.insert(QStringLiteral("required"), declaration.value(QStringLiteral("required")).toBool());
        item.insert(QStringLiteral("description"), declaration.value(QStringLiteral("description")).toString());
        item.insert(QStringLiteral("value"), m_preflightBindings.contains(name)
                                                 ? m_preflightBindings.value(name).toVariant()
                                                 : declaration.value(QStringLiteral("default")).toVariant());
        if (declaration.contains(QStringLiteral("min")))
            item.insert(QStringLiteral("min"), declaration.value(QStringLiteral("min")).toVariant());
        if (declaration.contains(QStringLiteral("max")))
            item.insert(QStringLiteral("max"), declaration.value(QStringLiteral("max")).toVariant());
        variables.append(item);
    }
    return variables;
}

QString EditorHost::selectedPreflightProfileId() const
{
    return m_selectedPreflightProfileId;
}

bool EditorHost::preflightProfileEditing() const
{
    return m_preflightProfileDraft.isActive();
}

QVariantList EditorHost::preflightEditableChecks() const
{
    return m_preflightProfileDraft.editableChecks();
}

QString EditorHost::preflightProfileDraftVersion() const
{
    return m_preflightProfileDraft.suggestedNextVersion();
}

QString EditorHost::previewSummary() const
{
    return m_preview.summary();
}

QString EditorHost::inspectorTitle() const
{
    return m_inspector.title();
}

bool EditorHost::preferReducedMotion() const
{
    const QByteArray env = qgetenv("QT_ACCESSIBILITY_REDUCE_MOTION");
    if (!env.isEmpty())
    {
        return env == "1" || env.toLower() == "true";
    }

    return false;
}

bool EditorHost::highContrast() const
{
    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
    {
        if (QStyleHints* hints = app->styleHints())
        {
            if (const QAccessibilityHints* accessibility = hints->accessibility())
            {
                return accessibility->contrastPreference() != Qt::ContrastPreference::NoPreference;
            }
        }
    }
    return false;
}

bool EditorHost::pageFidelityIsExact() const
{
    if (!hasDocument())
    {
        return true;
    }

    const std::optional<pdf::PDFRenderDiagnostics> diagnostics = m_session->surfaces()->diagnosticsForPage(currentPage());
    return !diagnostics.has_value() || diagnostics->isExact();
}

QString EditorHost::pageFidelityReason() const
{
    if (!hasDocument())
    {
        return QString();
    }

    const std::optional<pdf::PDFRenderDiagnostics> diagnostics = m_session->surfaces()->diagnosticsForPage(currentPage());
    if (!diagnostics.has_value() || diagnostics->reasons.isEmpty())
    {
        return QString();
    }

    return diagnostics->reasons.join(QStringLiteral(" "));
}

bool EditorHost::pageFidelityIsAuthoritative() const
{
    if (!hasDocument())
    {
        return false;
    }

    return m_session->surfaces()->isPageAuthoritativeOverprint(currentPage());
}

void EditorHost::toggleCurrentPageFidelity()
{
    if (!hasDocument())
    {
        return;
    }

    const int pageIndex = currentPage();
    const bool wasAuthoritative = m_session->surfaces()->isPageAuthoritativeOverprint(pageIndex);
    m_session->surfaces()->setPageAuthoritativeOverprint(pageIndex, !wasAuthoritative);
    bumpPresentation();
}

void EditorHost::selectFinding(const QString& findingId)
{
    if (!m_session->revisionSource() || findingId.isEmpty())
    {
        return;
    }

    pdfinteraction::PreflightController::EvidenceNavigationRequest request;
    const QString documentRevision = m_session->facade().currentRevision().toString();
    if (!m_preflight.navigationFor(findingId, &request) ||
        request.documentKey != m_session->revisionSource()->documentKey() ||
        request.documentRevision != documentRevision)
    {
        if (m_findingNavigator)
        {
            m_findingNavigator->invalidate();
        }
        m_preflight.findingsModel()->setSelectedFinding({});
        applyEmptyCanvasInspectorSelection();
        bumpPresentation();
        return;
    }

    if (!m_inspector.setFindingSelection(*m_preflight.findingsModel(), findingId, documentRevision))
    {
        return;
    }
    m_preflight.findingsModel()->setSelectedFinding(findingId);
    onPreflightNavigation(request);

    // Navigation deliberately clears the canvas interaction target for a check that
    // cannot paint evidence, and that clear reaches the Inspector. Re-assert the
    // finding selection this request proved so the operator keeps the selected finding
    // together with its honest targeting explanation.
    if (m_inspector.selectionKind() != pdfinteraction::InspectorModel::SelectionKind::Finding)
    {
        m_inspector.setFindingSelection(*m_preflight.findingsModel(), findingId, documentRevision);
    }
}

bool EditorHost::selectNextFinding()
{
    return moveFindingSelection(1);
}

bool EditorHost::selectPreviousFinding()
{
    return moveFindingSelection(-1);
}

bool EditorHost::moveFindingSelection(int direction)
{
    if (!hasDocument() || direction == 0)
    {
        return false;
    }

    pdfinteraction::PreflightFindingsModel* findings = m_preflight.findingsModel();
    const QString findingId = findings->adjacentFindingId(findings->selectedFindingId(), direction);
    if (findingId.isEmpty())
    {
        return false;
    }

    selectFinding(findingId);
    return true;
}

void EditorHost::announceDocumentState(const QString& message)
{
    if (message.trimmed().isEmpty())
    {
        return;
    }

    QAccessibleAnnouncementEvent event(this, message);
    QAccessible::updateAccessibility(&event);
}

bool EditorHost::runPreflight()
{
    if (!hasDocument() || m_preflight.state() == pdfinteraction::PreflightController::State::Running ||
        !m_session->revisionSource())
    {
        return false;
    }

    const auto profileIt = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                        [this](const PreflightProfileChoice& profile)
                                        { return profile.id == m_selectedPreflightProfileId; });
    if (profileIt == m_preflightProfiles.cend() || !profileIt->valid)
    {
        return false;
    }

    const pdf::PDFDocumentPointer document = m_session->context().getDocumentPointer();
    if (!document)
    {
        return false;
    }

    const QString documentKey = m_session->revisionSource()->documentKey();
    const QString documentRevision = m_session->facade().currentRevision().toString();
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    pdf::PDFJobSpec spec;
    spec.jobId = jobId;
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = documentKey;
    spec.documentRevision = documentRevision;
    spec.operationId = QStringLiteral("preflight.%1").arg(profileIt->id);
    spec.checkId = profileIt->name;
    spec.progressModel = QStringLiteral("preflight-progress-v1");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    m_preflight.beginRun(documentKey,
                         documentRevision,
                         profileIt->digest,
                         jobId);
    auto outcome = std::make_shared<PreflightWorkerOutcome>();
    m_preflightOutcomes.insert(jobId, outcome);
    const PreflightProfileChoice selectedProfile = *profileIt;
    const QJsonObject bindings = m_preflightBindings;
    const QByteArray sourceHash = m_session->context().getDocumentIdentity().sourceDataHash;
    const QString documentPath = m_session->facade().source().path;
    const bool documentDirty = m_session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Dirty);

    const QString submittedId = m_session->scheduler().submit(
        spec,
        [document, outcome, selectedProfile, bindings, sourceHash, documentPath, documentDirty](pdf::PDFJobContext& context)
        {
            if (context.isCancellationRequested())
            {
                return;
            }

            const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(selectedProfile.profile,
                                                                                           selectedProfile.source);
            if (!imported.ok)
            {
                throw std::runtime_error(imported.errorMessage.toStdString());
            }
            const pdf::PreflightVariableBindResult bound =
                pdf::bindPreflightProfileVariables(imported.profile, bindings);
            if (!bound.ok)
            {
                throw std::runtime_error(bound.errorMessage.toStdString());
            }
            pdf::PreflightProfileResolver resolver;
            const pdf::PreflightResolvedProfile resolved = resolver.resolveExplicitProfile(
                bound.profile, selectedProfile.name,
                imported.identity.version.isEmpty() ? QStringLiteral("explicit") : imported.identity.version);
            if (!resolved.ok)
            {
                throw std::runtime_error(resolved.errorMessage.toStdString());
            }

            pdf::PreflightProfileData profile;
            QString profileError;
            if (!pdf::PreflightEngine::parseProfile(bound.profile, profile, profileError))
            {
                throw std::runtime_error(profileError.toStdString());
            }
            profile.variableBindings = bound.bindings;
            profile.fileDigest = imported.identity.digest;
            profile.effectiveDigest = pdf::computeProfileDigest(bound.profile);
            profile.profileIdentity = imported.identity.toJson();
            profile.profileIdentity.insert(QStringLiteral("effective_digest"), profile.effectiveDigest);
            context.reportProgress(5);

            std::unique_ptr<pdf::PDFDocumentSession, void (*)(pdf::PDFDocumentSession*)> session(
                pdf::PDFDocumentSession::createForInspection(document.data()), &pdf::PDFDocumentSession::destroy);
            QByteArray auditBytes;
            if (!documentDirty)
            {
                QFile sourceFile(documentPath);
                if (sourceFile.open(QIODevice::ReadOnly))
                {
                    const QByteArray diskBytes = sourceFile.readAll();
                    const QByteArray diskHash = QCryptographicHash::hash(diskBytes, QCryptographicHash::Sha256);
                    if (diskHash == sourceHash)
                        auditBytes = diskBytes;
                }
            }
            if (auditBytes.isEmpty())
            {
                QBuffer serialized(&auditBytes);
                if (!serialized.open(QIODevice::WriteOnly))
                    throw std::runtime_error("Could not prepare the current document revision for preflight audit.");
                pdf::PDFDocumentWriter writer(nullptr, context.operationControl());
                if (const pdf::PDFOperationResult writeResult = writer.write(&serialized, document.data()); !writeResult)
                    throw std::runtime_error(writeResult.getErrorMessage().toStdString());
            }

            const QByteArray revisionHash = QCryptographicHash::hash(auditBytes, QCryptographicHash::Sha256);
            pdf::PreflightEngine engine(session.get());
            engine.setOperationControl(context.operationControl());
            context.reportProgress(15);
            outcome->result = engine.run(profile);
            pdf::finalizePreflightResult(outcome->result, revisionHash, resolved);

            pdf::PDFOperationHistoryStatus auditStatus = pdf::PDFOperationHistoryStatus::Accepted;
            if (context.isCancellationRequested())
                auditStatus = pdf::PDFOperationHistoryStatus::Cancelled;
            else if (pdf::reducePreflightVerdict(outcome->result).state == pdf::PreflightVerdictState::Error)
                auditStatus = pdf::PDFOperationHistoryStatus::Failed;

            const QJsonObject auditSummary =
                pdf::preflightAuditReportSummary(outcome->result, documentPath);
            if (const pdf::PDFOperationResult auditResult =
                    pdf::appendPreflightAuditRun(documentPath,
                                                 auditBytes,
                                                 outcome->result,
                                                 auditStatus,
                                                 QStringLiteral("LoopEditor"),
                                                 auditSummary);
                !auditResult)
            {
                throw std::runtime_error(auditResult.getErrorMessage().toStdString());
            }

            if (context.isCancellationRequested())
                return;

            context.reportProgress(95);
            context.setResultSummary(QStringLiteral("Preflight completed."));
        });
    if (submittedId != jobId)
    {
        m_preflightOutcomes.remove(jobId);
        m_preflight.failRun(jobId, documentRevision, tr("Unable to submit preflight work."));
        return false;
    }

    bumpPresentation();
    return true;
}

bool EditorHost::cancelPreflight()
{
    return m_preflight.cancelRun(m_preflight.jobId());
}

bool EditorHost::selectPreflightProfile(const QString& id)
{
    const auto it = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                 [&id](const PreflightProfileChoice& profile)
                                 { return profile.id == id; });
    if (it == m_preflightProfiles.cend() || !it->valid || id == m_selectedPreflightProfileId)
    {
        return false;
    }
    m_selectedPreflightProfileId = id;
    m_preflightBindings = QJsonObject();
    m_preflight.markProfileStale();
    m_actionListController.markRecipeStale();
    Q_EMIT preflightProfilesChanged();
    bumpPresentation();
    return true;
}

bool EditorHost::setPreflightVariable(const QString& name, const QVariant& value)
{
    const auto it = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                 [this](const PreflightProfileChoice& profile)
                                 { return profile.id == m_selectedPreflightProfileId; });
    if (it == m_preflightProfiles.cend() || !it->variables.contains(name))
    {
        return false;
    }
    m_preflightBindings.insert(name, QJsonValue::fromVariant(value));
    m_preflight.markProfileStale();
    m_actionListController.markRecipeStale();
    Q_EMIT preflightProfilesChanged();
    bumpPresentation();
    return true;
}

void EditorHost::requestPreflightReportExport()
{
    if (m_preflight.hasResult())
    {
        Q_EMIT preflightReportExportRequested();
    }
}

bool EditorHost::exportPreflightReportFileUrl(const QUrl& url)
{
    if (!url.isValid() || !url.isLocalFile() || !m_preflight.hasResult())
    {
        return false;
    }
    const QByteArray report = m_preflight.serializedReport(m_session->facade().source().path);
    const pdf::PDFOperationResult result = pdf::PDFSafeFileWriter::writeData(
        url.toLocalFile(), report, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!result)
    {
        announceDocumentState(tr("Could not export the preflight report: %1").arg(result.getErrorMessage()));
        return false;
    }
    announceDocumentState(tr("Preflight report exported."));
    return true;
}

void EditorHost::requestPreflightProfileImport()
{
    Q_EMIT preflightProfileImportRequested();
}

void EditorHost::requestPreflightProfileExport()
{
    Q_EMIT preflightProfileExportRequested();
}

void EditorHost::requestPreflightProfileSave()
{
    if (m_preflightProfileDraft.isActive())
    {
        Q_EMIT preflightProfileSaveRequested();
    }
}

bool EditorHost::importPreflightProfileFileUrl(const QUrl& url)
{
    if (!url.isValid() || !url.isLocalFile())
    {
        return false;
    }

    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly))
    {
        announceDocumentState(tr("Could not import the preflight profile."));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
    {
        announceDocumentState(tr("The selected profile is not valid JSON."));
        return false;
    }

    const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(parsed.object(), url.toLocalFile());
    if (!imported.ok)
    {
        announceDocumentState(imported.errorMessage);
        return false;
    }

    const QString configRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configRoot.isEmpty())
    {
        announceDocumentState(tr("Could not resolve the local profile directory."));
        return false;
    }
    const QString profilesDirectory = QDir(configRoot).filePath(QStringLiteral("profiles"));
    if (!QDir().mkpath(profilesDirectory))
    {
        announceDocumentState(tr("Could not create the local profile directory."));
        return false;
    }
    const QString baseName = QFileInfo(url.toLocalFile()).completeBaseName();
    const QString destination = QDir(profilesDirectory).filePath(baseName + QStringLiteral(".json"));
    const QByteArray bytes = pdf::serializePreflightProfileBytes(imported.profile);
    const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeData(
        destination, bytes, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!writeResult)
    {
        announceDocumentState(tr("Could not save the imported profile: %1").arg(writeResult.getErrorMessage()));
        return false;
    }

    reloadPreflightProfiles();
    selectPreflightProfile(destination);
    announceDocumentState(tr("Preflight profile imported."));
    return true;
}

bool EditorHost::beginPreflightProfileEdit()
{
    const auto it = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                 [this](const PreflightProfileChoice& profile)
                                 { return profile.id == m_selectedPreflightProfileId; });
    if (it == m_preflightProfiles.cend() || !it->valid)
    {
        return false;
    }

    const pdf::PreflightProfileIdentity identity = pdf::identifyPreflightProfile(it->profile, it->source);
    if (!m_preflightProfileDraft.load(it->profile, identity))
    {
        return false;
    }
    Q_EMIT preflightProfileDraftChanged();
    bumpPresentation();
    return true;
}

bool EditorHost::setPreflightCheckField(const QString& checkId, const QString& field, const QVariant& value)
{
    if (!m_preflightProfileDraft.setCheckField(checkId, field, value))
    {
        return false;
    }
    Q_EMIT preflightProfileDraftChanged();
    return true;
}

bool EditorHost::savePreflightProfileEdit(const QString& newVersion, const QUrl& url)
{
    if (!m_preflightProfileDraft.isActive() || newVersion.isEmpty() || !url.isValid() || !url.isLocalFile())
    {
        return false;
    }

    const QJsonObject committed = m_preflightProfileDraft.commit(newVersion);
    if (committed.isEmpty())
    {
        return false;
    }

    const QByteArray bytes = pdf::serializePreflightProfileBytes(committed);
    const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeData(
        url.toLocalFile(), bytes, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!writeResult)
    {
        announceDocumentState(tr("Could not save the profile fork: %1").arg(writeResult.getErrorMessage()));
        return false;
    }

    m_preflightProfileDraft.clear();
    reloadPreflightProfiles();
    selectPreflightProfile(url.toLocalFile());
    m_preflight.markCheckSetStale();
    Q_EMIT preflightProfileDraftChanged();
    announceDocumentState(tr("Preflight profile saved."));
    bumpPresentation();
    return true;
}

bool EditorHost::exportPreflightProfileFileUrl(const QUrl& url)
{
    if (!url.isValid() || !url.isLocalFile())
    {
        return false;
    }

    QJsonObject profile;
    if (m_preflightProfileDraft.isActive())
    {
        profile = m_preflightProfileDraft.draftProfile();
    }
    else
    {
        const auto it = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                     [this](const PreflightProfileChoice& choice)
                                     { return choice.id == m_selectedPreflightProfileId; });
        if (it == m_preflightProfiles.cend() || !it->valid)
        {
            return false;
        }
        profile = it->profile;
    }

    const QByteArray bytes = pdf::serializePreflightProfileBytes(profile);
    const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeData(
        url.toLocalFile(), bytes, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!writeResult)
    {
        announceDocumentState(tr("Could not export the preflight profile: %1").arg(writeResult.getErrorMessage()));
        return false;
    }
    announceDocumentState(tr("Preflight profile exported."));
    return true;
}

void EditorHost::cancelPreflightProfileEdit()
{
    if (!m_preflightProfileDraft.isActive())
    {
        return;
    }
    m_preflightProfileDraft.clear();
    Q_EMIT preflightProfileDraftChanged();
    bumpPresentation();
}


QVariantList EditorHost::actionListRecipes() const
{
    QVariantList recipes;
    recipes.reserve(m_actionListCatalog.recipes().size());
    for (const pdfinteraction::ActionListRecipeEntry& recipe : m_actionListCatalog.recipes())
    {
        QVariantMap item;
        item.insert(QStringLiteral("id"), recipe.id);
        item.insert(QStringLiteral("name"), recipe.name);
        item.insert(QStringLiteral("source"), recipe.source);
        item.insert(QStringLiteral("valid"), recipe.valid);
        item.insert(QStringLiteral("diagnostic"), recipe.diagnostic);
        item.insert(QStringLiteral("recipeHash"), recipe.recipeHash);
        item.insert(QStringLiteral("stepCount"), recipe.actionList.steps.size());
        recipes.append(item);
    }
    return recipes;
}

QString EditorHost::selectedActionListRecipeId() const
{
    return m_selectedActionListRecipeId;
}

QVariantList EditorHost::actionListBindings() const
{
    QVariantList bindings;
    for (auto it = m_actionListBindings.begin(); it != m_actionListBindings.end(); ++it)
    {
        QVariantMap item;
        item.insert(QStringLiteral("name"), it.key());
        item.insert(QStringLiteral("value"), it.value().toVariant());
        bindings.append(item);
    }
    return bindings;
}

QVariantList EditorHost::actionListSteps() const
{
    QVariantList steps;
    if (!m_actionListDraftValid)
    {
        return steps;
    }
    steps.reserve(m_actionListDraft.steps.size());
    for (int index = 0; index < m_actionListDraft.steps.size(); ++index)
    {
        const pdf::PDFActionListStep& step = m_actionListDraft.steps.at(index);
        QVariantMap item;
        item.insert(QStringLiteral("index"), index);
        item.insert(QStringLiteral("id"), step.id);
        item.insert(QStringLiteral("operation"), step.operationId);
        item.insert(QStringLiteral("parameters"), step.parameters.toVariantMap());
        item.insert(QStringLiteral("parameterSchema"),
                    pdfinteraction::repairParameterSchema(step.operationId).toVariantMap());
        steps.append(item);
    }
    return steps;
}

QString EditorHost::actionListStateName() const
{
    return actionListStateToString(m_actionListController.state());
}

QVariantList EditorHost::repairOperations() const
{
    const QJsonArray descriptors = pdfinteraction::repairOperationDescriptors();
    QVariantList operations;
    operations.reserve(descriptors.size());
    for (const QJsonValue& value : descriptors)
    {
        operations.append(value.toObject().toVariantMap());
    }
    return operations;
}

QVariantMap EditorHost::repairParameterSchemaForOperation(const QString& operationId) const
{
    return pdfinteraction::repairParameterSchema(operationId).toVariantMap();
}

bool EditorHost::importActionListRecipe(const QUrl& url)
{
    if (!url.isValid() || !url.isLocalFile())
    {
        return false;
    }
    QString importedId;
    QString error;
    if (!m_actionListCatalog.importRecipe(url.toLocalFile(), &importedId, &error))
    {
        announceDocumentState(error);
        return false;
    }
    m_selectedActionListRecipeId = importedId;
    m_actionListBindings = QJsonObject();
    syncActionListDraft();
    m_actionListController.markRecipeStale();
    updateActionListRecipeWatch();
    Q_EMIT actionListRecipesChanged();
    bumpPresentation();
    announceDocumentState(tr("Action List recipe imported."));
    return true;
}

bool EditorHost::exportActionListRecipe(const QUrl& url)
{
    if (!url.isValid() || !url.isLocalFile() || m_selectedActionListRecipeId.isEmpty())
    {
        return false;
    }
    QString error;
    if (!m_actionListCatalog.exportRecipe(m_selectedActionListRecipeId, url.toLocalFile(), &error))
    {
        announceDocumentState(error);
        return false;
    }
    announceDocumentState(tr("Action List recipe exported."));
    return true;
}

bool EditorHost::selectActionListRecipe(const QString& id)
{
    const pdfinteraction::ActionListRecipeEntry* recipe = m_actionListCatalog.recipe(id);
    if (!recipe || !recipe->valid || id == m_selectedActionListRecipeId)
    {
        return false;
    }
    m_selectedActionListRecipeId = id;
    m_actionListBindings = QJsonObject();
    syncActionListDraft();
    m_actionListController.markRecipeStale();
    Q_EMIT actionListRecipesChanged();
    bumpPresentation();
    return true;
}

bool EditorHost::setActionListBinding(const QString& name, const QVariant& value)
{
    if (name.trimmed().isEmpty())
    {
        return false;
    }
    const QJsonValue parsed = actionListEditorValue(value, QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } }, nullptr);
    const QString text = value.toString().trimmed();
    if (value.metaType().id() == QMetaType::QString)
    {
        if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
        {
            m_actionListBindings.insert(name, true);
        }
        else if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
        {
            m_actionListBindings.insert(name, false);
        }
        else
        {
            bool integerOk = false;
            const qlonglong integer = text.toLongLong(&integerOk);
            if (integerOk)
            {
                m_actionListBindings.insert(name, integer);
            }
            else
            {
                bool numberOk = false;
                const double number = text.toDouble(&numberOk);
                m_actionListBindings.insert(name, numberOk ? QJsonValue(number) : parsed);
            }
        }
    }
    else
    {
        m_actionListBindings.insert(name, parsed);
    }
    m_actionListController.markRecipeStale();
    Q_EMIT actionListRecipesChanged();
    bumpPresentation();
    return true;
}

bool EditorHost::setActionListStepParameter(int stepIndex, const QString& name, const QVariant& value)
{
    if (!m_actionListDraftValid || stepIndex < 0 || stepIndex >= m_actionListDraft.steps.size() || name.trimmed().isEmpty())
    {
        return false;
    }
    pdf::PDFActionListStep& step = m_actionListDraft.steps[stepIndex];
    const QJsonObject operationSchema = pdfinteraction::repairParameterSchema(step.operationId);
    const QJsonObject schema = operationSchema.value(QStringLiteral("properties")).toObject().value(name).toObject();
    if (schema.isEmpty())
    {
        return false;
    }
    bool omit = false;
    const QJsonValue converted = actionListEditorValue(value, schema, &omit);
    bool required = false;
    for (const QJsonValue& requiredValue : operationSchema.value(QStringLiteral("required")).toArray())
    {
        required = required || requiredValue.toString() == name;
    }
    if (omit && !required)
    {
        step.parameters.remove(name);
    }
    else
    {
        step.parameters.insert(name, converted);
    }
    m_actionListController.markRecipeStale();
    Q_EMIT actionListRecipesChanged();
    bumpPresentation();
    return true;
}

bool EditorHost::saveActionListRecipe()
{
    if (!m_actionListDraftValid || m_selectedActionListRecipeId.isEmpty())
    {
        return false;
    }
    QString error;
    if (!m_actionListCatalog.saveRecipe(m_selectedActionListRecipeId, m_actionListDraft, &error))
    {
        announceDocumentState(error);
        return false;
    }
    m_actionListController.markRecipeStale();
    reloadActionListRecipes();
    announceDocumentState(tr("Action List recipe saved."));
    return true;
}

bool EditorHost::submitActionListJob(pdfinteraction::ActionListRunPhase phase,
                                     pdfinteraction::ActionListController::State controllerState)
{
    if (!hasDocument() || m_selectedActionListRecipeId.isEmpty() || !m_session->revisionSource())
    {
        return false;
    }

    const pdfinteraction::ActionListController::State currentState = m_actionListController.state();
    if (currentState == pdfinteraction::ActionListController::State::Validating ||
        currentState == pdfinteraction::ActionListController::State::Planning ||
        currentState == pdfinteraction::ActionListController::State::Running)
    {
        return false;
    }

    const pdfinteraction::ActionListRecipeEntry* recipe = m_actionListCatalog.recipe(m_selectedActionListRecipeId);
    if (!recipe || !recipe->valid)
    {
        return false;
    }

    const pdf::PDFDocumentPointer document = m_session->context().getDocumentPointer();
    if (!document)
    {
        return false;
    }

    const QString documentKey = m_session->revisionSource()->documentKey();
    const QString documentRevision = m_session->facade().currentRevision().toString();
    const auto profileIt = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                        [this](const PreflightProfileChoice& profile)
                                        { return profile.id == m_selectedPreflightProfileId; });
    const QJsonObject profileForPlan = profileIt != m_preflightProfiles.cend() && profileIt->valid
                                           ? profileIt->profile
                                           : QJsonObject();
    const QString bindingsHash = actionListBindingsHash(QJsonObject{
        { QStringLiteral("recipe_bindings"), m_actionListBindings },
        { QStringLiteral("preflight_profile"), profileForPlan },
        { QStringLiteral("preflight_bindings"), m_preflightBindings } });
    if (phase == pdfinteraction::ActionListRunPhase::Plan &&
        !m_actionListController.validationMatches(documentKey, documentRevision, recipe->recipeHash, bindingsHash))
    {
        announceDocumentState(tr("Validate the current Action List recipe and bindings before planning."));
        return false;
    }
    if (phase == pdfinteraction::ActionListRunPhase::Execute &&
        !m_actionListController.planMatches(documentKey, documentRevision, recipe->recipeHash, bindingsHash))
    {
        m_actionListController.markRecipeStale();
        announceDocumentState(tr("The Action List plan is stale; validate and plan again."));
        bumpPresentation();
        return false;
    }
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    pdf::PDFJobSpec spec;
    spec.jobId = jobId;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = documentKey;
    spec.documentRevision = documentRevision;
    spec.operationId = QStringLiteral("action-list.%1").arg(recipe->actionList.id);
    spec.checkId = recipe->actionList.name;
    spec.progressModel = QStringLiteral("action-list-progress-v1");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    m_actionListController.beginRun(controllerState,
                                    documentKey,
                                    documentRevision,
                                    recipe->id,
                                    recipe->recipeHash,
                                    bindingsHash,
                                    jobId);
    auto outcome = std::make_shared<pdfinteraction::ActionListWorkerOutcome>();
    m_actionListOutcomes.insert(jobId, outcome);
    const pdf::PDFActionList actionList = recipe->actionList;
    const QJsonObject bindings = m_actionListBindings;
    QString preflightProfilePath;
    QJsonObject preflightProfile;
    QJsonObject preflightProfileBindings;
    if ((phase == pdfinteraction::ActionListRunPhase::Plan ||
         phase == pdfinteraction::ActionListRunPhase::Execute) &&
        profileIt != m_preflightProfiles.cend() && profileIt->valid)
    {
        preflightProfilePath = profileIt->source;
        preflightProfile = profileIt->profile;
        preflightProfileBindings = m_preflightBindings;
    }

    const QString submittedId = m_session->scheduler().submit(
        spec,
        pdfinteraction::makeActionListRunWorker(phase,
                                                actionList,
                                                document,
                                                bindings,
                                                outcome,
                                                preflightProfilePath,
                                                preflightProfile,
                                                preflightProfileBindings));
    if (submittedId != jobId)
    {
        m_actionListOutcomes.remove(jobId);
        m_actionListController.failRun(jobId, documentRevision, tr("Unable to submit Action List work."));
        return false;
    }

    bumpPresentation();
    return true;
}

bool EditorHost::validateActionListRecipe()
{
    return submitActionListJob(pdfinteraction::ActionListRunPhase::Validate,
                               pdfinteraction::ActionListController::State::Validating);
}

bool EditorHost::planActionList()
{
    return submitActionListJob(pdfinteraction::ActionListRunPhase::Plan,
                               pdfinteraction::ActionListController::State::Planning);
}

bool EditorHost::runActionList()
{
    if (m_actionListController.state() != pdfinteraction::ActionListController::State::Planned)
    {
        return false;
    }
    return submitActionListJob(pdfinteraction::ActionListRunPhase::Execute,
                               pdfinteraction::ActionListController::State::Running);
}

bool EditorHost::cancelActionList()
{
    return m_actionListController.cancelRun(m_actionListController.jobId());
}

bool EditorHost::confirmActionListPlan()
{
    return runActionList();
}

void EditorHost::discardActionListPlan()
{
    m_actionListController.discardPlan();
    bumpPresentation();
}

void EditorHost::reloadActionListRecipes()
{
    const QString priorId = m_selectedActionListRecipeId;
    const QString priorHash = priorId.isEmpty() || !m_actionListCatalog.recipe(priorId)
                                  ? QString()
                                  : m_actionListCatalog.recipe(priorId)->recipeHash;
    m_actionListCatalog.reload();
    if (m_selectedActionListRecipeId.isEmpty() ||
        !m_actionListCatalog.recipe(m_selectedActionListRecipeId) ||
        !m_actionListCatalog.recipe(m_selectedActionListRecipeId)->valid)
    {
        const QList<pdfinteraction::ActionListRecipeEntry>& recipes = m_actionListCatalog.recipes();
        const auto valid = std::find_if(recipes.cbegin(), recipes.cend(),
                                        [](const pdfinteraction::ActionListRecipeEntry& recipe)
                                        { return recipe.valid; });
        m_selectedActionListRecipeId = valid == recipes.cend() ? QString() : valid->id;
        m_actionListBindings = QJsonObject();
    }
    syncActionListDraft();
    const pdfinteraction::ActionListRecipeEntry* currentRecipe = m_actionListCatalog.recipe(m_selectedActionListRecipeId);
    if (!priorId.isEmpty() &&
        (priorId != m_selectedActionListRecipeId || !currentRecipe || priorHash != currentRecipe->recipeHash))
    {
        m_actionListController.markRecipeStale();
    }
    updateActionListRecipeWatch();
    Q_EMIT actionListRecipesChanged();
    bumpPresentation();
}

void EditorHost::syncActionListDraft()
{
    const pdfinteraction::ActionListRecipeEntry* recipe = m_actionListCatalog.recipe(m_selectedActionListRecipeId);
    if (!recipe || !recipe->valid)
    {
        m_actionListDraft = pdf::PDFActionList();
        m_actionListDraftValid = false;
        return;
    }
    m_actionListDraft = recipe->actionList;
    m_actionListDraftValid = true;
}

void EditorHost::updateActionListRecipeWatch()
{
    if (!m_actionListRecipeWatcher)
    {
        return;
    }
    m_actionListRecipeWatcher->removePaths(m_actionListRecipeWatcher->directories());
    m_actionListRecipeWatcher->removePaths(m_actionListRecipeWatcher->files());
    const QString localDirectory = m_actionListCatalog.recipesDirectory();
    if (QFileInfo::exists(localDirectory))
    {
        m_actionListRecipeWatcher->addPath(localDirectory);
    }
    for (const pdfinteraction::ActionListRecipeEntry& recipe : m_actionListCatalog.recipes())
    {
        if (QFileInfo::exists(recipe.source))
        {
            m_actionListRecipeWatcher->addPath(recipe.source);
        }
    }
}
void EditorHost::reloadPreflightProfiles()
{
    const QString priorId = m_selectedPreflightProfileId;
    QString priorDigest;
    QJsonArray priorChecks;
    for (const PreflightProfileChoice& profile : std::as_const(m_preflightProfiles))
    {
        if (profile.id == priorId)
        {
            priorDigest = profile.digest;
            priorChecks = profile.profile.value(QStringLiteral("checks")).toArray();
            break;
        }
    }

    QList<PreflightProfileChoice> profiles;
    const auto addProfile = [&profiles](const QString& source, const QByteArray& data)
    {
        PreflightProfileChoice choice;
        choice.id = source;
        choice.source = source;
        QJsonParseError parseError;
        const QJsonDocument parsed = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        {
            choice.name = QFileInfo(source).completeBaseName();
            choice.diagnostic = QStringLiteral("Profile JSON is invalid.");
            profiles.append(choice);
            return;
        }
        const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(parsed.object(), source);
        choice.name = imported.profile.value(QStringLiteral("name")).toString(QFileInfo(source).completeBaseName());
        choice.version = imported.identity.version;
        choice.digest = imported.identity.digest;
        choice.profile = imported.profile;
        choice.variables = imported.profile.value(QStringLiteral("variables")).toObject();
        choice.valid = imported.ok;
        choice.diagnostic = imported.ok ? QString() : imported.errorMessage;
        if (choice.valid)
        {
            // importPreflightProfile validates identity and authored digest;
            // the Core binder/parser is the authority for executable profile
            // validity. Binding here is deliberately read-only, so a required
            // operator variable remains selectable while its diagnostic is
            // exposed before the first run.
            const pdf::PreflightVariableBindResult bound =
                pdf::bindPreflightProfileVariables(choice.profile);
            if (bound.ok)
            {
                pdf::PreflightProfileData parsedProfile;
                QString profileParseError;
                if (!pdf::PreflightEngine::parseProfile(bound.profile, parsedProfile, profileParseError))
                {
                    choice.valid = false;
                    choice.diagnostic = profileParseError;
                }
            }
            else if (bound.errorCode != QStringLiteral("unresolved-variable"))
            {
                choice.valid = false;
                choice.diagnostic = bound.errorMessage;
            }
            else
            {
                choice.diagnostic = bound.errorMessage;
            }
        }
        profiles.append(choice);
    };

    const QDir bundled(QStringLiteral(":/profiles"));
    for (const QFileInfo& file : bundled.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name))
    {
        QFile input(file.filePath());
        if (input.open(QIODevice::ReadOnly))
        {
            addProfile(file.filePath(), input.readAll());
        }
    }

    const QString localDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                       .filePath(QStringLiteral("profiles"));
    const QDir local(localDirectory);
    for (const QFileInfo& file : local.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name))
    {
        QFile input(file.absoluteFilePath());
        if (input.open(QIODevice::ReadOnly))
        {
            addProfile(file.absoluteFilePath(), input.readAll());
        }
    }

    m_preflightProfiles = std::move(profiles);
    if (m_selectedPreflightProfileId.isEmpty() ||
        std::none_of(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                     [this](const PreflightProfileChoice& profile)
                     { return profile.id == m_selectedPreflightProfileId && profile.valid; }))
    {
        const auto valid = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                        [](const PreflightProfileChoice& profile)
                                        { return profile.valid; });
        m_selectedPreflightProfileId = valid == m_preflightProfiles.cend() ? QString() : valid->id;
        m_preflightBindings = QJsonObject();
    }
    const auto current = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                      [this](const PreflightProfileChoice& profile)
                                      { return profile.id == m_selectedPreflightProfileId; });
    if (!priorId.isEmpty() && (priorId != m_selectedPreflightProfileId || current == m_preflightProfiles.cend()))
    {
        m_preflight.markProfileStale();
        m_actionListController.markRecipeStale();
    }
    else if (!priorId.isEmpty() && current != m_preflightProfiles.cend() && current->digest != priorDigest)
    {
        if (priorChecks != current->profile.value(QStringLiteral("checks")).toArray())
        {
            m_preflight.markCheckSetStale();
        }
        else
        {
            m_preflight.markProfileStale();
        }
        m_actionListController.markRecipeStale();
    }
    updatePreflightProfileWatch();
    Q_EMIT preflightProfilesChanged();
    bumpPresentation();
}

void EditorHost::updatePreflightProfileWatch()
{
    if (!m_preflightProfileWatcher)
    {
        return;
    }
    m_preflightProfileWatcher->removePaths(m_preflightProfileWatcher->directories());
    m_preflightProfileWatcher->removePaths(m_preflightProfileWatcher->files());
    const QString localDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                       .filePath(QStringLiteral("profiles"));
    if (QFileInfo::exists(localDirectory))
    {
        m_preflightProfileWatcher->addPath(localDirectory);
    }
    for (const PreflightProfileChoice& profile : std::as_const(m_preflightProfiles))
    {
        if (!profile.source.startsWith(QLatin1Char(':')) && QFileInfo::exists(profile.source))
        {
            m_preflightProfileWatcher->addPath(profile.source);
        }
    }
}

QVariantList EditorHost::commandDescriptors() const
{
    QVariantList descriptors;
    descriptors.reserve(m_session->catalog().descriptors().size());
    for (const pdfinteraction::CommandDescriptor& descriptor : m_session->catalog().descriptors())
    {
        descriptors.append(descriptorToVariant(descriptor, m_session->catalog().isEnabled(descriptor.id)));
    }
    return descriptors;
}

bool EditorHost::isCommandEnabled(const QString& commandId) const
{
    return m_session->catalog().isEnabled(commandId);
}

quint64 EditorHost::invokeCommand(const QString& commandId, const QVariantMap& parameters)
{
    const pdfinteraction::CommandInvocationId invocation = m_session->catalog().invoke(commandId, parameters);
    if (invocation != pdfinteraction::InvalidCommandInvocation)
    {
        bumpCommandEpoch();
    }
    return invocation;
}

bool EditorHost::cancelCommand(quint64 invocationId)
{
    return m_session->catalog().cancelInvocation(invocationId);
}

void EditorHost::openFileUrl(const QUrl& url)
{
    if (!url.isValid() || !url.isLocalFile())
    {
        return;
    }

    QVariantMap parameters;
    parameters.insert(QStringLiteral("path"), url.toLocalFile());
    invokeCommand(pdfinteraction::DocumentFacade::OpenCommandId, parameters);
}

void EditorHost::saveAsFileUrl(const QUrl& url)
{
    if (!url.isValid() || !url.isLocalFile())
    {
        return;
    }

    QVariantMap parameters;
    parameters.insert(QStringLiteral("path"), url.toLocalFile());
    invokeCommand(pdfinteraction::DocumentFacade::SaveAsCommandId, parameters);
}

void EditorHost::reopenDocument()
{
    if (m_session->facade().reopen() != pdfinteraction::InvalidCommandInvocation)
    {
        bumpCommandEpoch();
    }
}

void EditorHost::cancelPendingOperation()
{
    if (m_session->facade().cancelPendingOperation())
    {
        bumpCommandEpoch();
    }
}

void EditorHost::attachCanvas(QObject* canvasObject)
{
    m_canvas = qobject_cast<pdfquick::LoopCanvasItem*>(canvasObject);
    if (m_canvas)
    {
        m_canvas->ensureTraceRecorder();
        m_canvas->setAsyncWorkKindsProvider([this]
                                            { return activeAsyncWorkKinds(); });
        refreshCanvasTrace();
    }
    if (m_documentBound)
    {
        bindCanvas();
    }
}

void EditorHost::detachCanvas()
{
    unbindCanvas();
    if (m_canvas)
    {
        m_canvas->setAsyncWorkKindsProvider({});
    }
    m_canvas.clear();
}

void EditorHost::setViewportGeometry(qreal pixelPerMM, qreal devicePixelRatio, int widthPx, int heightPx)
{
    if (m_canvas)
    {
        return;
    }

    if (pixelPerMM > 0.0)
    {
        m_session->viewport().setPixelPerMM(pixelPerMM);
    }

    if (devicePixelRatio > 0.0)
    {
        m_session->viewport().setDevicePixelRatio(devicePixelRatio);
    }

    if (widthPx > 0 && heightPx > 0)
    {
        m_session->viewport().setViewportSizePx(QSize(widthPx, heightPx));
        if (m_documentBound)
        {
            m_session->surfaces()->requestSurfaces();
        }
    }

    bumpPresentation();
}

void EditorHost::openInitialPath(const QString& path)
{
    if (path.isEmpty())
    {
        return;
    }

    const QUrl url = QUrl::fromUserInput(path);
    if (!url.isValid() || (!url.scheme().isEmpty() && url.scheme() != QStringLiteral("file")))
    {
        return;
    }

    QVariantMap parameters;
    parameters.insert(QStringLiteral("path"), path);
    invokeCommand(pdfinteraction::DocumentFacade::OpenCommandId, parameters);
}

QString EditorHost::shortcutForCommand(const QString& commandId) const
{
    const pdfinteraction::CommandDescriptor* descriptor = m_session->catalog().descriptor(commandId);
    if (!descriptor)
    {
        return QString();
    }

    if (!descriptor->shortcut.sequence.isEmpty())
    {
        return descriptor->shortcut.sequence;
    }

    if (descriptor->shortcut.standardKey.isEmpty())
    {
        return QString();
    }

    const QByteArray standardKeyLatin = descriptor->shortcut.standardKey.toLatin1();
    const QMetaEnum standardKeys = QMetaEnum::fromType<QKeySequence::StandardKey>();
    bool found = false;
    const int value = standardKeys.keyToValue(standardKeyLatin.constData(), &found);
    if (!found || !qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
    {
        return descriptor->shortcut.standardKey;
    }

    return QKeySequence(static_cast<QKeySequence::StandardKey>(value)).toString(QKeySequence::PortableText);
}

void EditorHost::connectFacade()
{
    connect(&m_session->context(), &pdf::PDFDocumentContext::revisionChanged,
            this,
            [this](const pdf::PDFRevisionIdentity&, const pdf::PDFRevisionIdentity&)
            {
                cancelPreflight();
                syncRevisionModels();
                if (m_documentBound)
                {
                    m_documentModel.setDocument(&m_session->context());
                    m_searchRow = -1;
                    bumpPresentation();
                }
            });

    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::stateChanged, this, [this](pdfinteraction::DocumentState state)
            {
                syncDocumentLifecycle();
                if (state == pdfinteraction::DocumentState::Empty || state == pdfinteraction::DocumentState::Error)
                {
                    onDocumentGone();
                }

                bumpPresentation();
                refreshFeatureAvailability();
                bumpCommandEpoch(); });

    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::facetsChanged, this, [this](pdfinteraction::DocumentFacets)
            {
                syncDocumentLifecycle();
                syncProductionState();
                bumpPresentation(); });

    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::documentReplaced, this, [this](quint64)
            {
                onDocumentGone();
                onDocumentReady();
                refreshFeatureAvailability();
                bumpPresentation();
                bumpCommandEpoch(); });

    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::documentClosed, this, [this](quint64)
            {
                onDocumentGone();
                refreshFeatureAvailability();
                bumpPresentation();
                bumpCommandEpoch(); });
}

void EditorHost::connectViewport()
{
    connect(&m_session->viewport(), &pdfinteraction::ViewportController::placementsChanged, this, &EditorHost::bumpPresentation);
    connect(&m_session->viewport(), &pdfinteraction::ViewportController::demandChanged, this, &EditorHost::bumpPresentation);
}

void EditorHost::connectInteraction()
{
    connect(m_session->interaction(),
            &pdfinteraction::InteractionController::selectionChanged,
            this,
            &EditorHost::onInteractionSelectionChanged);
    connect(m_session->interaction(),
            &pdfinteraction::InteractionController::dragCompleted,
            this,
            &EditorHost::onDragCompleted);
}

void EditorHost::connectSurfaces()
{
    // The coordinator outlives every document (see DocumentViewSession), so
    // this connects once rather than per-document. Both signals mean an
    // admitted surface -- and so possibly this page's diagnostics -- changed;
    // bumpPresentation() re-reads pageFidelityIsExact/pageFidelityReason from
    // whatever is admitted now.
    connect(m_session->surfaces(), &pdfinteraction::PageSurfaceCoordinator::snapshotChanged, this, &EditorHost::bumpPresentation);
    connect(m_session->surfaces(), &pdfinteraction::PageSurfaceCoordinator::surfaceTerminal, this, [this](pdfinteraction::PageSurfaceKey, pdfinteraction::SurfaceTerminalState)
            { bumpPresentation(); });
}

void EditorHost::registerShellHandlers()
{
    pdfinteraction::CommandCatalog::Handler quit;
    quit.invoke = [this](pdfinteraction::CommandInvocationId invocation, const QVariantMap&)
    {
        m_session->catalog().finishInvocation(invocation, pdfinteraction::CommandTerminalState::Completed);
        QCoreApplication::quit();
    };
    m_session->catalog().setHandler(QuitCommandId, std::move(quit));
    m_session->catalog().setEnabled(QuitCommandId, true);
}

void EditorHost::registerFeatureHandlers()
{
    auto bind = [this](const QString& id, std::function<void()> action)
    {
        pdfinteraction::CommandCatalog::Handler handler;
        handler.invoke = [this, action = std::move(action)](pdfinteraction::CommandInvocationId invocation,
                                                            const QVariantMap&)
        {
            action();
            m_session->catalog().finishInvocation(invocation, pdfinteraction::CommandTerminalState::Completed);
            bumpPresentation();
        };
        m_session->catalog().setHandler(id, std::move(handler));
    };

    bind(QStringLiteral("actionPageLayoutContinuous"), [this]
         { m_session->viewport().setPageLayout(pdfinteraction::PageLayout::OneColumn); });
    bind(QStringLiteral("actionPageLayoutSinglePage"), [this]
         { m_session->viewport().setPageLayout(pdfinteraction::PageLayout::SinglePage); });
    bind(QStringLiteral("actionPageLayoutTwoColumns"), [this]
         { m_session->viewport().setPageLayout(pdfinteraction::PageLayout::TwoColumnLeft); });
    bind(QStringLiteral("actionPageLayoutTwoPages"), [this]
         { m_session->viewport().setPageLayout(pdfinteraction::PageLayout::TwoPagesLeft); });
    bind(QStringLiteral("actionFullscreenMode"), [this]
         { m_fullscreenRequested = !m_fullscreenRequested; });
    bind(QStringLiteral("actionFind"), [this]
         {
             m_searchPanelVisible = true;
             setWorkspace(LoopWorkspace::Document); });
    bind(QStringLiteral("actionFindNext"), [this]
         { moveSearch(1); });
    bind(QStringLiteral("actionFindPrevious"), [this]
         { moveSearch(-1); });
    bind(QStringLiteral("actionProperties"), [this]
         { setWorkspace(LoopWorkspace::Inspect); });
    refreshFeatureAvailability();
}

void EditorHost::refreshFeatureAvailability()
{
    const bool ready = hasDocument();
    QHash<pdfinteraction::CommandId, bool> availability;
    for (const QString& id : { QStringLiteral("actionPageLayoutContinuous"), QStringLiteral("actionPageLayoutSinglePage"),
                               QStringLiteral("actionPageLayoutTwoColumns"), QStringLiteral("actionPageLayoutTwoPages"),
                               QStringLiteral("actionFind"), QStringLiteral("actionProperties") })
    {
        availability.insert(id, ready);
    }
    const bool hasSearchResults = ready && m_documentModel.searchResultCount() > 0;
    availability.insert(QStringLiteral("actionFindNext"), hasSearchResults);
    availability.insert(QStringLiteral("actionFindPrevious"), hasSearchResults);
    availability.insert(QStringLiteral("actionFullscreenMode"), true);
    m_session->catalog().setEnabledBatch(availability);
}

void EditorHost::moveSearch(int direction)
{
    const int count = m_documentModel.searchResults()->rowCount();
    if (count == 0)
    {
        return;
    }

    if (m_searchRow < 0)
    {
        m_searchRow = direction > 0 ? 0 : count - 1;
    }
    else
    {
        m_searchRow = (m_searchRow + direction + count) % count;
    }
    goToPage(m_documentModel.searchPageAt(m_searchRow));
}

void EditorHost::refreshHitTestSources()
{
    const pdfinteraction::PreflightController::State state = m_preflight.state();
    const bool current = state == pdfinteraction::PreflightController::State::Pass ||
                         state == pdfinteraction::PreflightController::State::Findings ||
                         state == pdfinteraction::PreflightController::State::Incomplete;
    m_preflightOverlayBridge.setPresentationEnabled(current);
    m_findingsHitTest.setTargets(current ? m_preflight.findingsModel()->interactionTargets()
                                         : QList<pdfinteraction::InteractionTarget>{});
    m_preflightOverlayBridge.applyFindings();
}

void EditorHost::connectCatalog()
{
    connect(&m_session->catalog(), &pdfinteraction::CommandCatalog::availabilityChanged, this, &EditorHost::bumpCommandEpoch);
}

void EditorHost::bumpPresentation()
{
    updateCanvasAccessibilitySummary();
    if (m_canvas)
    {
        m_canvas->setHighContrast(highContrast());
    }
    Q_EMIT presentationChanged();
}

void EditorHost::bumpCommandEpoch()
{
    ++m_commandEpoch;
    Q_EMIT commandEpochChanged();
}

void EditorHost::onDocumentReady()
{
    m_session->prepareDocumentView();

    syncRevisionModels();
    m_documentModel.setDocument(&m_session->context());
    syncDocumentLifecycle();
    m_searchRow = -1;
    refreshHitTestSources();
    m_documentBound = true;
    bindCanvas();
    updateCanvasAccessibilitySummary();
    applyEmptyCanvasInspectorSelection();
    announceDocumentState(tr("Document ready."));
}

void EditorHost::syncDocumentLifecycle()
{
    const auto& facade = m_session->facade();
    QString outputState;
    switch (facade.outputState())
    {
        case pdfinteraction::DocumentOutputState::None:
            outputState = QStringLiteral("none");
            break;
        case pdfinteraction::DocumentOutputState::Pending:
            outputState = QStringLiteral("pending");
            break;
        case pdfinteraction::DocumentOutputState::Saved:
            outputState = QStringLiteral("saved");
            break;
    }

    m_documentModel.setLifecycleState(QString::fromLatin1(pdfinteraction::getDocumentStateName(facade.state())),
                                      facade.facets().testFlag(pdfinteraction::DocumentFacet::Dirty),
                                      facade.facets().testFlag(pdfinteraction::DocumentFacet::Stale),
                                      std::move(outputState), facade.typedError());
    refreshPreflightCertificateState();
}

void EditorHost::onDocumentGone()
{
    cancelPreflight();
    cancelActionList();
    if (m_findingNavigator)
    {
        m_findingNavigator->invalidate();
    }
    unbindCanvas();
    m_session->clearDocumentView();
    m_preflight.clear();
    m_actionListController.clear();
    m_inspector.clearSelection();
    m_documentModel.clear();
    m_searchRow = -1;
    m_preview.clear();
    m_production.clear();
    m_session->hitTest()->clearSources();
    m_documentBound = false;
    m_preflightCertificateStateName = QStringLiteral("not-certified");
    m_preflightCertificateSummary = tr("No certified preflight is recorded for this document.");
    updateCanvasAccessibilitySummary();
}

void EditorHost::bindCanvas()
{
    if (!m_canvas || !m_documentBound)
    {
        return;
    }

    m_session->hitTest()->clearSources();
    m_session->hitTest()->addSource(&m_findingsHitTest);
    m_session->hitTest()->addSource(&m_session->pageBoxSource());
    m_session->pageBoxSource().setEdgeTolerance(2.0 / qMax(m_session->viewport().zoom(), qreal(0.01)));

    m_canvas->bind(&m_session->viewport(), m_session->interaction(), m_session->surfaces());
}

void EditorHost::unbindCanvas()
{
    if (!m_canvas)
    {
        return;
    }

    m_canvas->bind(nullptr, nullptr, nullptr);
}

QStringList EditorHost::activeAsyncWorkKinds() const
{
    QStringList kinds;
    kinds.reserve(m_activeAsyncJobs.size());
    for (auto it = m_activeAsyncJobs.cbegin(); it != m_activeAsyncJobs.cend(); ++it)
    {
        kinds.append(QString::fromLatin1(pdf::getPDFJobKindName(it.value())));
    }
    kinds.removeDuplicates();
    std::sort(kinds.begin(), kinds.end());
    return kinds;
}

void EditorHost::acceptPreflightResult(const QString& jobId,
                                       const QString& documentRevision,
                                       const pdf::PreflightResult& result)
{
    if (m_acceptPreflightResults && m_preflight.acceptResult(jobId, documentRevision, result))
    {
        refreshCanvasTrace();
        bumpPresentation();
    }
}

void EditorHost::finishPreflightJob(const pdf::PDFJobSnapshot& snapshot)
{
    const std::shared_ptr<PreflightWorkerOutcome> outcome = m_preflightOutcomes.take(snapshot.jobId);
    if (snapshot.jobId != m_preflight.jobId() || m_preflight.state() != pdfinteraction::PreflightController::State::Running)
    {
        return;
    }

    switch (snapshot.status)
    {
        case pdf::PDFJobStatus::Succeeded:
            if (outcome)
            {
                acceptPreflightResult(snapshot.jobId, snapshot.documentRevision, outcome->result);
            }
            else
            {
                m_preflight.failRun(snapshot.jobId, snapshot.documentRevision, tr("Preflight result was unavailable."));
            }
            break;
        case pdf::PDFJobStatus::Failed:
            m_preflight.failRun(snapshot.jobId, snapshot.documentRevision, snapshot.errorMessage);
            break;
        case pdf::PDFJobStatus::Cancelled:
            m_preflight.cancelRun(snapshot.jobId);
            break;
        case pdf::PDFJobStatus::Stale:
            m_actionListController.markRecipeStale();
            syncRevisionModels();
            bumpPresentation();
            break;
        case pdf::PDFJobStatus::Queued:
        case pdf::PDFJobStatus::Running:
            break;
    }
}

void EditorHost::finishActionListJob(const pdf::PDFJobSnapshot& snapshot)
{
    const std::shared_ptr<pdfinteraction::ActionListWorkerOutcome> outcome = m_actionListOutcomes.take(snapshot.jobId);
    if (snapshot.jobId != m_actionListController.jobId())
    {
        return;
    }

    const pdfinteraction::ActionListController::State state = m_actionListController.state();
    if (state != pdfinteraction::ActionListController::State::Validating &&
        state != pdfinteraction::ActionListController::State::Planning &&
        state != pdfinteraction::ActionListController::State::Running)
    {
        return;
    }

    switch (snapshot.status)
    {
        case pdf::PDFJobStatus::Succeeded:
            if (!outcome)
            {
                m_actionListController.failRun(snapshot.jobId, snapshot.documentRevision,
                                               tr("Action List result was unavailable."));
                break;
            }
            if (state == pdfinteraction::ActionListController::State::Validating)
            {
                if (m_acceptActionListResults)
                {
                    m_actionListController.acceptValidation(snapshot.jobId, snapshot.documentRevision,
                                                            outcome->validationErrors,
                                                            outcome->validationSteps);
                }
            }
            else if (state == pdfinteraction::ActionListController::State::Planning)
            {
                if (m_acceptActionListResults)
                {
                    m_actionListController.acceptPlan(snapshot.jobId, snapshot.documentRevision, outcome->executionResult);
                }
            }
            else if (state == pdfinteraction::ActionListController::State::Running)
            {
                if (m_acceptActionListResults &&
                    m_actionListController.acceptExecution(snapshot.jobId, snapshot.documentRevision, outcome->executionResult) &&
                    outcome->candidate)
                {
                    m_session->context().setDocument(outcome->candidate);
                    m_preflight.markProfileStale();
                    syncRevisionModels();
                    announceDocumentState(tr("Action List applied to the open document."));
                }
            }
            bumpPresentation();
            break;
        case pdf::PDFJobStatus::Failed:
            m_actionListController.failRun(snapshot.jobId, snapshot.documentRevision,
                                           snapshot.errorMessage.isEmpty() ? tr("Action List failed.")
                                                                           : snapshot.errorMessage);
            bumpPresentation();
            break;
        case pdf::PDFJobStatus::Cancelled:
            m_actionListController.cancelRun(snapshot.jobId);
            bumpPresentation();
            break;
        case pdf::PDFJobStatus::Stale:
            syncRevisionModels();
            break;
        case pdf::PDFJobStatus::Queued:
        case pdf::PDFJobStatus::Running:
            break;
    }
}

void EditorHost::refreshCanvasTrace()
{
    if (m_canvas)
    {
        m_canvas->update();
    }
}

void EditorHost::refreshPreflightCertificateState()
{
    m_preflightCertificateStateName = QStringLiteral("not-certified");
    m_preflightCertificateSummary = tr("No certified preflight is recorded for this document.");

    if (!hasDocument())
        return;

    const QString documentPath = m_session->facade().source().path;
    if (documentPath.isEmpty())
        return;

    const QString historyDirectory = QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history");
    const QString historyPath = QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3"));
    if (!QFileInfo::exists(historyPath))
        return;

    pdf::PDFOperationHistoryStore history(historyPath);
    QString historyError;
    if (!history.open(&historyError))
    {
        m_preflightCertificateStateName = QStringLiteral("certificate-invalid");
        m_preflightCertificateSummary = tr("Certified preflight history could not be opened: %1").arg(historyError);
        return;
    }

    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&historyError);
    if (!historyError.isEmpty())
    {
        m_preflightCertificateStateName = QStringLiteral("certificate-invalid");
        m_preflightCertificateSummary = tr("Certified preflight history could not be read: %1").arg(historyError);
        return;
    }

    QString certificateError;
    const std::optional<pdf::PreflightCertificate> certificate =
        pdf::latestPreflightCertificate(events, &certificateError);
    if (!certificate.has_value())
    {
        if (!certificateError.isEmpty())
        {
            m_preflightCertificateStateName = QStringLiteral("certificate-invalid");
            m_preflightCertificateSummary = certificateError;
        }
        return;
    }

    if (m_session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Dirty))
    {
        m_preflightCertificateStateName = QStringLiteral("certificate-invalid");
        m_preflightCertificateSummary = tr("The certified revision has unsaved document changes.");
        return;
    }

    QFile document(documentPath);
    if (!document.open(QIODevice::ReadOnly))
    {
        m_preflightCertificateStateName = QStringLiteral("certificate-invalid");
        m_preflightCertificateSummary = tr("The certified document bytes could not be read.");
        return;
    }

    const pdf::PreflightCertificateVerification verification =
        pdf::verifyPreflightCertificate(*certificate, document.readAll(), events);
    m_preflightCertificateStateName =
        verification.isValid() ? QStringLiteral("certified") : QStringLiteral("certificate-invalid");
    m_preflightCertificateSummary = verification.reason;
}

void EditorHost::syncRevisionModels()
{
    if (!m_session->revisionSource())
    {
        return;
    }

    if (m_findingNavigator)
    {
        m_findingNavigator->invalidate();
    }

    const QString documentKey = m_session->revisionSource()->documentKey();
    const QString documentRevision = m_session->facade().currentRevision().toString();
    m_preflight.setCurrentRevision(documentKey, documentRevision);
    m_actionListController.setCurrentRevision(documentKey, documentRevision);
    m_inspector.setCurrentRevision(documentKey, documentRevision);
    m_preview.setCurrentRevision(documentKey, documentRevision);
    m_production.setCurrentRevision(documentKey, documentRevision);
    refreshPreflightCertificateState();

    if (hasDocument())
    {
        m_preview.setState(documentKey,
                           documentRevision,
                           pdfinteraction::PreviewStateModel::Authority::Approximate,
                           tr("Production preview is approximate until proof mode is active."),
                           tr("The current view uses the standard render path."),
                           QString());
        syncProductionState();
    }
}

void EditorHost::syncProductionState()
{
    if (!m_session->revisionSource() || !hasDocument())
    {
        return;
    }

    const QString documentKey = m_session->revisionSource()->documentKey();
    const QString documentRevision = m_session->facade().currentRevision().toString();
    pdfinteraction::ProductionModel::State state = pdfinteraction::ProductionModel::State::Ready;
    if (m_session->facade().outputState() == pdfinteraction::DocumentOutputState::Pending)
    {
        state = pdfinteraction::ProductionModel::State::OperationPending;
    }
    else if (m_session->facade().outputState() == pdfinteraction::DocumentOutputState::Saved)
    {
        state = pdfinteraction::ProductionModel::State::OutputWritten;
    }
    else if (m_preview.status() == pdfinteraction::PreviewStateModel::Status::Unavailable)
    {
        state = pdfinteraction::ProductionModel::State::NotReady;
    }

    m_production.setState(documentKey, documentRevision, state);
}

void EditorHost::updateCanvasAccessibilitySummary()
{
    if (!m_canvas)
    {
        return;
    }

    if (!hasDocument())
    {
        m_canvas->setAccessibleDocumentSummary(tr("No document is currently open."));
        return;
    }

    const int pageNumber = currentPage() + 1;
    const int pages = pageCount();
    const int zoomPercent = qRound(zoom() * 100.0);
    m_canvas->setAccessibleDocumentSummary(
        tr("Document canvas. Page %1 of %2. Zoom %3 percent.").arg(pageNumber).arg(pages).arg(zoomPercent));
}

void EditorHost::onPreflightNavigation(pdfinteraction::PreflightController::EvidenceNavigationRequest request)
{
    if (!m_session->revisionSource() ||
        request.documentKey != m_session->revisionSource()->documentKey() ||
        request.documentRevision != m_session->facade().currentRevision().toString())
    {
        return;
    }

    const pdfinteraction::PreflightFindingView* finding = m_preflight.findingsModel()->finding(request.findingId);
    if (!finding || !m_findingNavigator)
    {
        return;
    }

    const pdfinteraction::FindingNavigationResult result =
        m_findingNavigator->navigate(pdfinteraction::FindingNavigationRequest::fromFinding(*finding));
    if (!result.accepted())
    {
        return;
    }
    bumpPresentation();
}

bool EditorHost::isWorkspaceEnabled(LoopWorkspace workspace) const
{
    // QML supplies registered enum values, but the public invokable can also
    // be reached through QVariant/int callers. Reject out-of-range values and
    // the explicitly deferred Compare destination fail-closed.
    if (workspace < LoopWorkspace::Document || workspace > LoopWorkspace::Compare)
    {
        return false;
    }

    return workspace != LoopWorkspace::Compare;
}

void EditorHost::setInspectionMode(QString mode)
{
    mode = mode.trimmed().isEmpty() ? QStringLiteral("page") : std::move(mode);
    if (m_inspectionMode == mode)
    {
        return;
    }
    m_inspectionMode = std::move(mode);
}

void EditorHost::onDragCompleted(pdfinteraction::DragSession session)
{
    Q_UNUSED(session);
    if (m_session->interaction())
    {
        m_session->interaction()->refreshOverlay();
    }
}

void EditorHost::onInteractionSelectionChanged(pdfinteraction::InteractionTarget target)
{
    applyInspectorSelection(target);
    bumpPresentation();
}

void EditorHost::applyEmptyCanvasInspectorSelection()
{
    setInspectionMode(QStringLiteral("page"));
    if (!m_session->revisionSource() || !hasDocument())
    {
        m_inspector.clearSelection();
        return;
    }

    pdfinteraction::InspectorModel::Selection selection;
    selection.documentKey = m_session->revisionSource()->documentKey();
    selection.documentRevision = m_session->facade().currentRevision().toString();
    selection.selectionId = QStringLiteral("canvas");
    selection.title = tr("Document canvas");
    selection.kind = pdfinteraction::InspectorModel::SelectionKind::EmptyCanvas;
    selection.properties = {
        { QStringLiteral("document"), QStringLiteral("Document"), displayTitle() },
        { QStringLiteral("pages"), QStringLiteral("Pages"), QString::number(pageCount()) },
        { QStringLiteral("document-status"), QStringLiteral("Document status"), documentShellStatus() },
        { QStringLiteral("preflight"), QStringLiteral("Preflight"), preflightStateName() },
        { QStringLiteral("production"), QStringLiteral("Production"), productionStateName() },
    };
    m_inspector.setSelection(selection);
}

void EditorHost::applyInspectorSelection(const pdfinteraction::InteractionTarget& target)
{
    if (!m_session->revisionSource() || !hasDocument())
    {
        applyEmptyCanvasInspectorSelection();
        return;
    }

    if (!target.isValid())
    {
        applyEmptyCanvasInspectorSelection();
        return;
    }

    const QString documentKey = m_session->revisionSource()->documentKey();
    const QString documentRevision = m_session->facade().currentRevision().toString();

    if (target.kind == pdfinteraction::InteractionTargetKind::Finding)
    {
        if (m_preflight.findingsModel()->selectedFindingId() != target.id ||
            m_inspector.selectionKind() != pdfinteraction::InspectorModel::SelectionKind::Finding ||
            m_inspector.selectionId() != target.id)
        {
            selectFinding(target.id);
        }
        return;
    }

    if (target.id.startsWith(QStringLiteral("image:")))
    {
        pdfinteraction::InspectorModel::Selection selection;
        selection.documentKey = documentKey;
        selection.documentRevision = documentRevision;
        selection.selectionId = target.id;
        selection.title = tr("Image");
        selection.kind = pdfinteraction::InspectorModel::SelectionKind::Image;
        selection.properties = {
            { QStringLiteral("id"), QStringLiteral("Image"), target.id.mid(6) },
            { QStringLiteral("page"), QStringLiteral("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("bounds"), QStringLiteral("Bounds"),
              QStringLiteral("%1,%2 %3x%4")
                  .arg(QString::number(target.pageBounds.x()),
                       QString::number(target.pageBounds.y()),
                       QString::number(target.pageBounds.width()),
                       QString::number(target.pageBounds.height())) },
            { QStringLiteral("dpi"), QStringLiteral("Effective DPI"), tr("pending") },
            { QStringLiteral("colour-space"), QStringLiteral("Colour space"), tr("pending") },
            { QStringLiteral("compression"), QStringLiteral("Compression"), tr("pending") },
            { QStringLiteral("mask"), QStringLiteral("Mask"), tr("pending") },
        };
        m_inspector.setSelection(selection);
        return;
    }

    if (target.id.startsWith(QStringLiteral("separation:")))
    {
        pdfinteraction::InspectorModel::Selection selection;
        selection.documentKey = documentKey;
        selection.documentRevision = documentRevision;
        selection.selectionId = target.id;
        selection.title = tr("Separation");
        selection.kind = pdfinteraction::InspectorModel::SelectionKind::Separation;
        selection.properties = {
            { QStringLiteral("name"), QStringLiteral("Ink"), target.id.mid(11) },
            { QStringLiteral("page"), QStringLiteral("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("coverage"), QStringLiteral("Ink coverage"), tr("pending") },
            { QStringLiteral("kind"), QStringLiteral("Process / spot"), tr("pending") },
        };
        m_inspector.setSelection(selection);
        return;
    }

    if (target.kind == pdfinteraction::InteractionTargetKind::Page ||
        target.kind == pdfinteraction::InteractionTargetKind::PageBox)
    {
        pdfinteraction::InspectorModel::Selection selection;
        selection.documentKey = documentKey;
        selection.documentRevision = documentRevision;
        selection.selectionId = target.id.isEmpty() ? QStringLiteral("page") : target.id;
        selection.title = target.kind == pdfinteraction::InteractionTargetKind::PageBox
                              ? tr("Page box: %1").arg(target.id)
                              : tr("Page %1").arg(target.pageIndex + 1);
        selection.kind = pdfinteraction::InspectorModel::SelectionKind::Page;
        selection.properties = {
            { QStringLiteral("page"), QStringLiteral("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("box"), QStringLiteral("Box"), target.id },
            { QStringLiteral("size"), QStringLiteral("Size"),
              QStringLiteral("%1 x %2")
                  .arg(QString::number(target.pageBounds.width()), QString::number(target.pageBounds.height())) },
            { QStringLiteral("rotation"), QStringLiteral("Rotation"), QStringLiteral("%1°").arg(rotationDegrees()) },
            { QStringLiteral("ocg"), QStringLiteral("Optional content"), m_documentModel.hasOptionalContent() ? tr("present") : tr("none") },
        };
        m_inspector.setSelection(selection);
        return;
    }

    applyEmptyCanvasInspectorSelection();
}
