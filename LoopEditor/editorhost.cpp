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
#include "preflightcontroller.h"
#include "preflightclirun.h"
#include "preflightrunsubmitter.h"
#include "shellinspectordispatch.h"
#include "previewstatemodel.h"
#include "productionmodel.h"

#include "pdfdocumentsession.h"
#include "pdfsafefilewriter.h"

#include "pdfblockingthreadguard.h"
#include "pdfpage.h"
#include "pdftransparencyrenderer.h"

#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QAccessibilityHints>
#include <QCoreApplication>
#include <QFile>
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

pdfquick::tokens::LoopStateVisual resolvedPreflightVisual(const QString& stateName)
{
    return pdfquick::tokens::resolvePreflightStateVisual(stateName);
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
    entry.insert(QStringLiteral("menuGroup"), descriptor.menuGroup);

    QVariantMap shortcut;
    shortcut.insert(QStringLiteral("standardKey"), descriptor.shortcut.standardKey);
    shortcut.insert(QStringLiteral("sequence"), descriptor.shortcut.sequence);
    entry.insert(QStringLiteral("shortcut"), shortcut);
    entry.insert(QStringLiteral("shortcutText"),
                 descriptor.shortcut.sequence.isEmpty() ? descriptor.shortcut.standardKey : descriptor.shortcut.sequence);
    return entry;
}

}   // namespace

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
    m_preflight(&m_session->scheduler(), this)
{
    pdf::PDFBlockingThreadGuard::registerInteractiveThread();

    connect(&m_preflightProfileCatalog, &pdfinteraction::PreflightProfileCatalog::changed, this,
            [this]
            {
                Q_EMIT preflightProfilesChanged();
                bumpPresentation();
            });
    connect(&m_preflightProfileCatalog, &pdfinteraction::PreflightProfileCatalog::profileStaleRequested, this,
            [this]
            { m_preflight.markProfileStale(); });
    connect(&m_preflightProfileCatalog, &pdfinteraction::PreflightProfileCatalog::checkSetStaleRequested, this,
            [this]
            { m_preflight.markCheckSetStale(); });

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

    connect(&m_preflight, &pdfinteraction::PreflightController::stateChanged, this, &EditorHost::bumpPresentation);
    connect(&m_preflight, &pdfinteraction::PreflightController::progressChanged, this, &EditorHost::bumpPresentation);
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
            { m_preflight.updateProgress(snapshot.jobId, snapshot.documentRevision, snapshot.progress); });
    connect(&m_session->scheduler(), &pdf::PDFJobScheduler::jobFinished, this, [this](const pdf::PDFJobSnapshot& snapshot)
            {
                m_activeAsyncJobs.remove(snapshot.jobId);
                finishPreflightJob(snapshot);
                refreshCanvasTrace(); });
}

EditorHost::~EditorHost()
{
    m_acceptPreflightResults = false;
    cancelPreflight();
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
    const pdfquick::tokens::LoopStateVisual visual = resolvedPreflightVisual(preflightStateName());

    QVariantMap result;
    result.insert(QStringLiteral("kind"), pdfquick::tokens::stateKindName(visual.kind));
    result.insert(QStringLiteral("colorRole"), pdfquick::tokens::colorRoleName(visual.colorRole));
    result.insert(QStringLiteral("icon"), pdfquick::tokens::stateIconName(visual.icon));
    result.insert(QStringLiteral("accessibleName"), visual.accessibleName);
    return result;
}

QColor EditorHost::preflightStateColor() const
{
    const pdfquick::tokens::LoopStateVisual visual = resolvedPreflightVisual(preflightStateName());
    const pdfquick::tokens::LoopTheme theme =
        highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    return pdfquick::tokens::color(visual.colorRole, theme);
}

QString EditorHost::preflightOperatorSummary() const
{
    return m_preflight.operatorSummary();
}

QVariantList EditorHost::preflightProfiles() const
{
    QVariantList profiles;
    profiles.reserve(m_preflightProfileCatalog.profiles().size());
    for (const pdfinteraction::PreflightProfileChoice& profile : m_preflightProfileCatalog.profiles())
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
    const pdfinteraction::PreflightProfileChoice* selected = m_preflightProfileCatalog.selectedProfile();
    if (selected == nullptr)
    {
        return {};
    }

    QVariantList variables;
    const QStringList names = selected->variables.keys();
    const QJsonObject bindings = m_preflightProfileCatalog.bindings();
    for (const QString& name : names)
    {
        const QJsonObject declaration = selected->variables.value(name).toObject();
        QVariantMap item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("type"), declaration.value(QStringLiteral("type")).toString());
        item.insert(QStringLiteral("required"), declaration.value(QStringLiteral("required")).toBool());
        item.insert(QStringLiteral("description"), declaration.value(QStringLiteral("description")).toString());
        item.insert(QStringLiteral("value"), bindings.contains(name)
                                                 ? bindings.value(name).toVariant()
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
    return m_preflightProfileCatalog.selectedProfileId();
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

    const QString documentRevision = m_session->facade().currentRevision().toString();
    m_preflight.findingsModel()->setSelectedFinding(findingId);
    m_inspector.setFindingSelection(*m_preflight.findingsModel(), findingId, documentRevision);

    pdfinteraction::PreflightController::EvidenceNavigationRequest request;
    if (!m_preflight.navigationFor(findingId, &request))
    {
        setInspectionMode(QStringLiteral("page"));
        bumpPresentation();
        return;
    }

    onPreflightNavigation(request);
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

    const pdfinteraction::PreflightProfileChoice* profile = m_preflightProfileCatalog.selectedProfile();
    if (profile == nullptr || !profile->valid)
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
    spec.operationId = QStringLiteral("preflight.%1").arg(profile->id);
    spec.checkId = profile->name;
    spec.progressModel = QStringLiteral("preflight-progress-v1");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    m_preflight.beginRun(documentKey,
                         documentRevision,
                         profile->digest,
                         jobId);
    auto outcome = std::make_shared<pdfinteraction::PreflightRunOutcome>();
    m_preflightOutcomes.insert(jobId, outcome);

    pdfinteraction::PreflightRunRequest request;
    request.document = document;
    request.profile = *profile;
    request.bindings = m_preflightProfileCatalog.bindings();
    request.sourceHash = m_session->context().getDocumentIdentity().sourceDataHash;

    const QString submittedId = m_session->scheduler().submit(spec, pdfinteraction::makePreflightRunWorker(request, outcome));
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
    if (!m_preflightProfileCatalog.selectProfile(id))
    {
        return false;
    }
    bumpPresentation();
    return true;
}

bool EditorHost::setPreflightVariable(const QString& name, const QVariant& value)
{
    if (!m_preflightProfileCatalog.setVariable(name, value))
    {
        return false;
    }
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
    m_findingsHitTest.setTargets(m_preflight.findingsModel()->interactionTargets());
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
    onInteractionSelectionChanged({});
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
}

void EditorHost::onDocumentGone()
{
    cancelPreflight();
    if (m_findingNavigator)
    {
        m_findingNavigator->invalidate();
    }
    unbindCanvas();
    m_session->clearDocumentView();
    m_preflight.clear();
    m_inspector.clearSelection();
    m_documentModel.clear();
    m_searchRow = -1;
    m_preview.clear();
    m_production.clear();
    m_session->hitTest()->clearSources();
    m_documentBound = false;
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
    const std::shared_ptr<pdfinteraction::PreflightRunOutcome> outcome = m_preflightOutcomes.take(snapshot.jobId);
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
    m_inspector.setCurrentRevision(documentKey, documentRevision);
    m_preview.setCurrentRevision(documentKey, documentRevision);
    m_production.setCurrentRevision(documentKey, documentRevision);

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
    if (target.kind == pdfinteraction::InteractionTargetKind::Finding)
    {
        selectFinding(target.id);
        bumpPresentation();
        return;
    }

    setInspectionMode(QStringLiteral("page"));
    if (!m_session->revisionSource() || !hasDocument())
    {
        m_inspector.clearSelection();
        bumpPresentation();
        return;
    }

    m_inspector.setSelection(pdfinteraction::buildInspectorSelection(target, inspectorContext()));
    bumpPresentation();
}

pdfinteraction::ShellInspectorContext EditorHost::inspectorContext() const
{
    pdfinteraction::ShellInspectorContext context;
    if (m_session->revisionSource())
    {
        context.documentKey = m_session->revisionSource()->documentKey();
        context.documentRevision = m_session->facade().currentRevision().toString();
    }
    context.displayTitle = displayTitle();
    context.pageCount = pageCount();
    context.documentShellStatus = documentShellStatus();
    context.preflightStateName = preflightStateName();
    context.productionStateName = productionStateName();
    context.rotationDegrees = rotationDegrees();
    context.hasOptionalContent = m_documentModel.hasOptionalContent();
    context.canvasTitle = tr("Document canvas");
    context.imageTitle = tr("Image");
    context.separationTitle = tr("Separation");
    context.pageTitlePrefix = tr("Page %1");
    context.pageBoxTitlePrefix = tr("Page box: %1");
    context.optionalContentPresent = tr("present");
    context.optionalContentNone = tr("none");
    return context;
}
