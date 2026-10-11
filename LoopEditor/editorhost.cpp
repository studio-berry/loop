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
#include "shellglossary.h"
#include "preflightclirun.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"
#include "previewstatemodel.h"
#include "productionmodel.h"
#include "interactiontarget.h"

#include "pdfdocumentsession.h"
#include "pdfdocumentimageextractor.h"
#include "pdfsecurityhandler.h"
#include "pdfdocumentwriter.h"
#include "pdfartifactstore.h"
#include "pdfcertificatestore.h"
#include "pdfform.h"
#include "pdfoperationhistorystore.h"
#include "pdfpreflightaudit.h"
#include "pdfpreflightcertificate.h"
#include "pdfpreflightverdict.h"
#include "pdfsafefilewriter.h"

#include "pdfblockingthreadguard.h"
#include "pdfcms.h"
#include "pdfdiagnostics.h"
#include "pdfpage.h"
#include "pdfrenderer.h"
#include "pdftransparencyrenderer.h"

#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QAccessibilityHints>
#include <QBuffer>
#include <QClipboard>
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
#include <limits>
#include <memory>
#include <vector>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace
{

const QString QuitCommandId = QStringLiteral("actionQuit");
const QString MoveSelectionCommandId = QStringLiteral("actionMoveSelection");
const QString TranslatePageBoxOperationId = QStringLiteral("translate-page-box");

/// The destination type each `actionCreateHyperlinkToThisPDF*` command writes
/// (issue #233). `XYZInheritZoom` is the XYZ destination with a null zoom,
/// which asks the viewer to keep its own zoom.
struct HyperlinkDestinationVariant
{
    const char* command;
    pdf::DestinationType type;
    bool inheritZoom;
};

const HyperlinkDestinationVariant HyperlinkDestinationVariants[] = {
    { "actionCreateHyperlinkToThisPDFFit", pdf::DestinationType::Fit, false },
    { "actionCreateHyperlinkToThisPDFFitB", pdf::DestinationType::FitB, false },
    { "actionCreateHyperlinkToThisPDFFitBH", pdf::DestinationType::FitBH, false },
    { "actionCreateHyperlinkToThisPDFFitBV", pdf::DestinationType::FitBV, false },
    { "actionCreateHyperlinkToThisPDFFitH", pdf::DestinationType::FitH, false },
    { "actionCreateHyperlinkToThisPDFFitR", pdf::DestinationType::FitR, false },
    { "actionCreateHyperlinkToThisPDFFitV", pdf::DestinationType::FitV, false },
    { "actionCreateHyperlinkToThisPDFXYZ", pdf::DestinationType::XYZ, false },
    { "actionCreateHyperlinkToThisPDFXYZInheritZoom", pdf::DestinationType::XYZ, true },
};

/// Reads a page-space rectangle from four scalar parameters, or nothing when
/// the surface supplied none. A partial rectangle is refused: a corner the
/// surface never gave is not a corner the host may invent.
std::optional<QRectF> rectangleFromParameters(const QVariantMap& parameters,
                                              const QString& leftKey,
                                              const QString& bottomKey,
                                              const QString& rightKey,
                                              const QString& topKey,
                                              QString& typedError)
{
    const bool any = parameters.contains(leftKey) || parameters.contains(bottomKey) ||
                     parameters.contains(rightKey) || parameters.contains(topKey);
    if (!any)
    {
        return std::nullopt;
    }

    if (!parameters.contains(leftKey) || !parameters.contains(bottomKey) ||
        !parameters.contains(rightKey) || !parameters.contains(topKey))
    {
        typedError = QStringLiteral("hyperlink/incomplete-geometry");
        return std::nullopt;
    }

    return QRectF(QPointF(parameters.value(leftKey).toDouble(), parameters.value(bottomKey).toDouble()),
                  QPointF(parameters.value(rightKey).toDouble(), parameters.value(topKey).toDouble()));
}

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

/// Rasterization resolution of the ink-coverage point readout. It samples one
/// point, not a page: the value only has to be fine enough that the pixel under
/// the point is that point, and the job is charged for the pixels it renders.
constexpr int InkCoverageReadoutDpi = 150;

/// Ceiling on the readout raster's longest edge. A huge page samples the same
/// point at a coarser resolution instead of rasterizing an unbounded image.
constexpr int InkCoverageMaxRasterPixels = 4096;

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

pdf::PDFRenderer::Features featuresForOptions(const EditorSettings::RenderOptions& options)
{
    pdf::PDFRenderer::Features features = pdf::PDFRenderer::getDefaultFeatures();
    features.setFlag(pdf::PDFRenderer::Antialiasing, options.antialiasing);
    features.setFlag(pdf::PDFRenderer::TextAntialiasing, options.textAntialiasing);
    features.setFlag(pdf::PDFRenderer::SmoothImages, options.smoothPictures);
    features.setFlag(pdf::PDFRenderer::DisplayAnnotations, options.displayAnnotations);
    features.setFlag(pdf::PDFRenderer::IgnoreOptionalContent, options.ignoreOptionalContentSettings);
    features.setFlag(pdf::PDFRenderer::DisplayTimes, options.displayRenderTimes);
    features.setFlag(pdf::PDFRenderer::DebugTextBlocks, options.showTextBlocks);
    features.setFlag(pdf::PDFRenderer::DebugTextLines, options.showTextLines);
    return features;
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
    // Menu placement is the manifest's routing policy (menu_group); the prefix
    // table is only the fallback for a descriptor that arrives without one.
    entry.insert(QStringLiteral("menuGroup"),
                 descriptor.menuGroup.isEmpty() ? shellMenuGroupForAction(descriptor.id, descriptor.target)
                                                : descriptor.menuGroup);

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
    QString effectiveProfileDigest;
    QString documentPath;
    QByteArray auditBytes;
    QJsonObject auditSummary;
};

EditorHost::EditorHost(EditorSettings* settings, QObject* parent, QString recipesDirectory) :
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
    m_actionListController(&m_session->scheduler(), this),
    m_actionListCatalog(nullptr, std::move(recipesDirectory))
{
    if (settings)
    {
        m_settings = settings;
    }
    else
    {
        m_ownedSettings = std::make_unique<EditorSettings>(nullptr, this);
        m_settings = m_ownedSettings.get();
    }

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
    connect(m_session.get(), &DocumentViewSession::passwordRequestChanged, this, &EditorHost::passwordRequestChanged);
    connectViewport();
    connectCatalog();
    connectInteraction();
    connectSurfaces();
    registerShellHandlers();
    registerFeatureHandlers();

    // One apply path for every option change, wherever it came from: the
    // command handlers, the settings surface, or a factory reset.
    connect(m_settings, &EditorSettings::optionsChanged, this, [this]
            { applyRenderOptions(); });
    connect(m_settings, &EditorSettings::noticeChanged, this, &EditorHost::bumpPresentation);

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
    connect(&m_inspector,
            &pdfinteraction::InspectorModel::correctiveOperationRequested,
            this,
            &EditorHost::onCorrectiveOperationRequested);
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
                finishRollbackJob(snapshot);
                finishPreflightJob(snapshot);
                finishActionListJob(snapshot);
                finishInkCoverageJob(snapshot);
                finishDocumentOutput(snapshot);
                finishSignatureCheckJob(snapshot);
                refreshCanvasTrace(); });
}

EditorHost::~EditorHost()
{
    m_acceptPreflightResults = false;
    m_acceptActionListResults = false;
    cancelPreflight();
    cancelActionList();
    QObject::disconnect(&m_session->scheduler(), nullptr, this, nullptr);
    if (outputBusy())
        m_session->scheduler().cancel(m_outputJobId);
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

quint64 EditorHost::passwordRequestId() const
{
    return m_session->passwordRequestId();
}

bool EditorHost::submitPassword(quint64 requestId, const QString& password)
{
    return m_session->answerPassword(requestId, password, true);
}

bool EditorHost::cancelPassword(quint64 requestId)
{
    return m_session->answerPassword(requestId, {}, false);
}

bool EditorHost::correctionsAllowed() const
{
    return m_session->facade().permissions().allowsCorrection();
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

QString EditorHost::activeTool() const
{
    const pdfinteraction::InteractionController* interaction = m_session->interaction();
    return QString::fromLatin1(pdfinteraction::getInteractionToolName(
        interaction ? interaction->activeTool() : pdfinteraction::InteractionTool::Select));
}

bool EditorHost::setActiveTool(const QString& toolId)
{
    const std::optional<pdfinteraction::InteractionTool> tool = pdfinteraction::interactionToolFromName(toolId);
    if (!tool)
    {
        return false;
    }

    // The controller is the single source of truth for the tool; it cancels an in-flight
    // drag with ToolChanged (#141 AC3) and the host only forwards the operator's choice.
    pdfinteraction::InteractionController* interaction = m_session->interaction();
    if (!interaction)
    {
        return false;
    }

    if (interaction->activeTool() == *tool)
    {
        return true;
    }

    interaction->setActiveTool(*tool);
    Q_EMIT activeToolChanged();
    return true;
}

bool EditorHost::copySelectedText()
{
    pdfinteraction::InteractionController* interaction = m_session->interaction();
    if (!interaction)
    {
        return false;
    }

    // The refusal is typed and named, and the clipboard is left untouched: an
    // empty string is never copied as success.
    const pdfinteraction::TextCopyResult result = interaction->selectedText();
    if (!result.copied)
    {
        announceDocumentState(tr("Copy refused: %1").arg(result.refusalReason));
        return false;
    }

    QGuiApplication::clipboard()->setText(result.text);
    announceDocumentState(tr("Copied %n character(s).", "", result.text.size()));
    return true;
}

QImage EditorHost::composeVisibleCanvasImage() const
{
    pdfinteraction::PageSurfaceCoordinator* surfaces = m_session->surfaces();
    if (!surfaces || !hasDocument())
    {
        return QImage();
    }

    const pdfinteraction::CanvasSnapshot& snapshot = surfaces->snapshot();
    if (snapshot.isEmpty() || !snapshot.token.revision.isValid() ||
        snapshot.token.revision != m_session->facade().currentRevision())
    {
        return QImage();
    }

    return pdfinteraction::composeCanvasImage(snapshot,
                                              m_session->viewport().viewportRect(),
                                              m_session->viewport().devicePixelRatio());
}

bool EditorHost::captureCanvasFileUrl(const QUrl& url)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
    {
        announceDocumentState(tr("Choose a local file to save the screenshot to."));
        return false;
    }

    const QImage image = composeVisibleCanvasImage();
    if (image.isNull())
    {
        announceDocumentState(tr("There is nothing on the canvas to capture."));
        return false;
    }

    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG"))
        {
            announceDocumentState(tr("The screenshot could not be encoded."));
            return false;
        }
    }

    const pdf::PDFOperationResult result =
        pdf::PDFSafeFileWriter::writeData(path, png, pdf::PDFSafeFileWriter::OverwritePolicy::Fail);
    if (!result)
    {
        announceDocumentState(tr("Could not write the screenshot: %1").arg(result.getErrorMessage()));
        return false;
    }

    announceDocumentState(tr("Screenshot saved."));
    return true;
}

bool EditorHost::extractCurrentPageImagesFileUrl(const QUrl& directory)
{
    if (!hasDocument() || !m_session->imageExtractor())
    {
        announceDocumentState(tr("Open a document before extracting images."));
        return false;
    }

    const QString directoryPath = directory.toLocalFile();
    if (directoryPath.isEmpty() || !QFileInfo(directoryPath).isDir())
    {
        announceDocumentState(tr("Choose an existing directory to extract the images into."));
        return false;
    }

    QString typedError;
    const QList<pdfinteraction::PageImage> images = m_session->imageExtractor()->extractPage(currentPage(), &typedError);
    if (!typedError.isEmpty())
    {
        announceDocumentState(tr("Could not extract images: %1").arg(typedError));
        return false;
    }
    if (images.isEmpty())
    {
        announceDocumentState(tr("This page has no images to extract."));
        return false;
    }

    // Guard every planned output up front: extraction must not silently clobber
    // an existing file, and a run must not stop halfway through the set.
    QStringList plannedOutputs;
    plannedOutputs.reserve(images.size());
    for (int index = 0; index < images.size(); ++index)
    {
        plannedOutputs << QDir(directoryPath).filePath(QStringLiteral("page-%1-image-%2.png").arg(currentPage() + 1).arg(index + 1));
    }

    if (!pdf::PDFSafeFileWriter::findOutputConflicts(plannedOutputs, true).isEmpty())
    {
        announceDocumentState(tr("Refusing to overwrite an existing file in the destination directory."));
        return false;
    }

    for (int index = 0; index < images.size(); ++index)
    {
        QByteArray png;
        {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (!images[index].image.save(&buffer, "PNG"))
            {
                announceDocumentState(tr("An extracted image could not be encoded."));
                return false;
            }
        }

        const pdf::PDFOperationResult result =
            pdf::PDFSafeFileWriter::writeData(plannedOutputs[index], png, pdf::PDFSafeFileWriter::OverwritePolicy::Fail);
        if (!result)
        {
            announceDocumentState(tr("Could not write %1: %2")
                                      .arg(QFileInfo(plannedOutputs[index]).fileName(), result.getErrorMessage()));
            return false;
        }
    }

    announceDocumentState(tr("Extracted %n image(s).", "", images.size()));
    return true;
}

bool EditorHost::importBookmarksFileUrl(const QUrl& url, const QString& revision)
{
    if (!url.isValid() || !url.isLocalFile())
    {
        return false;
    }

    return invokeCommand(QStringLiteral("actionBookmarkImport"),
                         { { QStringLiteral("path"), url.toLocalFile() },
                           { QStringLiteral("revision"), revision } }) != pdfinteraction::InvalidCommandInvocation;
}

bool EditorHost::exportBookmarksFileUrl(const QUrl& url, const QString& revision)
{
    if (!url.isValid() || !url.isLocalFile())
    {
        return false;
    }

    return invokeCommand(QStringLiteral("actionBookmarkExport"),
                         { { QStringLiteral("path"), url.toLocalFile() },
                           { QStringLiteral("revision"), revision } }) != pdfinteraction::InvalidCommandInvocation;
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

// ---------------------------------------------------------------------------
// Workspace surfaces (#586)
//
// Everything below projects state Core, PdfTool or the shell's own run already
// decided, so the QML panes render identities and wording instead of deriving
// them. The one thing the shell owns here is the operator's own review decision
// (approved/rejected) for the plan it is showing - never the publication
// approval, which stays Core's plan-bound approval in the run.
// ---------------------------------------------------------------------------

namespace
{
QVariantMap fixStateVisualMap(const pdfquick::tokens::LoopStateVisual& visual)
{
    QVariantMap result;
    result.insert(QStringLiteral("kind"), pdfquick::tokens::stateKindName(visual.kind));
    result.insert(QStringLiteral("colorRole"), pdfquick::tokens::colorRoleName(visual.colorRole));
    result.insert(QStringLiteral("icon"), pdfquick::tokens::stateIconName(visual.icon));
    result.insert(QStringLiteral("accessibleName"), visual.accessibleName);
    return result;
}

}   // namespace

QString EditorHost::fixCurrentPlanDigest() const
{
    const QString planned = m_actionListController.planDigest();
    if (!planned.isEmpty())
    {
        return planned;
    }
    // A finished run keeps its digest in the result, so a review decision can never be
    // inherited by a plan the operator did not review. A plan the controller has since
    // discarded is still named here so the surface can show what went stale.
    const QString resultDigest = m_actionListController.result().planDigest;
    return resultDigest.isEmpty() ? m_fixPlannedPlanDigest : resultDigest;
}

bool EditorHost::fixPlannedPlanIsSuperseded() const
{
    return !m_fixPlannedPlanDigest.isEmpty() && !m_fixPlannedDocumentRevision.isEmpty() &&
           hasDocument() &&
           m_fixPlannedDocumentRevision != m_session->facade().currentRevision().toString();
}

bool EditorHost::fixPlanIsCurrent() const
{
    if (!hasDocument() || !m_session->revisionSource())
    {
        return false;
    }
    return !m_fixPlannedDocumentRevision.isEmpty() &&
           m_fixPlannedDocumentRevision == m_session->facade().currentRevision().toString();
}

pdf::PDFActionListExecutionResult EditorHost::fixRunResult() const
{
    return m_actionListController.result();
}

bool EditorHost::fixExecutionArmed() const
{
    return m_session->facade().permissions().allowsCorrection() &&
           fixLifecycleStateName() == QLatin1String("approved") && fixPlanIsCurrent() &&
           (!planSignatureInvalidationApplies() || planSignatureInvalidationAcknowledged());
}

QString EditorHost::fixLifecycleStateName() const
{
    using ControllerState = pdfinteraction::ActionListController::State;
    const ControllerState state = m_actionListController.state();

    switch (state)
    {
        case ControllerState::Running:
            return QStringLiteral("executing");
        case ControllerState::Failed:
            return QStringLiteral("failed");
        case ControllerState::Cancelled:
            return QStringLiteral("cancelled");
        case ControllerState::Validating:
        case ControllerState::Planning:
            return QStringLiteral("planned");
        case ControllerState::Succeeded:
            return QStringLiteral("succeeded");
        case ControllerState::Planned:
            break;
        case ControllerState::Idle:
            // A plan the operator had for an earlier revision is stale, not absent: the
            // surface has to say which revision its evidence belongs to.
            return fixPlannedPlanIsSuperseded() ? QStringLiteral("stale") : QStringLiteral("idle");
    }

    if (!fixPlanIsCurrent())
    {
        return QStringLiteral("stale");
    }
    if (m_fixReviewedPlanDigest == fixCurrentPlanDigest())
    {
        if (m_fixReviewDecision == FixReviewDecision::Approved)
        {
            return QStringLiteral("approved");
        }
        if (m_fixReviewDecision == FixReviewDecision::Rejected)
        {
            return QStringLiteral("rejected");
        }
    }
    return QStringLiteral("preview-ready");
}

QVariantMap EditorHost::fixLifecycleVisual() const
{
    return fixStateVisualMap(
        pdfquick::tokens::resolveFixLifecycleStateVisual(fixLifecycleStateName()));
}

QColor EditorHost::fixLifecycleColor() const
{
    const pdfquick::tokens::LoopStateVisual visual =
        pdfquick::tokens::resolveFixLifecycleStateVisual(fixLifecycleStateName());
    const pdfquick::tokens::LoopTheme theme =
        highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    return pdfquick::tokens::color(visual.colorRole, theme);
}

QString EditorHost::fixLifecycleSummary() const
{
    const QString state = fixLifecycleStateName();

    if (!hasDocument())
    {
        return tr("Open a document to plan a correction.");
    }
    if (state == QLatin1String("idle"))
    {
        return tr("No correction is planned for this document.");
    }
    if (state == QLatin1String("planned"))
    {
        return m_actionListController.operatorSummary();
    }
    if (state == QLatin1String("preview-ready"))
    {
        return tr("The plan and its preview are bound to revision %1. Approve it to execute, "
                  "or reject it and plan again.")
            .arg(m_fixPlannedDocumentRevision.left(12));
    }
    if (state == QLatin1String("approved"))
    {
        if (planSignatureInvalidationApplies() && !planSignatureInvalidationAcknowledged())
        {
            return tr("The approved plan invalidates document signatures. Acknowledge the signature "
                      "invalidation before executing.");
        }
        return tr("The approved plan is bound to revision %1 and is ready to execute.")
            .arg(m_fixPlannedDocumentRevision.left(12));
    }
    if (state == QLatin1String("rejected"))
    {
        return tr("The operator rejected this plan. Plan again to continue.");
    }
    if (state == QLatin1String("stale"))
    {
        return tr("This plan belongs to a different document revision (%1). Plan again before executing.")
            .arg(m_fixPlannedDocumentRevision.left(12));
    }
    if (state == QLatin1String("executing"))
    {
        return m_actionListController.operatorSummary();
    }
    if (state == QLatin1String("failed"))
    {
        return m_actionListController.operatorSummary();
    }
    if (state == QLatin1String("cancelled"))
    {
        return tr("The run was cancelled. Nothing was published for this revision.");
    }
    return tr("The correction was rechecked and published as new revision %1.")
        .arg(m_actionListController.result().planDigest.left(12));
}

QVariantMap EditorHost::fixPlanIdentity() const
{
    const pdf::PDFActionListExecutionResult result = fixRunResult();
    QVariantMap identity;
    identity.insert(QStringLiteral("documentKey"),
                    m_session->revisionSource() ? m_session->revisionSource()->documentKey() : QString());
    identity.insert(QStringLiteral("documentRevision"),
                    hasDocument() ? m_session->facade().currentRevision().toString() : QString());
    identity.insert(QStringLiteral("plannedRevision"), m_fixPlannedDocumentRevision);
    identity.insert(QStringLiteral("planIsCurrent"), fixPlanIsCurrent());
    identity.insert(QStringLiteral("lifecycleStateName"), fixLifecycleStateName());
    identity.insert(QStringLiteral("stateVisual"), fixLifecycleVisual());
    identity.insert(QStringLiteral("sourceSha256"), result.sourceSha256);
    identity.insert(QStringLiteral("planDigest"), fixCurrentPlanDigest());
    identity.insert(QStringLiteral("recipeId"), m_actionListController.recipeId());
    identity.insert(QStringLiteral("recipeHash"), result.recipeHash);
    identity.insert(
        QStringLiteral("effectiveProfileDigest"),
        result.governed.value(QStringLiteral("effective_profile_digest")).toString(result.governed.value(QStringLiteral("sign_off")).toObject().value(QStringLiteral("effective_profile_digest")).toString()));
    identity.insert(QStringLiteral("reviewedPlanDigest"), m_fixReviewedPlanDigest);
    identity.insert(QStringLiteral("reviewDecision"),
                    m_fixReviewDecision == FixReviewDecision::Approved
                        ? QStringLiteral("approved")
                        : (m_fixReviewDecision == FixReviewDecision::Rejected ? QStringLiteral("rejected")
                                                                              : QStringLiteral("none")));
    identity.insert(QStringLiteral("executionArmed"), fixExecutionArmed());
    identity.insert(QStringLiteral("signatureInvalidationApplies"), planSignatureInvalidationApplies());
    identity.insert(QStringLiteral("signatureInvalidationAcknowledged"), planSignatureInvalidationAcknowledged());
    return identity;
}

QVariantMap EditorHost::fixPreview() const
{
    const pdf::PDFActionListExecutionResult result = fixRunResult();

    QVariantMap preview;
    preview.insert(QStringLiteral("available"), false);
    preview.insert(QStringLiteral("sourceSha256"), result.sourceSha256);
    preview.insert(QStringLiteral("planDigest"), fixCurrentPlanDigest());
    preview.insert(QStringLiteral("plannedRevision"), m_fixPlannedDocumentRevision);
    preview.insert(QStringLiteral("planIsCurrent"), fixPlanIsCurrent());
    preview.insert(QStringLiteral("pageFidelityIsExact"), pageFidelityIsExact());
    preview.insert(QStringLiteral("pageFidelityReason"), pageFidelityReason());
    preview.insert(QStringLiteral("pageFidelityIsAuthoritative"), pageFidelityIsAuthoritative());
    preview.insert(QStringLiteral("signatureInvalidation"), signatureInvalidationMap());

    QVariantList steps;
    QVariantList plannedSteps;
    QString candidateSha256;
    QString technicalStatus;
    QString visualStatus;
    bool incomplete = false;
    int changedPages = 0;
    QVariantList changedPageList;
    for (const pdf::PDFActionListStepResult& step : result.steps)
    {
        plannedSteps.append(QVariantMap{
            { QStringLiteral("stepId"), step.stepId },
            { QStringLiteral("operation"), step.operationId },
            { QStringLiteral("parameters"), step.resolvedParameters.toVariantMap() },
            { QStringLiteral("scope"), step.affectedScope.toVariantList() },
            { QStringLiteral("impact"), step.plan.value(QStringLiteral("expected_changes")).toObject().toVariantMap() },
            { QStringLiteral("risk"), step.plan.value(QStringLiteral("risk")).toString() } });
        const QJsonObject technical = step.repairResult.value(QStringLiteral("technical_preview")).toObject();
        const QJsonObject visual = step.repairResult.value(QStringLiteral("visual_preview")).toObject();
        if (technical.isEmpty() && visual.isEmpty())
        {
            continue;
        }

        QVariantMap stepPreview;
        stepPreview.insert(QStringLiteral("stepId"), step.stepId);
        stepPreview.insert(QStringLiteral("operation"), step.operationId);
        stepPreview.insert(QStringLiteral("status"), pdfActionListStepStatusName(step.status));
        stepPreview.insert(QStringLiteral("technicalStatus"), technical.value(QStringLiteral("status")).toString());
        stepPreview.insert(QStringLiteral("visualStatus"), visual.value(QStringLiteral("status")).toString());
        stepPreview.insert(QStringLiteral("candidateSha256"),
                           technical.value(QStringLiteral("candidate_sha256"))
                               .toString(visual.value(QStringLiteral("candidate_sha256")).toString()));
        stepPreview.insert(QStringLiteral("technical"), technical.toVariantMap());
        stepPreview.insert(QStringLiteral("visual"), visual.toVariantMap());
        steps.append(stepPreview);

        if (candidateSha256.isEmpty())
        {
            candidateSha256 = stepPreview.value(QStringLiteral("candidateSha256")).toString();
            technicalStatus = stepPreview.value(QStringLiteral("technicalStatus")).toString();
            visualStatus = stepPreview.value(QStringLiteral("visualStatus")).toString();
        }
        // A preview that reports anything other than a completion is incomplete evidence and
        // must never read as current approval evidence.
        const QString stepTechnical = technical.value(QStringLiteral("status")).toString();
        const QString stepVisual = visual.value(QStringLiteral("status")).toString();
        if (!stepTechnical.startsWith(QLatin1String("complete")) ||
            !stepVisual.startsWith(QLatin1String("complete")))
        {
            incomplete = true;
        }

        for (const QJsonValue& page : visual.value(QStringLiteral("pages")).toArray())
        {
            const QJsonObject pageObject = page.toObject();
            const int changed = pageObject.value(QStringLiteral("changed_pixel_count")).toInt();
            if (changed <= 0)
            {
                continue;
            }
            ++changedPages;
            QVariantMap pageEntry;
            pageEntry.insert(QStringLiteral("pageIndex"), pageObject.value(QStringLiteral("page_index")).toInt());
            pageEntry.insert(QStringLiteral("changedPixelCount"), changed);
            pageEntry.insert(QStringLiteral("unexpectedChangedPixelCount"),
                             pageObject.value(QStringLiteral("unexpected_changed_pixel_count")).toInt());
            changedPageList.append(pageEntry);
        }
    }

    preview.insert(QStringLiteral("available"), !steps.isEmpty());
    preview.insert(QStringLiteral("candidateSha256"), candidateSha256);
    preview.insert(QStringLiteral("technicalStatus"), technicalStatus);
    preview.insert(QStringLiteral("visualStatus"), visualStatus);
    preview.insert(QStringLiteral("incomplete"), incomplete);
    preview.insert(QStringLiteral("changedPageCount"), changedPages);
    preview.insert(QStringLiteral("changedPages"), changedPageList);
    preview.insert(QStringLiteral("steps"), steps);
    preview.insert(QStringLiteral("plannedSteps"), plannedSteps);
    return preview;
}

QVariantMap EditorHost::fixRecheck() const
{
    const pdf::PDFActionListExecutionResult result = fixRunResult();

    QVariantMap recheck;
    recheck.insert(QStringLiteral("available"), !result.postflight.isEmpty());
    recheck.insert(QStringLiteral("status"), m_actionListController.resultStatus());
    recheck.insert(QStringLiteral("postflight"), result.postflight.toVariantMap());
    recheck.insert(QStringLiteral("pass"), result.postflight.value(QStringLiteral("pass")).toBool());
    // The verdict Core reduced, never a completeness flag the surface could judge for itself.
    const QJsonObject postflightVerdict = result.postflight.value(QStringLiteral("verdict")).toObject();
    recheck.insert(QStringLiteral("verdictState"), postflightVerdict.value(QStringLiteral("state")).toString());
    recheck.insert(QStringLiteral("verdictSummary"),
                   postflightVerdict.value(QStringLiteral("operator_summary")).toString());

    QVariantList resolved;
    QVariantList unchanged;
    QVariantList introduced;
    QVariantList incomplete;
    bool compared = false;
    for (const pdf::PDFActionListStepResult& step : result.steps)
    {
        const QJsonObject delta = step.repairResult.value(QStringLiteral("finding_delta")).toObject();
        if (delta.isEmpty())
        {
            continue;
        }
        compared = compared || delta.value(QStringLiteral("compared")).toBool();
        for (const QJsonValue& finding : delta.value(QStringLiteral("resolved")).toArray())
        {
            resolved.append(finding.toString());
        }
        for (const QJsonValue& finding : delta.value(QStringLiteral("unchanged")).toArray())
        {
            unchanged.append(finding.toString());
        }
        for (const QJsonValue& finding : delta.value(QStringLiteral("introduced")).toArray())
        {
            introduced.append(finding.toString());
        }
        for (const QJsonValue& finding : delta.value(QStringLiteral("incomplete")).toArray())
        {
            incomplete.append(finding.toString());
        }
    }
    recheck.insert(QStringLiteral("compared"), compared);
    recheck.insert(QStringLiteral("resolved"), resolved);
    recheck.insert(QStringLiteral("unchanged"), unchanged);
    recheck.insert(QStringLiteral("introduced"), introduced);
    recheck.insert(QStringLiteral("incomplete"), incomplete);
    recheck.insert(QStringLiteral("hasIntroducedFindings"), !introduced.isEmpty());
    recheck.insert(QStringLiteral("hasIncompleteFindings"), !incomplete.isEmpty());
    return recheck;
}

QVariantMap EditorHost::fixSignOff() const
{
    const pdf::PDFActionListExecutionResult result = fixRunResult();

    QVariantMap signOff;
    signOff.insert(QStringLiteral("status"), m_actionListController.governedStatus());
    signOff.insert(QStringLiteral("governed"), result.governed.toVariantMap());
    signOff.insert(QStringLiteral("planDigest"),
                   result.governed.value(QStringLiteral("plan_digest"))
                       .toString(result.governed.value(QStringLiteral("sign_off"))
                                     .toObject()
                                     .value(QStringLiteral("plan_digest"))
                                     .toString()));
    signOff.insert(QStringLiteral("publishedSha256"),
                   result.governed.value(QStringLiteral("candidate_sha256"))
                       .toString(result.governed.value(QStringLiteral("sign_off"))
                                     .toObject()
                                     .value(QStringLiteral("published_sha256"))
                                     .toString()));
    signOff.insert(QStringLiteral("certificateStateName"), m_preflightCertificateStateName);
    signOff.insert(QStringLiteral("certificateStateVisual"), preflightCertificateStateVisual());
    signOff.insert(QStringLiteral("certificateSummary"), m_preflightCertificateSummary);
    signOff.insert(QStringLiteral("summary"), m_actionListController.governedStatus());
    return signOff;
}

bool EditorHost::planSignatureInvalidationApplies() const
{
    if (!hasDocument())
    {
        return false;
    }
    if (m_session->facade().signatureFacts().state == pdfinteraction::DocumentSignatureState::None)
    {
        return false;
    }
    // The plan's own merged save policy, the field Core serializes as
    // `invalidates_signatures` and includes in the plan digest.
    return fixRunResult().savePolicy.invalidatesSignatures;
}

bool EditorHost::planSignatureInvalidationAcknowledged() const
{
    return !m_fixSignatureInvalidationAcknowledgedDigest.isEmpty() &&
           m_fixSignatureInvalidationAcknowledgedDigest == fixCurrentPlanDigest();
}

QString EditorHost::planSignatureInvalidationSummary() const
{
    if (!planSignatureInvalidationApplies())
    {
        return QString();
    }

    const pdfinteraction::DocumentSignatureFacts facts = m_session->facade().signatureFacts();
    const QString rationale = fixRunResult().savePolicy.rationale;
    return tr("This plan invalidates %1 document signature(s): %2 The published correction will "
              "not carry a verifiable signature.")
        .arg(facts.signatureCount)
        .arg(rationale);
}

QVariantMap EditorHost::signatureInvalidationMap() const
{
    const pdfinteraction::DocumentSignatureFacts facts = m_session->facade().signatureFacts();

    QVariantMap invalidation;
    invalidation.insert(QStringLiteral("applies"), planSignatureInvalidationApplies());
    invalidation.insert(QStringLiteral("acknowledged"), planSignatureInvalidationAcknowledged());
    invalidation.insert(QStringLiteral("signatureStateName"), signatureStateName());
    invalidation.insert(QStringLiteral("signatureCount"), facts.signatureCount);
    invalidation.insert(QStringLiteral("summary"), planSignatureInvalidationSummary());
    return invalidation;
}

bool EditorHost::acknowledgePlanSignatureInvalidation(const QString& planDigest)
{
    if (m_actionListController.state() != pdfinteraction::ActionListController::State::Planned ||
        !fixPlanIsCurrent() || planDigest.isEmpty() ||
        planDigest != m_fixPlannedPlanDigest || planDigest != fixCurrentPlanDigest())
    {
        return false;
    }
    if (!planSignatureInvalidationApplies())
    {
        return false;
    }

    m_fixSignatureInvalidationAcknowledgedDigest = planDigest;
    announceDocumentState(tr("Signature invalidation acknowledged for this plan."));
    bumpPresentation();
    return true;
}

QString EditorHost::signatureStateName() const
{
    return QString::fromLatin1(
        pdfinteraction::getDocumentSignatureStateName(m_session->facade().signatureFacts().state));
}

QVariantMap EditorHost::signatureStateVisual() const
{
    return fixStateVisualMap(
        pdfquick::tokens::resolveSignatureStateVisual(signatureStateName()));
}

QColor EditorHost::signatureStateColor() const
{
    const pdfquick::tokens::LoopStateVisual visual =
        pdfquick::tokens::resolveSignatureStateVisual(signatureStateName());
    const pdfquick::tokens::LoopTheme theme =
        highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    return pdfquick::tokens::color(visual.colorRole, theme);
}

QString EditorHost::signatureSummary() const
{
    const pdfinteraction::DocumentSignatureFacts facts = m_session->facade().signatureFacts();

    switch (facts.state)
    {
        case pdfinteraction::DocumentSignatureState::None:
            return hasDocument() ? tr("No signature is present in this document.")
                                 : tr("Open a document to check its signatures.");
        case pdfinteraction::DocumentSignatureState::Valid:
            return tr("%1 signature(s) verified against the document's source bytes.")
                .arg(facts.signatureCount);
        case pdfinteraction::DocumentSignatureState::Invalid:
            return tr("At least one signature does not match the document's source bytes. The "
                      "document was changed after it was signed, or the signature is damaged.");
        case pdfinteraction::DocumentSignatureState::ModifiedAfterSigning:
            return tr("The document carries bytes added after it was signed; the existing "
                      "signature(s) do not cover the whole file.");
        case pdfinteraction::DocumentSignatureState::Unknown:
            break;
    }

    if (!facts.detail.isEmpty())
    {
        return facts.detail;
    }
    return tr("The signature(s) could not be verified with the certificates available to this "
              "machine.");
}

bool EditorHost::fixRollbackAvailable() const
{
    return m_historyError.isEmpty() && m_session->facade().permissions().allowsCorrection() && !m_fixRollbackPoints.isEmpty() &&
           m_rollbackOutcomes.isEmpty() && m_actionListOutcomes.isEmpty() &&
           m_session->facade().outputState() != pdfinteraction::DocumentOutputState::Pending &&
           (documentShellStatus() != QLatin1String("MODIFIED") ||
            m_publishedHistoryRevision == m_session->facade().currentRevision().toString());
}

QVariantList EditorHost::fixRollbackPoints() const
{
    return m_fixRollbackPoints;
}

QString EditorHost::historyDocumentPath() const
{
    return m_publishedHistoryRevision == m_session->facade().currentRevision().toString()
               ? m_publishedHistoryPath
               : m_session->facade().source().path;
}

QString EditorHost::historyArtifactDigest() const
{
    const auto document = m_session->context().getDocumentPointer();
    return document ? QString::fromLatin1(document->getSourceDataHash().toHex()) : QString();
}

QString EditorHost::revisionHistorySummary() const
{
    if (!m_historyError.isEmpty())
        return tr("Revision history is unavailable: %1").arg(m_historyError);
    return tr("%1 recorded events. Undo and Redo publish forward rollback artifacts; approvals and receipts are retained.")
        .arg(m_revisionHistory.size());
}

bool EditorHost::historyUndoAvailable() const
{
    return fixRollbackAvailable() && !m_historyUndoId.isEmpty();
}

bool EditorHost::historyRedoAvailable() const
{
    return fixRollbackAvailable() && !m_historyRedoId.isEmpty();
}

QString EditorHost::fixRollbackSummary() const
{
    if (!hasDocument())
    {
        return tr("Open a document to return to a recorded revision.");
    }
    if (documentShellStatus() == QLatin1String("MODIFIED") &&
        m_publishedHistoryRevision != m_session->facade().currentRevision().toString())
    {
        return tr("Save the document before returning to a recorded revision.");
    }
    if (m_fixRollbackPoints.isEmpty())
    {
        return tr("No recorded revisions are available for this document.");
    }
    return tr("%1 recorded revisions are available. Returning to one writes a new revision and "
              "keeps every existing history entry.")
        .arg(m_fixRollbackPoints.size());
}

QVariantMap EditorHost::previewIdentity() const
{
    const pdf::PDFActionListExecutionResult result = fixRunResult();

    QVariantMap identity;
    identity.insert(QStringLiteral("documentKey"),
                    m_session->revisionSource() ? m_session->revisionSource()->documentKey() : QString());
    identity.insert(QStringLiteral("documentRevision"),
                    hasDocument() ? m_session->facade().currentRevision().toString() : QString());
    identity.insert(QStringLiteral("previewMode"), inspectionMode());
    identity.insert(QStringLiteral("previewSummary"), previewSummary());
    identity.insert(QStringLiteral("previewDetail"),
                    m_preview.detail().isEmpty() ? QString() : m_preview.detail());
    identity.insert(QStringLiteral("profileIdentity"), m_preview.profileIdentity());
    identity.insert(QStringLiteral("planDigest"), fixCurrentPlanDigest());
    identity.insert(QStringLiteral("sourceSha256"), result.sourceSha256);
    identity.insert(QStringLiteral("candidateSha256"),
                    fixPreview().value(QStringLiteral("candidateSha256")));
    identity.insert(QStringLiteral("stale"), !previewStaleReason().isEmpty());
    identity.insert(QStringLiteral("staleReason"), previewStaleReason());
    identity.insert(QStringLiteral("productionStateName"), productionStateName());
    return identity;
}

QString EditorHost::previewStaleReason() const
{
    if (!hasDocument())
    {
        return QString();
    }
    if (!m_preview.isCurrent(m_session->revisionSource() ? m_session->revisionSource()->documentKey() : QString(),
                             m_session->facade().currentRevision().toString()))
    {
        return tr("The production preview was produced for an earlier revision and is not current.");
    }
    if (!fixCurrentPlanDigest().isEmpty() && !fixPlanIsCurrent())
    {
        return tr("The preview belongs to a plan for an earlier document revision.");
    }
    return QString();
}

QString EditorHost::previewFidelityStateName() const
{
    const bool stale = !previewStaleReason().isEmpty();
    if (hasDocument() && !stale && !m_session->surfaces()->diagnosticsForPage(currentPage()).has_value())
    {
        return QStringLiteral("unavailable");
    }
    // A display simulation outranks the render underneath it: the operator is
    // looking at a transformed page, so the indicator may not report the render
    // path's own fidelity as if that were what is on screen (issue #235 display
    // half, issue #237).
    return pdfquick::tokens::classifyPreviewFidelityState(hasDocument(),
                                                          stale,
                                                          pageFidelityIsAuthoritative(),
                                                          pageFidelityIsExact(),
                                                          displaySimulationName());
}

QString EditorHost::displaySimulationName() const
{
    if (softProofActive())
    {
        return QStringLiteral("soft-proof");
    }
    if (m_displayColor.isActive())
    {
        return QStringLiteral("display-filter");
    }
    return QString();
}

QVariantMap EditorHost::previewFidelityVisual() const
{
    const pdfquick::tokens::LoopStateVisual visual =
        pdfquick::tokens::resolvePreviewFidelityStateVisual(previewFidelityStateName());

    QVariantMap result;
    result.insert(QStringLiteral("kind"), pdfquick::tokens::stateKindName(visual.kind));
    result.insert(QStringLiteral("colorRole"), pdfquick::tokens::colorRoleName(visual.colorRole));
    result.insert(QStringLiteral("icon"), pdfquick::tokens::stateIconName(visual.icon));
    result.insert(QStringLiteral("accessibleName"), visual.accessibleName);
    return result;
}

QColor EditorHost::previewFidelityColor() const
{
    const pdfquick::tokens::LoopStateVisual visual =
        pdfquick::tokens::resolvePreviewFidelityStateVisual(previewFidelityStateName());
    const pdfquick::tokens::LoopTheme theme =
        highContrast() ? pdfquick::tokens::LoopTheme::HighContrast : pdfquick::tokens::LoopTheme::Dark;
    return pdfquick::tokens::color(visual.colorRole, theme);
}

QString EditorHost::previewFidelityOriginName() const
{
    return pdfquick::tokens::previewFidelityOriginName(previewFidelityStateName());
}

bool EditorHost::previewRequiresAuthoritative() const
{
    return previewFidelityStateName() == QLatin1String("approximate");
}

QString EditorHost::previewFidelitySummary() const
{
    const QString state = previewFidelityStateName();

    if (state == QLatin1String("unavailable"))
    {
        if (hasDocument())
        {
            return tr("Render fidelity is unavailable until the current page's requested render completes. "
                      "No rendered evidence is available for a print claim.");
        }
        return tr("No document is open, so there is no preview to describe.");
    }
    if (state == QLatin1String("stale"))
    {
        return tr("Not current evidence: %1").arg(previewStaleReason());
    }
    if (state == QLatin1String("authoritative") && !pageFidelityIsExact())
    {
        return tr("Fidelity authoritative but still approximate, origin output-preview: this page is "
                  "rendered with the overprint-accurate compositor, yet its diagnostics report an "
                  "approximation (%1). Do not read it as an exact render.")
            .arg(pageFidelityReason());
    }
    if (state == QLatin1String("authoritative"))
    {
        return tr("Fidelity authoritative, origin output-preview: this page is rendered with the "
                  "overprint-accurate compositor. Read it with the plates and separations; it does "
                  "not by itself certify publication safety.");
    }
    if (state == QLatin1String("approximate"))
    {
        return tr("Fidelity approximate, origin fast-canvas: overprint is not simulated (%1). These "
                  "canvas pixels cannot stand as proof of print-safe output; switch this page to the "
                  "authoritative overprint render before any print claim.")
            .arg(pageFidelityReason());
    }
    if (state == QLatin1String("display-filter"))
    {
        return tr("Display colour simulation (%1): a display colour mode is filtering the canvas, so "
                  "these pixels are a presentation of the page and not a production render. They are "
                  "never recorded as approval evidence.")
            .arg(displayColorModeName());
    }
    if (state == QLatin1String("soft-proof"))
    {
        return tr("Soft-proof simulation of the output intent through colour management (profile %1). "
                  "It is a simulation of print, not the print's evidence, and it is never recorded as "
                  "approval.")
            .arg(m_softProofProfile.name.isEmpty() ? m_softProofProfile.id : m_softProofProfile.name);
    }
    return tr("Fidelity exact, origin fast-canvas: this page reports no overprint-sensitive content. "
              "The interactive preview still does not certify print-safe output.");
}

QVariantMap EditorHost::compareReview() const
{
    const QVariantMap identity = fixPlanIdentity();
    const QVariantMap preview = fixPreview();
    const QVariantMap recheck = fixRecheck();
    const QVariantMap signOff = fixSignOff();
    const pdf::PDFActionListExecutionResult result = fixRunResult();

    const QString planDigest = identity.value(QStringLiteral("planDigest")).toString();
    const QString reviewedPlanDigest = identity.value(QStringLiteral("reviewedPlanDigest")).toString();
    const QString publishedPlanDigest = signOff.value(QStringLiteral("planDigest")).toString();
    const bool planIsCurrent = identity.value(QStringLiteral("planIsCurrent")).toBool();
    // The shell's own lifecycle already decided whether the plan and its preview still
    // describe the open revision; the comparison does not second-guess it.
    const QString lifecycle = fixLifecycleStateName();
    const bool comparisonStale = lifecycle == QLatin1String("stale");
    QString staleReason;
    if (comparisonStale)
    {
        staleReason = previewStaleReason();
        if (staleReason.isEmpty())
        {
            staleReason = tr("The comparison belongs to a different document revision. Plan the "
                             "correction again; it is not refreshed.");
        }
    }

    // Before/after artifact identities: the exact input the plan bound to, and the
    // candidate Core serialized plus the bytes a governed run published.
    QVariantMap before;
    before.insert(QStringLiteral("documentKey"), identity.value(QStringLiteral("documentKey")));
    before.insert(QStringLiteral("documentRevision"), identity.value(QStringLiteral("documentRevision")));
    before.insert(QStringLiteral("plannedRevision"), identity.value(QStringLiteral("plannedRevision")));
    before.insert(QStringLiteral("sourceSha256"), identity.value(QStringLiteral("sourceSha256")));

    QVariantMap after;
    after.insert(QStringLiteral("candidateSha256"), preview.value(QStringLiteral("candidateSha256")));
    after.insert(QStringLiteral("publishedSha256"), signOff.value(QStringLiteral("publishedSha256")));
    after.insert(QStringLiteral("reviewDecision"), identity.value(QStringLiteral("reviewDecision")));
    after.insert(QStringLiteral("publicationStatus"), signOff.value(QStringLiteral("status")));

    QVariantMap plan;
    plan.insert(QStringLiteral("planDigest"), planDigest);
    plan.insert(QStringLiteral("reviewedPlanDigest"), reviewedPlanDigest);
    plan.insert(QStringLiteral("publishedPlanDigest"), publishedPlanDigest);
    plan.insert(QStringLiteral("planIsCurrent"), planIsCurrent);
    plan.insert(QStringLiteral("recipeId"), identity.value(QStringLiteral("recipeId")));
    plan.insert(QStringLiteral("recipeHash"), identity.value(QStringLiteral("recipeHash")));
    plan.insert(QStringLiteral("effectiveProfileDigest"),
                identity.value(QStringLiteral("effectiveProfileDigest")));

    // Core's technical finding delta, carried through unchanged.
    QVariantMap findingDelta;
    findingDelta.insert(QStringLiteral("compared"), recheck.value(QStringLiteral("compared")));
    findingDelta.insert(QStringLiteral("resolved"), recheck.value(QStringLiteral("resolved")));
    findingDelta.insert(QStringLiteral("unchanged"), recheck.value(QStringLiteral("unchanged")));
    findingDelta.insert(QStringLiteral("introduced"), recheck.value(QStringLiteral("introduced")));
    findingDelta.insert(QStringLiteral("incomplete"), recheck.value(QStringLiteral("incomplete")));

    // The declared change surface, the facts the comparison preserved and the risk it
    // left unresolved, all read off Core's own step plans and finding deltas.
    QVariantList declaredChangeAttributes;
    QVariantList carriedForwardFindings;
    QVariantList unresolvedFindings;
    QVariantList unresolvedMessages;
    QVariantList materialDeltas;
    QVariantList steps;
    QString highestRisk;
    // Core names risk low < medium < high < destructive; a name outside that set ranks above all of them.
    const QStringList riskOrder{ QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"),
                                 QStringLiteral("destructive") };
    const auto riskRank = [&riskOrder](const QString& risk)
    {
        const qsizetype position = riskOrder.indexOf(risk);
        return position < 0 ? riskOrder.size() : position;
    };
    QString savePolicyMode;
    QString savePolicyRationale;
    for (int index = 0; index < result.steps.size(); ++index)
    {
        const pdf::PDFActionListStepResult& step = result.steps.at(index);
        const QJsonObject stepPlan = step.plan;
        const QJsonObject expected = stepPlan.value(QStringLiteral("expected_changes")).toObject();
        for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        {
            if (it.value().toBool() && !declaredChangeAttributes.contains(it.key()))
            {
                declaredChangeAttributes.append(it.key());
            }
        }
        const QString stepRisk = stepPlan.value(QStringLiteral("risk")).toString();
        if (highestRisk.isEmpty() || riskRank(stepRisk) > riskRank(highestRisk))
        {
            highestRisk = stepRisk;
        }
        if (savePolicyMode.isEmpty())
        {
            const QJsonObject savePolicy = stepPlan.value(QStringLiteral("save_policy")).toObject();
            savePolicyMode = savePolicy.value(QStringLiteral("mode")).toString();
            savePolicyRationale = savePolicy.value(QStringLiteral("rationale")).toString();
        }
        for (const QJsonValue& reason : stepPlan.value(QStringLiteral("unsupported_reasons")).toArray())
        {
            unresolvedMessages.append(reason.toString());
        }
        for (const QJsonValue& warning : stepPlan.value(QStringLiteral("warnings")).toArray())
        {
            unresolvedMessages.append(warning.toString());
        }

        QVariantMap stepEntry;
        stepEntry.insert(QStringLiteral("index"), index);
        stepEntry.insert(QStringLiteral("stepId"), step.stepId);
        stepEntry.insert(QStringLiteral("operation"), step.operationId);
        stepEntry.insert(QStringLiteral("status"), pdfActionListStepStatusName(step.status));
        steps.append(stepEntry);

        const QJsonObject stepDelta = step.repairResult.value(QStringLiteral("finding_delta")).toObject();
        for (const QJsonValue& finding : stepDelta.value(QStringLiteral("carried_forward")).toArray())
        {
            carriedForwardFindings.append(finding.toString());
        }
        for (const QJsonValue& finding : stepDelta.value(QStringLiteral("incomplete")).toArray())
        {
            unresolvedFindings.append(finding.toString());
        }
        for (const QJsonValue& reason : step.repairResult.value(QStringLiteral("incomplete_reasons")).toArray())
        {
            unresolvedMessages.append(reason.toString());
        }
        for (const QJsonValue& warning : step.repairResult.value(QStringLiteral("warnings")).toArray())
        {
            unresolvedMessages.append(warning.toString());
        }

        // Material deltas the operator navigates: what the step cleared, introduced or
        // could not finish rechecking. The owning step index is kept for the route.
        for (const QJsonValue& finding : stepDelta.value(QStringLiteral("resolved")).toArray())
        {
            materialDeltas.append(QVariantMap{ { QStringLiteral("kind"), QStringLiteral("resolved") },
                                               { QStringLiteral("findingId"), finding.toString() },
                                               { QStringLiteral("stepIndex"), index } });
        }
        for (const QJsonValue& finding : stepDelta.value(QStringLiteral("introduced")).toArray())
        {
            materialDeltas.append(QVariantMap{ { QStringLiteral("kind"), QStringLiteral("introduced") },
                                               { QStringLiteral("findingId"), finding.toString() },
                                               { QStringLiteral("stepIndex"), index } });
        }
        for (const QJsonValue& finding : stepDelta.value(QStringLiteral("incomplete")).toArray())
        {
            materialDeltas.append(QVariantMap{ { QStringLiteral("kind"), QStringLiteral("incomplete") },
                                               { QStringLiteral("findingId"), finding.toString() },
                                               { QStringLiteral("stepIndex"), index } });
        }
    }

    QVariantMap preserved;
    preserved.insert(QStringLiteral("declaredChangeAttributes"), declaredChangeAttributes);
    preserved.insert(QStringLiteral("unchangedFindings"), recheck.value(QStringLiteral("unchanged")));
    preserved.insert(QStringLiteral("carriedForwardFindings"), carriedForwardFindings);
    preserved.insert(QStringLiteral("savePolicyMode"), savePolicyMode);
    preserved.insert(QStringLiteral("savePolicyRationale"), savePolicyRationale);

    QVariantMap risk;
    risk.insert(QStringLiteral("level"), highestRisk);
    risk.insert(QStringLiteral("introducedFindings"), recheck.value(QStringLiteral("introduced")));
    risk.insert(QStringLiteral("incompleteFindings"), unresolvedFindings);
    risk.insert(QStringLiteral("messages"), unresolvedMessages);
    risk.insert(QStringLiteral("hasIntroduced"), recheck.value(QStringLiteral("hasIntroducedFindings")));
    risk.insert(QStringLiteral("hasIncomplete"), recheck.value(QStringLiteral("hasIncompleteFindings")));

    QVariantMap review;
    review.insert(QStringLiteral("available"),
                  hasDocument() && (!planDigest.isEmpty() || !result.steps.isEmpty()));
    review.insert(QStringLiteral("before"), before);
    review.insert(QStringLiteral("after"), after);
    review.insert(QStringLiteral("plan"), plan);
    review.insert(QStringLiteral("findingDelta"), findingDelta);
    review.insert(QStringLiteral("preserved"), preserved);
    review.insert(QStringLiteral("unresolvedRisk"), risk);
    review.insert(QStringLiteral("materialDeltas"), materialDeltas);
    review.insert(QStringLiteral("hasMaterialDeltas"), !materialDeltas.isEmpty());
    review.insert(QStringLiteral("steps"), steps);
    review.insert(QStringLiteral("stale"), comparisonStale);
    review.insert(QStringLiteral("staleReason"), staleReason);

    // Fail closed: a comparison that cannot describe the open revision, or whose reviewed
    // or published digest is not the plan on screen, is blocked rather than refreshed.
    QString blockedReason;
    if (review.value(QStringLiteral("available")).toBool())
    {
        if (comparisonStale)
        {
            blockedReason = staleReason;
        }
        else if (!reviewedPlanDigest.isEmpty() && reviewedPlanDigest != planDigest)
        {
            blockedReason = tr("The reviewed plan digest does not match the plan on screen. It is not "
                               "refreshed; review the current plan again.");
        }
        else if (!publishedPlanDigest.isEmpty() && !planDigest.isEmpty() &&
                 publishedPlanDigest != planDigest)
        {
            blockedReason = tr("The published artifact belongs to a different plan. It is not refreshed.");
        }
    }
    review.insert(QStringLiteral("lifecycleStateName"), lifecycle);
    review.insert(QStringLiteral("blocked"), !blockedReason.isEmpty());
    review.insert(QStringLiteral("blockedReason"), blockedReason);
    return review;
}

bool EditorHost::navigateCompareDelta(int deltaIndex)
{
    const QVariantMap review = compareReview();
    if (review.value(QStringLiteral("blocked")).toBool())
    {
        return false;
    }

    const QVariantList deltas = review.value(QStringLiteral("materialDeltas")).toList();
    if (deltaIndex < 0 || deltaIndex >= deltas.size())
    {
        return false;
    }

    const QVariantMap delta = deltas.at(deltaIndex).toMap();
    const int stepIndex = delta.value(QStringLiteral("stepIndex")).toInt();
    const QString findingId = delta.value(QStringLiteral("findingId")).toString();
    if (stepIndex < 0)
    {
        return false;
    }

    if (!inspectActionListStep(stepIndex))
    {
        return false;
    }

    setWorkspace(LoopWorkspace::Inspect);
    if (!findingId.isEmpty())
    {
        selectFinding(findingId);
    }
    return true;
}

bool EditorHost::navigateToAnnotation(int row)
{
    if (!hasDocument() || !m_documentBound)
    {
        return false;
    }

    QuickAnnotationModel& annotations = m_documentModel.annotationModel();
    if (row < 0 || row >= annotations.rows().size())
    {
        return false;
    }

    const QuickAnnotation& annotation = annotations.rows().at(row);
    annotations.setSelectedRow(row);
    refreshAnnotationOverlay();

    const bool revealed = m_session->viewport().revealPageRegion(annotation.pageIndex,
                                                                 annotation.bounds, 0.2);
    updateCanvasAccessibilitySummary();
    bumpPresentation();
    return revealed;
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

QString EditorHost::settingsNotice()
{
    return m_settings->notice();
}

QVariantMap EditorHost::diagnosticsBundle() const
{
    QVariantMap bundle;
    bundle.insert(QStringLiteral("status"), m_diagnosticsBundle.status);
    bundle.insert(QStringLiteral("directory"), m_diagnosticsBundle.directory);
    bundle.insert(QStringLiteral("error"), m_diagnosticsBundle.error);
    bundle.insert(QStringLiteral("fileCount"), m_diagnosticsBundle.fileCount);
    return bundle;
}

bool EditorHost::requestSettingsReset()
{
    m_settingsResetPending = true;
    Q_EMIT settingsResetRequested();
    bumpPresentation();
    return true;
}

bool EditorHost::confirmSettingsReset()
{
    if (!m_settingsResetPending)
    {
        return false;
    }
    m_settingsResetPending = false;

    // resetToFactory() reports its own failure through the store notice; the
    // typed state is only replaced when every owned key was removed.
    const bool reset = m_settings->resetToFactory();
    bumpPresentation();
    return reset;
}

void EditorHost::cancelSettingsReset()
{
    if (!m_settingsResetPending)
    {
        return;
    }
    m_settingsResetPending = false;
    bumpPresentation();
}

bool EditorHost::collectDiagnosticsFileUrl(const QUrl& parentDirectory)
{
    if (!parentDirectory.isValid() || !parentDirectory.isLocalFile() || parentDirectory.toLocalFile().isEmpty())
    {
        m_diagnosticsBundle.status = QStringLiteral("failed");
        m_diagnosticsBundle.directory.clear();
        m_diagnosticsBundle.fileCount = 0;
        m_diagnosticsBundle.error = tr("Choose a local folder for the diagnostics bundle.");
        bumpPresentation();
        return false;
    }

    pdf::PDFDiagnosticsOptions options;
    options.applicationId = QStringLiteral("editor");
    options.outputDirectory = parentDirectory.toLocalFile();
    const pdf::PDFDiagnosticsResult result = pdf::PDFDiagnosticsCollector::collect(options);

    if (!result.success)
    {
        m_diagnosticsBundle.status = QStringLiteral("failed");
        m_diagnosticsBundle.directory.clear();
        m_diagnosticsBundle.fileCount = 0;
        m_diagnosticsBundle.error = result.errorMessage.isEmpty()
                                        ? tr("The diagnostics bundle could not be written.")
                                        : result.errorMessage;
        bumpPresentation();
        return false;
    }

    m_diagnosticsBundle.status = QStringLiteral("ready");
    m_diagnosticsBundle.error.clear();
    m_diagnosticsBundle.directory = result.bundleDirectory;
    m_diagnosticsBundle.fileCount = static_cast<int>(result.files.size());
    bumpPresentation();
    return true;
}

void EditorHost::applyRenderOptions()
{
    m_session->setSurfaceRenderFeatures(featuresForOptions(m_settings->renderOptions()));
    if (m_documentBound)
    {
        m_session->surfaces()->requestSurfaces();
    }
    bumpPresentation();
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

QString EditorHost::localizedTerm(const QString& code) const
{
    return pdfinteraction::glossary::term(code);
}

QString EditorHost::preflightStateName() const
{
    return preflightStateToString(m_preflight.state());
}

QString EditorHost::preflightRunUnavailableReason() const
{
    if (!hasDocument() || !m_session->revisionSource())
    {
        return tr("Open a document before running preflight.");
    }
    if (m_preflight.state() == pdfinteraction::PreflightController::State::Running)
    {
        return m_preflight.runUnavailableReason();
    }
    const auto profile = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                      [this](const PreflightProfileChoice& choice)
                                      { return choice.id == m_selectedPreflightProfileId; });
    return profile == m_preflightProfiles.cend() || !profile->valid
               ? tr("Select a validated preflight profile before running preflight.")
               : QString();
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

    if (m_settings->isOptionEnabled(EditorSettings::IgnoreOptionalContentOptionId))
    {
        // The optional-content override deliberately deviates from the
        // document's own view state; no render under it is an exact render.
        return false;
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

    QStringList reasons;
    if (m_settings->isOptionEnabled(EditorSettings::IgnoreOptionalContentOptionId))
    {
        reasons.append(tr("The optional content override is enabled: this render shows all optional "
                          "content regardless of the document's settings."));
    }

    const std::optional<pdf::PDFRenderDiagnostics> diagnostics = m_session->surfaces()->diagnosticsForPage(currentPage());
    if (diagnostics.has_value() && !diagnostics->reasons.isEmpty())
    {
        reasons.append(diagnostics->reasons);
    }

    return reasons.join(QStringLiteral(" "));
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

bool EditorHost::ensureAuthoritativePreview()
{
    if (!hasDocument() || !previewRequiresAuthoritative())
    {
        return false;
    }

    m_session->surfaces()->setPageAuthoritativeOverprint(currentPage(), true);
    bumpPresentation();
    return true;
}

QString EditorHost::displayColorModeName() const
{
    return pdfquick::getCanvasDisplayColorModeName(m_displayColor.mode);
}

bool EditorHost::displayColorModeActive() const
{
    return m_displayColor.isActive();
}

QColor EditorHost::displayColorCustomForeground() const
{
    return m_displayColor.customForeground;
}

QColor EditorHost::displayColorCustomBackground() const
{
    return m_displayColor.customBackground;
}

int EditorHost::displayColorBitonalThreshold() const
{
    return m_displayColor.bitonalThreshold;
}

bool EditorHost::setDisplayColorMode(const QString& modeName)
{
    const std::optional<pdfquick::CanvasDisplayColorMode> mode =
        pdfquick::canvasDisplayColorModeFromName(modeName);
    if (!mode)
    {
        return false;
    }

    m_displayColor.mode = *mode;
    applyDisplayColorToCanvas();
    bumpPresentation();
    return true;
}

bool EditorHost::setDisplayColorCustomColors(const QColor& foreground, const QColor& background)
{
    if (!foreground.isValid() || !background.isValid())
    {
        return false;
    }

    m_displayColor.customForeground = foreground;
    m_displayColor.customBackground = background;
    applyDisplayColorToCanvas();
    bumpPresentation();
    return true;
}

bool EditorHost::setDisplayColorBitonalThreshold(int threshold)
{
    if (threshold < 1 || threshold > 254)
    {
        return false;
    }

    m_displayColor.bitonalThreshold = threshold;
    applyDisplayColorToCanvas();
    bumpPresentation();
    return true;
}

void EditorHost::applyDisplayColorToCanvas()
{
    if (m_canvas)
    {
        m_canvas->setDisplayColorMode(pdfquick::getCanvasDisplayColorModeName(m_displayColor.mode));
        m_canvas->setDisplayColorCustomForeground(m_displayColor.customForeground);
        m_canvas->setDisplayColorCustomBackground(m_displayColor.customBackground);
        m_canvas->setDisplayColorBitonalThreshold(m_displayColor.bitonalThreshold);
    }
}

bool EditorHost::softProofActive() const
{
    return m_softProofEnabled && !m_softProofProfile.id.isEmpty();
}

QString EditorHost::softProofStateName() const
{
    return m_softProofStateName;
}

QString EditorHost::softProofProfileName() const
{
    return m_softProofProfile.name;
}

QString EditorHost::softProofProfileId() const
{
    return m_softProofProfile.id;
}

QString EditorHost::softProofProfileTypeName() const
{
    return m_softProofProfile.typeName;
}

QString EditorHost::softProofSummary() const
{
    if (!hasDocument())
    {
        return tr("No document is open, so there is no output intent to simulate.");
    }
    if (softProofActive())
    {
        return tr("Soft proof of the output intent through colour management. The profile is %1. "
                  "This is a simulation of print, not the print's evidence: it is never recorded "
                  "as approval.")
            .arg(m_softProofProfile.name.isEmpty() ? m_softProofProfile.id : m_softProofProfile.name);
    }
    if (m_softProofStateName == QLatin1String("profile-missing"))
    {
        return tr("Soft proof is unavailable: this document declares no output-intent profile. "
                  "Nothing is simulated and the canvas keeps the unproofed render.");
    }
    if (m_softProofStateName == QLatin1String("profile-invalid"))
    {
        return tr("Soft proof is unavailable: the document's output-intent profile could not be "
                  "read. Nothing is simulated and the canvas keeps the unproofed render.");
    }
    return tr("Soft proof is off. The canvas shows the unproofed render.");
}

std::optional<EditorHost::SoftProofProfile> EditorHost::resolveOutputIntentProfile(bool* documentDeclaresOutputIntent) const
{
    if (documentDeclaresOutputIntent)
    {
        *documentDeclaresOutputIntent = false;
    }

    if (!hasDocument())
    {
        return std::nullopt;
    }

    pdf::PDFDocumentSession* session = m_session->context().tryGetSession();
    const pdf::PDFDocument* document = m_session->context().getDocument();
    if (!session || !document)
    {
        return std::nullopt;
    }

    if (documentDeclaresOutputIntent)
    {
        *documentDeclaresOutputIntent = !document->getCatalog()->getOutputIntents().empty();
    }

    // Core resolves the output intent: PDFCMSManager decodes the profile stream
    // under the session's operation budget and refuses one it cannot open, so an
    // unreadable profile is absent here rather than guessed at. A budget failure
    // is the same kind of answer -- the profile could not be read -- and is
    // reported as one instead of escaping a presentation call.
    try
    {
        pdf::PDFCMSManager cmsManager(nullptr);
        cmsManager.setDocument(document, session->getProcessingBudget());

        for (const pdf::PDFColorProfileIdentifier& identifier : cmsManager.getCMYKProfiles())
        {
            if (!identifier.isOutputIntentProfile)
            {
                continue;
            }

            SoftProofProfile profile;
            profile.id = identifier.id;
            profile.name = identifier.name;
            profile.typeName = QStringLiteral("output-intent");
            return profile;
        }
    }
    catch (const pdf::PDFException&)
    {
        return std::nullopt;
    }
    return std::nullopt;
}

bool EditorHost::setSoftProofEnabled(bool enabled)
{
    if (!enabled)
    {
        const bool wasActive = softProofActive();
        m_softProofEnabled = false;
        m_softProofStateName = QStringLiteral("off");
        m_softProofProfile = SoftProofProfile();
        if (m_session)
        {
            m_session->setSoftProofProfile(QString());
        }
        if (wasActive)
        {
            bumpPresentation();
        }
        return wasActive;
    }

    if (!hasDocument())
    {
        m_softProofEnabled = false;
        m_softProofStateName = QStringLiteral("profile-missing");
        bumpPresentation();
        return false;
    }

    bool declaresOutputIntent = false;
    const std::optional<SoftProofProfile> profile = resolveOutputIntentProfile(&declaresOutputIntent);
    if (!profile)
    {
        m_softProofEnabled = false;
        m_softProofStateName = declaresOutputIntent ? QStringLiteral("profile-invalid")
                                                    : QStringLiteral("profile-missing");
        bumpPresentation();
        return false;
    }

    m_softProofProfile = *profile;
    m_softProofEnabled = true;
    m_softProofStateName = QStringLiteral("simulating");
    m_session->setSoftProofProfile(m_softProofProfile.id);
    bumpPresentation();
    return true;
}

bool EditorHost::toggleSoftProof()
{
    if (softProofActive())
    {
        setSoftProofEnabled(false);
        return false;
    }
    return setSoftProofEnabled(true);
}

QVariantMap EditorHost::plateRow(const QString& id) const
{
    for (const QVariant& entry : m_plates)
    {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("id")).toString() == id)
        {
            return row;
        }
    }
    return QVariantMap();
}

QVariantList EditorHost::productionPlates() const
{
    return m_plates;
}

QString EditorHost::plateIsolationSummary() const
{
    if (m_plates.isEmpty())
    {
        return tr("No separations are known for this document.");
    }
    if (!m_isolatedPlateId.isEmpty())
    {
        const QVariantMap isolated = plateRow(m_isolatedPlateId);
        return tr("Isolated plate: %1. The other plates are hidden.")
            .arg(isolated.value(QStringLiteral("displayName")).toString());
    }

    QStringList hidden;
    for (const QVariant& entry : m_plates)
    {
        const QVariantMap row = entry.toMap();
        if (!row.value(QStringLiteral("visible")).toBool())
        {
            hidden.append(row.value(QStringLiteral("displayName")).toString());
        }
    }
    if (hidden.isEmpty())
    {
        return tr("Every plate is shown.");
    }
    return tr("Hidden plates: %1.").arg(hidden.join(QStringLiteral(", ")));
}

bool EditorHost::setPlateVisible(const QString& plateId, bool visible)
{
    if (plateRow(plateId).isEmpty())
    {
        return false;
    }

    m_plateVisible.insert(plateId, visible);
    if (!visible && m_isolatedPlateId == plateId)
    {
        m_isolatedPlateId.clear();
    }
    reloadProductionPlates();
    bumpPresentation();
    return true;
}

bool EditorHost::isolatePlate(const QString& plateId)
{
    if (plateRow(plateId).isEmpty())
    {
        return false;
    }

    m_isolatedPlateId = plateId;
    for (const QVariant& entry : m_plates)
    {
        const QString id = entry.toMap().value(QStringLiteral("id")).toString();
        m_plateVisible.insert(id, id == plateId);
    }
    reloadProductionPlates();
    bumpPresentation();
    return true;
}

bool EditorHost::showAllPlates()
{
    m_isolatedPlateId.clear();
    for (const QVariant& entry : m_plates)
    {
        m_plateVisible.insert(entry.toMap().value(QStringLiteral("id")).toString(), true);
    }
    reloadProductionPlates();
    bumpPresentation();
    return true;
}

void EditorHost::reloadProductionPlates()
{
    m_plates.clear();

    if (!hasDocument())
    {
        return;
    }

    pdf::PDFDocumentSession* session = m_session->context().tryGetSession();
    const pdf::PDFDocument* document = m_session->context().getDocument();
    if (!session || !document)
    {
        return;
    }

    // The press separations Core resolves for this document: the four process
    // plates the preview simulates plus the document's own spot inks. This is
    // the same PDFInkMapper recipe PdfTool's ink tools and the authoritative
    // renderer use, so the list names the inks the render path actually knows.
    try
    {
        pdf::PDFCMSManager cmsManager(nullptr);
        cmsManager.setDocument(document, session->getProcessingBudget());
        pdf::PDFInkMapper inkMapper(&cmsManager, document);
        inkMapper.createSpotColors(true);

        for (const pdf::PDFInkMapper::ColorInfo& info : inkMapper.getSeparations(4, true))
        {
            const QString id = QString::fromLatin1(info.name);
            QVariantMap row;
            row.insert(QStringLiteral("id"), id);
            row.insert(QStringLiteral("displayName"), info.textName.isEmpty() ? id : info.textName);
            row.insert(QStringLiteral("kind"), info.isSpot ? QStringLiteral("spot") : QStringLiteral("process"));
            row.insert(QStringLiteral("spotColorName"), info.isSpot ? id : QString());
            row.insert(QStringLiteral("color"), info.color.name());
            row.insert(QStringLiteral("visible"), m_plateVisible.value(id, true));
            row.insert(QStringLiteral("isolated"), m_isolatedPlateId == id);
            m_plates.append(row);
        }
    }
    catch (const pdf::PDFException&)
    {
        // A document whose inks could not be enumerated says so through the
        // empty list and its summary rather than presenting an invented one.
        m_plates.clear();
    }
}

bool EditorHost::requestInkCoverageAtPoint(qreal pageX, qreal pageY)
{
    if (!hasDocument() || !m_session->revisionSource())
    {
        return false;
    }

    const int pageIndex = currentPage();
    const pdf::PDFDocumentPointer document = m_session->context().getDocumentPointer();
    if (!document || pageIndex < 0)
    {
        return false;
    }

    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pdf::PDFJobSpec spec;
    spec.jobId = jobId;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = m_session->revisionSource()->documentKey();
    spec.documentRevision = m_session->facade().currentRevision().toString();
    spec.operationId = QStringLiteral("ink-coverage.point");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    auto outcome = std::make_shared<InkCoverageOutcome>();
    outcome->pageIndex = pageIndex;
    m_inkCoverageOutcomes.insert(jobId, outcome);

    m_session->scheduler().submit(spec, [document, outcome, pageIndex, pageX, pageY](pdf::PDFJobContext& context)
                                  {
        if (context.isCancellationRequested())
        {
            return;
        }

        const pdf::PDFPage* page = document->getCatalog()->getPage(static_cast<size_t>(pageIndex));
        if (!page)
        {
            outcome->reason = QStringLiteral("ink-coverage/no-such-page");
            return;
        }

        // The readout is rendered through Core's separation-accurate path, so the
        // coverage it reports is the coverage of the separated inks rather than
        // of the display raster.
        pdf::PDFCMSManager cmsManager(nullptr);
        cmsManager.setDocument(document.data(), nullptr);
        pdf::PDFInkMapper inkMapper(&cmsManager, document.data());
        inkMapper.createSpotColors(true);

        pdf::PDFFontCache fontCache(pdf::DEFAULT_FONT_CACHE_LIMIT, pdf::DEFAULT_REALIZED_FONT_CACHE_LIMIT);
        pdf::PDFOptionalContentActivity optionalContent(document.data(), pdf::OCUsage::Print, nullptr);
        pdf::PDFModifiedDocument modifiedDocument(document.data(), &optionalContent);
        fontCache.setDocument(modifiedDocument);
        fontCache.setCacheShrinkEnabled(nullptr, false);

        pdf::PDFTransparencyRendererSettings rendererSettings;
        rendererSettings.flags.setFlag(pdf::PDFTransparencyRendererSettings::SeparationSimulation, true);
        rendererSettings.renderPolicy = pdf::PDFRenderPolicy::forSeparationPreview();

        // The readout raster is chosen for the point query, not for display: it
        // only has to be fine enough that one pixel is a point sample, and the
        // job is charged for every pixel it rasterizes.
        const QRectF cropBox = page->getCropBox();
        if (!cropBox.isValid() || cropBox.width() <= 0.0 || cropBox.height() <= 0.0)
        {
            outcome->reason = QStringLiteral("ink-coverage/page-has-no-box");
            return;
        }

        const qreal scale = static_cast<qreal>(InkCoverageReadoutDpi) / 72.0;
        QSize pixelSize(qRound(cropBox.width() * scale), qRound(cropBox.height() * scale));
        pixelSize = pixelSize.boundedTo(QSize(InkCoverageMaxRasterPixels, InkCoverageMaxRasterPixels));
        if (pixelSize.isEmpty())
        {
            outcome->reason = QStringLiteral("ink-coverage/raster-too-small");
            return;
        }

        context.processingBudget().chargeRenderPixels(static_cast<std::uint64_t>(pixelSize.width()) * static_cast<std::uint64_t>(pixelSize.height()),
                                                      QStringLiteral("ink coverage readout"));

        const QRectF deviceRect(QPointF(0.0, 0.0), QSizeF(pixelSize));
        const QTransform pagePointToDevice = pdf::PDFRenderer::createPagePointToDevicePointMatrix(page, deviceRect, pdf::PageRotation::None);
        const QPointF devicePoint = pagePointToDevice.map(QPointF(pageX, pageY));
        const int pixelX = qFloor(devicePoint.x());
        const int pixelY = qFloor(devicePoint.y());
        if (pixelX < 0 || pixelY < 0 || pixelX >= pixelSize.width() || pixelY >= pixelSize.height())
        {
            outcome->reason = QStringLiteral("ink-coverage/point-outside-page");
            return;
        }

        pdf::PDFTransparencyRenderer renderer(page,
                                              document.data(),
                                              &fontCache,
                                              cmsManager.getCurrentCMS().data(),
                                              &optionalContent,
                                              &inkMapper,
                                              rendererSettings,
                                              pagePointToDevice);
        renderer.setOperationControl(context.operationControl());
        renderer.beginPaint(pixelSize);
        renderer.processContents();
        const pdf::PDFFloatBitmap& separated = renderer.endPaint();

        if (context.isCancellationRequested())
        {
            return;
        }

        const pdf::PDFConstColorBuffer channels = separated.getPixel(static_cast<size_t>(pixelX), static_cast<size_t>(pixelY));
        const pdf::PDFPixelFormat format = separated.getPixelFormat();

        QVariantList channelRows;
        const std::vector<pdf::PDFInkMapper::ColorInfo> separations = inkMapper.getSeparations(4, true);
        const int channelCount = format.getColorChannelCount();
        for (int channel = 0; channel < channelCount; ++channel)
        {
            QVariantMap row;
            const size_t separationIndex = static_cast<size_t>(channel);
            if (separationIndex < separations.size())
            {
                row.insert(QStringLiteral("name"), QString::fromLatin1(separations[separationIndex].name));
                row.insert(QStringLiteral("displayName"), separations[separationIndex].textName);
                row.insert(QStringLiteral("isSpot"), separations[separationIndex].isSpot);
            }
            else
            {
                row.insert(QStringLiteral("name"), QStringLiteral("channel-%1").arg(channel));
                row.insert(QStringLiteral("displayName"), QStringLiteral("Channel %1").arg(channel));
                row.insert(QStringLiteral("isSpot"), false);
            }
            row.insert(QStringLiteral("coverage"), double(channels[static_cast<uint8_t>(channel)]));
            channelRows.append(row);
        }

        outcome->ok = true;
        outcome->readout.insert(QStringLiteral("available"), true);
        outcome->readout.insert(QStringLiteral("pageIndex"), pageIndex);
        outcome->readout.insert(QStringLiteral("pageX"), pageX);
        outcome->readout.insert(QStringLiteral("pageY"), pageY);
        outcome->readout.insert(QStringLiteral("totalInkCoverage"), double(separated.getPixelInkCoverage(static_cast<size_t>(pixelX), static_cast<size_t>(pixelY))));
        outcome->readout.insert(QStringLiteral("channelCount"), channelCount);
        outcome->readout.insert(QStringLiteral("channels"), channelRows);
        outcome->readout.insert(QStringLiteral("rasterSize"), pixelSize);
        outcome->readout.insert(QStringLiteral("reason"), QString()); });

    return true;
}

QVariantMap EditorHost::inkCoverageReadout() const
{
    return m_inkCoverageReadout;
}

qreal EditorHost::currentPageCentreX() const
{
    return currentPageCentre().x();
}

qreal EditorHost::currentPageCentreY() const
{
    return currentPageCentre().y();
}

QPointF EditorHost::currentPageCentre() const
{
    if (!hasDocument())
    {
        return QPointF();
    }

    const pdf::PDFDocument* document = m_session->context().getDocument();
    const int pageIndex = currentPage();
    if (!document || pageIndex < 0 || pageIndex >= static_cast<int>(document->getCatalog()->getPageCount()))
    {
        return QPointF();
    }

    const pdf::PDFPage* page = document->getCatalog()->getPage(static_cast<size_t>(pageIndex));
    if (!page)
    {
        return QPointF();
    }

    const QRectF cropBox = page->getCropBox();
    return cropBox.isValid() ? cropBox.center() : QPointF();
}

void EditorHost::finishInkCoverageJob(const pdf::PDFJobSnapshot& snapshot)
{
    const auto outcome = m_inkCoverageOutcomes.take(snapshot.jobId);
    if (!outcome)
    {
        return;
    }

    if (!outcome->ok)
    {
        // A readout that could not be produced is a visible state, never a zero:
        // "0% ink" and "not measured" are different facts.
        QVariantMap readout;
        readout.insert(QStringLiteral("available"), false);
        readout.insert(QStringLiteral("pageIndex"), outcome->pageIndex);
        readout.insert(QStringLiteral("reason"), outcome->reason.isEmpty()
                                                     ? QStringLiteral("ink-coverage/not-measured")
                                                     : outcome->reason);
        readout.insert(QStringLiteral("channels"), QVariantList());
        m_inkCoverageReadout = readout;
    }
    else
    {
        m_inkCoverageReadout = outcome->readout;
    }
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
    if (!preflightRunUnavailableReason().isEmpty())
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
            outcome->effectiveProfileDigest = profile.effectiveDigest;
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
            outcome->documentPath = documentPath;
            outcome->auditBytes = std::move(auditBytes);
            outcome->auditSummary = pdf::preflightAuditReportSummary(outcome->result, documentPath);
            pdf::PreflightInspectionReceipt receipt;
            QString receiptError;
            if (!pdf::buildPreflightInspectionReceipt(outcome->result, profile, session->getRevision(),
                                                      engine.lastEvidenceGraph(), receipt, receiptError))
            {
                throw std::runtime_error(receiptError.toStdString());
            }
            const auto* security = document->getStorage().getSecurityHandler();
            receipt.limitations.append(security && security->getMode() != pdf::EncryptionMode::None
                                           ? tr("Document is encrypted; authenticated document permissions apply.")
                                           : tr("Document is not encrypted."));
            outcome->auditSummary.insert(QStringLiteral("inspection_receipt"), receipt.toJson());

            if (context.isCancellationRequested())
                return;

            context.reportProgress(95);
            context.setResultSummary(tr("Preflight completed."));
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
        item.insert(QStringLiteral("builtIn"), recipe.builtIn);
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
    if (!m_rollbackOutcomes.isEmpty() || !m_session->facade().permissions().allowsCorrection())
    {
        announceDocumentState(tr("A rollback is in progress or document permissions restrict corrections."));
        return false;
    }
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
                                                preflightProfileBindings,
                                                historyDocumentPath(),
                                                phase == pdfinteraction::ActionListRunPhase::Execute &&
                                                    planSignatureInvalidationAcknowledged()));
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
    if (!fixExecutionArmed())
    {
        if (planSignatureInvalidationApplies() && !planSignatureInvalidationAcknowledged())
        {
            announceDocumentState(tr("The plan invalidates document signatures. Acknowledge the "
                                     "signature invalidation before executing."));
        }
        return false;
    }
    return submitActionListJob(pdfinteraction::ActionListRunPhase::Execute,
                               pdfinteraction::ActionListController::State::Running);
}

bool EditorHost::cancelActionList()
{
    return m_actionListController.cancelRun(m_actionListController.jobId());
}

void EditorHost::clearFixReview()
{
    m_fixReviewDecision = FixReviewDecision::None;
    m_fixReviewedPlanDigest.clear();
    m_fixSignatureInvalidationAcknowledgedDigest.clear();
}

bool EditorHost::approveActionListPlan()
{
    return approveActionListPlan(fixCurrentPlanDigest(), fixRunResult().sourceSha256,
                                 m_fixPlannedDocumentRevision);
}

bool EditorHost::approveActionListPlan(const QString& planDigest,
                                       const QString& sourceSha256,
                                       const QString& documentRevision)
{
    if (!correctionsAllowed())
    {
        return false;
    }
    if (m_actionListController.state() != pdfinteraction::ActionListController::State::Planned ||
        !fixPlanIsCurrent() || planDigest.isEmpty() || sourceSha256.isEmpty() ||
        planDigest != m_fixPlannedPlanDigest || planDigest != fixCurrentPlanDigest() ||
        sourceSha256 != fixRunResult().sourceSha256 || documentRevision != m_fixPlannedDocumentRevision)
    {
        return false;
    }

    m_fixReviewDecision = FixReviewDecision::Approved;
    m_fixReviewedPlanDigest = fixCurrentPlanDigest();
    announceDocumentState(tr("Correction plan approved for revision %1.")
                              .arg(m_fixPlannedDocumentRevision.left(12)));
    bumpPresentation();
    return true;
}

bool EditorHost::rejectActionListPlan()
{
    if (m_actionListController.state() != pdfinteraction::ActionListController::State::Planned)
    {
        return false;
    }

    m_fixReviewDecision = FixReviewDecision::Rejected;
    m_fixReviewedPlanDigest = fixCurrentPlanDigest();
    announceDocumentState(tr("Correction plan rejected. Plan again to continue."));
    bumpPresentation();
    return true;
}

bool EditorHost::executeApprovedActionListPlan()
{
    return runActionList();
}

bool EditorHost::selectActionListRecipeForOperation(const QString& operationId)
{
    if (operationId.isEmpty())
    {
        return false;
    }

    // Precedence: a valid recipe the operator imported wins. The shipped recipe is a fallback
    // only. An operator recipe that offers the operation but is invalid stops the change with
    // its diagnostic rather than silently running the shipped one.
    const pdfinteraction::ActionListRecipeEntry* operatorRecipe = nullptr;
    const pdfinteraction::ActionListRecipeEntry* invalidOperatorRecipe = nullptr;
    const pdfinteraction::ActionListRecipeEntry* shippedRecipe = nullptr;
    for (const pdfinteraction::ActionListRecipeEntry& recipe : m_actionListCatalog.recipes())
    {
        const bool offersOperation = std::any_of(
            recipe.actionList.steps.cbegin(),
            recipe.actionList.steps.cend(),
            [&operationId](const pdf::PDFActionListStep& step)
            { return step.operationId == operationId; });
        if (!offersOperation)
        {
            continue;
        }
        if (recipe.builtIn)
        {
            shippedRecipe = &recipe;
        }
        else if (recipe.valid && !operatorRecipe)
        {
            operatorRecipe = &recipe;
        }
        else if (!recipe.valid && !invalidOperatorRecipe)
        {
            invalidOperatorRecipe = &recipe;
        }
    }

    const pdfinteraction::ActionListRecipeEntry* selected = operatorRecipe;
    if (!selected)
    {
        if (invalidOperatorRecipe)
        {
            announceDocumentState(tr("Recipe '%1' offers %2 but is invalid: %3")
                                      .arg(invalidOperatorRecipe->name, operationId, invalidOperatorRecipe->diagnostic));
            return false;
        }
        if (shippedRecipe && !shippedRecipe->valid)
        {
            announceDocumentState(tr("The shipped recipe for %1 is unavailable: %2")
                                      .arg(operationId, shippedRecipe->diagnostic));
            return false;
        }
        selected = shippedRecipe;
    }
    if (!selected)
    {
        announceDocumentState(tr("No recipe runs %1 yet. Import a recipe that offers it, then plan the change.")
                                  .arg(operationId));
        return false;
    }

    m_selectedActionListRecipeId = selected->id;
    syncActionListDraft();
    m_actionListController.markRecipeStale();
    Q_EMIT actionListRecipesChanged();
    bumpPresentation();
    announceDocumentState(
        tr("Recipe '%1' runs %2. Validate and plan it in the Fix workspace.").arg(selected->name, operationId));
    return true;
}

void EditorHost::onCorrectiveOperationRequested(const pdfinteraction::InspectorCorrectiveOperationIntent& intent)
{
    setWorkspace(LoopWorkspace::Fix);

    if (intent.operationId.isEmpty())
    {
        return;
    }

    // The finding's parameters become the recipe's bindings; everything else - validate, plan,
    // review, approve - stays with the operator in the Fix workspace.
    m_actionListBindings = intent.parameters;
    selectActionListRecipeForOperation(intent.operationId);
}

void EditorHost::replanActionList()
{
    m_actionListController.discardPlan();
    clearFixReview();
    m_fixPlannedDocumentRevision.clear();
    m_fixPlannedPlanDigest.clear();
    bumpPresentation();
}

bool EditorHost::inspectActionListStep(int stepIndex)
{
    const pdf::PDFActionListExecutionResult result = fixRunResult();
    if (stepIndex < 0 || stepIndex >= result.steps.size())
    {
        return false;
    }

    const pdf::PDFActionListStepResult& step = result.steps.at(stepIndex);
    const QJsonObject plan = step.plan;
    const QJsonObject savePolicy = plan.value(QStringLiteral("save_policy")).toObject();

    QStringList targets;
    for (const QJsonValue& target : plan.value(QStringLiteral("targets")).toArray())
    {
        targets.append(target.toObject().value(QStringLiteral("path")).toString());
    }
    QStringList domains;
    for (const QJsonValue& domain : plan.value(QStringLiteral("domains")).toArray())
    {
        domains.append(domain.toString());
    }
    QStringList unsupported;
    for (const QJsonValue& reason : plan.value(QStringLiteral("unsupported_reasons")).toArray())
    {
        unsupported.append(reason.toString());
    }

    const QJsonObject output = step.repairResult.value(QStringLiteral("output")).toObject();
    const QJsonObject delta = step.repairResult.value(QStringLiteral("finding_delta")).toObject();

    pdfinteraction::InspectorModel::Selection selection;
    selection.documentKey = m_session->revisionSource() ? m_session->revisionSource()->documentKey() : QString();
    selection.documentRevision =
        hasDocument() ? m_session->facade().currentRevision().toString() : QString();
    selection.selectionId = QStringLiteral("operation:%1").arg(step.stepId);
    selection.title = tr("Correction step %1").arg(step.stepId);
    selection.kind = pdfinteraction::InspectorModel::SelectionKind::Operation;
    selection.properties = {
        { QStringLiteral("operation"), tr("Operation"), step.operationId },
        { QStringLiteral("operation-version"),
          tr("Operation version"),
          QString::number(plan.value(QStringLiteral("version")).toInt(1)) },
        { QStringLiteral("step"), tr("Step"), step.stepId },
        { QStringLiteral("status"), tr("Step status"), pdfActionListStepStatusName(step.status) },
        { QStringLiteral("target"), tr("Target"), targets.isEmpty() ? tr("whole document") : targets.join(QStringLiteral(", ")) },
        { QStringLiteral("parameters"),
          tr("Parameters"),
          QString::fromUtf8(QJsonDocument(step.resolvedParameters).toJson(QJsonDocument::Compact)) },
        { QStringLiteral("impact"), tr("Impact class"),
          domains.isEmpty() ? tr("unclassified") : domains.join(QStringLiteral(", ")) },
        { QStringLiteral("risk"), tr("Risk"), plan.value(QStringLiteral("risk")).toString(tr("unknown")) },
        { QStringLiteral("save-policy"), tr("Save policy"),
          savePolicy.value(QStringLiteral("mode")).toString(tr("unknown")) },
        { QStringLiteral("save-policy-rationale"),
          tr("Save policy rationale"),
          savePolicy.value(QStringLiteral("rationale")).toString() },
        { QStringLiteral("requires-postflight"),
          tr("Requires postflight"),
          plan.value(QStringLiteral("requires_postflight")).toBool() ? tr("yes") : tr("no") },
        { QStringLiteral("unresolved-risk"), tr("Unresolved risk"),
          unsupported.isEmpty() ? plan.value(QStringLiteral("warnings")).toArray().isEmpty() ? tr("none reported") : tr("warnings reported")
                                : unsupported.join(QStringLiteral(", ")) },
        { QStringLiteral("approval"), tr("Approval state"),
          tr("%1 (plan %2)").arg(m_actionListController.governedStatus(), m_fixReviewedPlanDigest.isEmpty() ? tr("not reviewed") : (m_fixReviewDecision == FixReviewDecision::Approved ? tr("approved by the operator") : tr("rejected by the operator"))) },
        { QStringLiteral("output"), tr("Output identity"),
          output.value(QStringLiteral("sha256")).toString(tr("not published")) },
        { QStringLiteral("cleared"), tr("Findings cleared"),
          QString::number(delta.value(QStringLiteral("resolved")).toArray().size()) },
        { QStringLiteral("introduced"), tr("Findings introduced"),
          QString::number(delta.value(QStringLiteral("introduced")).toArray().size()) },
        { QStringLiteral("recipe"), tr("Recipe"), result.actionListId },
        { QStringLiteral("recipe-hash"), tr("Recipe hash"), result.recipeHash },
        { QStringLiteral("plan-digest"), tr("Plan digest"), fixCurrentPlanDigest() },
        { QStringLiteral("source-sha256"), tr("Source digest"), result.sourceSha256 },
        { QStringLiteral("plan-is-current"), tr("Plan matches this revision"),
          fixPlanIsCurrent() ? tr("yes") : tr("no") },
    };

    const bool current = m_inspector.setSelection(selection);
    setInspectionMode(QStringLiteral("operation"));
    bumpPresentation();
    return current;
}

void EditorHost::refreshFixRollbackPoints()
{
    refreshFixRollbackPointsFromHistory();
    bumpPresentation();
}

void EditorHost::refreshFixRollbackPointsFromHistory()
{
    m_fixRollbackPoints.clear();
    m_revisionHistory.clear();
    m_historyUndoId.clear();
    m_historyRedoId.clear();
    m_historyError.clear();
    if (!hasDocument())
        return;
    const QString databasePath = QDir(historyDocumentPath() + QStringLiteral(".loop-history")).filePath(QStringLiteral("history.sqlite3"));
    if (!QFileInfo::exists(databasePath))
        return;
    pdf::PDFOperationHistoryStore history(databasePath);
    if (!history.open(&m_historyError))
        return;
    const auto verification = history.verify();
    if (!verification.verified)
    {
        m_historyError = verification.errorMessage;
        return;
    }
    const auto points = history.rollbackPoints(&m_historyError);
    const auto events = history.events(&m_historyError);
    QHash<QString, QString> parents;
    QStringList redo;
    const QString currentDigest = historyArtifactDigest();
    for (const auto& event : events)
    {
        QVariantMap row = event.toJson().toVariantMap();
        row.insert(QStringLiteral("current"), event.output && event.output->sha256 == currentDigest);
        row.insert(QStringLiteral("receiptText"), QString::fromUtf8(QJsonDocument(event.resultSummary.value(QStringLiteral("governed")).toObject()).toJson(QJsonDocument::Compact)));
        m_revisionHistory.append(row);
        if (!event.output)
            continue;
        const auto execution = history.execution(event.executionId, &m_historyError);
        if (!execution)
            continue;
        const QString source = execution->input.sha256;
        const QString target = event.output->sha256;
        if (event.status == pdf::PDFOperationHistoryStatus::Accepted && source != target)
        {
            parents.insert(target, source);
            redo.clear();
        }
        else if (event.status == pdf::PDFOperationHistoryStatus::RolledBack)
        {
            if (parents.value(source) == target)
                redo.append(source);
            else if (!redo.isEmpty() && redo.last() == target)
                redo.removeLast();
            else
                redo.clear();
        }
    }
    for (const auto& point : points)
    {
        const auto accepted = std::find_if(events.cbegin(), events.cend(), [&point](const auto& event)
                                           { return event.entryId == point.auditEventId && event.status == pdf::PDFOperationHistoryStatus::Accepted; });
        if (point.artifactEvicted || (!point.isOriginalInput && accepted == events.cend()))
            continue;
        QVariantMap entry = point.toJson().toVariantMap();
        entry.insert(QStringLiteral("canReturn"), !point.isOriginalInput || point.documentRevisionDigest == parents.value(currentDigest));
        m_fixRollbackPoints.append(entry);
        if (point.documentRevisionDigest == parents.value(currentDigest))
            m_historyUndoId = point.rollbackId;
        if (!redo.isEmpty() && point.documentRevisionDigest == redo.last())
            m_historyRedoId = point.rollbackId;
    }
    refreshFeatureAvailability();
}

bool EditorHost::requestFixRollback(const QString& rollbackId)
{
    if (!fixRollbackAvailable() || rollbackId.isEmpty())
    {
        return false;
    }

    const QString documentPath = historyDocumentPath();
    if (documentPath.isEmpty())
    {
        return false;
    }

    const QString historyDirectory = QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history");
    pdf::PDFOperationHistoryStore history(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString error;
    if (!history.open(&error))
    {
        announceDocumentState(tr("Recorded revisions are unavailable: %1").arg(error));
        return false;
    }

    const QList<pdf::PDFRollbackPoint> points = history.rollbackPoints(&error);
    const auto point = std::find_if(points.cbegin(), points.cend(),
                                    [&rollbackId](const pdf::PDFRollbackPoint& candidate)
                                    { return candidate.rollbackId == rollbackId; });
    if (point == points.cend())
    {
        announceDocumentState(tr("That recorded revision is no longer available."));
        return false;
    }

    // The request names the revision the document is currently on, taken from the bytes the
    // shell has open, so a rollback can never be applied to a document it does not describe.
    QFile currentFile(documentPath);
    if (!currentFile.open(QIODevice::ReadOnly))
    {
        announceDocumentState(tr("The open document could not be read for the rollback request."));
        return false;
    }
    const QByteArray currentBytes = currentFile.readAll();
    currentFile.close();
    const QString currentDigest =
        QString::fromLatin1(QCryptographicHash::hash(currentBytes, QCryptographicHash::Sha256).toHex());

    if (currentDigest != historyArtifactDigest())
    {
        announceDocumentState(tr("The source artifact changed; reopen it before navigating history."));
        return false;
    }
    QUuid targetExecutionId;
    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&error);
    const auto event = std::find_if(events.cbegin(), events.cend(),
                                    [&point, &history, &currentDigest](const pdf::PDFOperationHistoryEvent& candidate)
                                    {
                                        if (!point->isOriginalInput)
                                            return candidate.entryId == point->auditEventId;
                                        const auto execution = history.execution(candidate.executionId);
                                        return execution && candidate.status == pdf::PDFOperationHistoryStatus::Accepted &&
                                               candidate.output && candidate.output->sha256 == currentDigest &&
                                               execution->input.sha256 == point->documentRevisionDigest;
                                    });
    if (event == events.cend())
    {
        announceDocumentState(tr("The recorded revision has no audit event and cannot be returned to."));
        return false;
    }
    targetExecutionId = event->executionId;
    history.close();

    // P2: a rollback revalidates the restored revision through the #38 path, so the
    // Editor must supply the effective profile it already holds. Without a validated
    // profile the rollback is refused and recorded, never fabricated.
    const auto profileIt = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                        [this](const PreflightProfileChoice& choice)
                                        { return choice.id == m_selectedPreflightProfileId; });
    if (profileIt == m_preflightProfiles.cend() || !profileIt->valid)
    {
        announceDocumentState(tr("Select a validated preflight profile before returning to a recorded revision."));
        return false;
    }

    pdf::PDFRollbackRequest request;
    request.currentArtifactSha256 = currentDigest;
    request.targetArtifactSha256 = point->documentRevisionDigest;
    request.targetExecutionId = targetExecutionId;
    request.reason = tr("The operator returned the document to a recorded revision.");
    request.approval.kind = pdf::PDFApprovalKind::Human;
    request.approval.actorId = QStringLiteral("Editor");
    request.approval.decision = QStringLiteral("approve");
    request.approval.policyId = QStringLiteral("desktop-rollback");
    request.approval.rationale = request.reason;
    request.approval.evidenceSha256 = point->documentRevisionDigest;
    request.approval.decisionReference =
        QStringLiteral("editor-rollback:%1").arg(point->documentRevisionDigest);
    request.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    request.profile = profileIt->profile;
    request.signOffActor = QStringLiteral("Editor");
    request.signOffPolicy = QStringLiteral("desktop-rollback-postflight");

    // A new sibling file, never the open document: returning to a revision must not
    // overwrite what the operator has on screen.
    const QFileInfo documentInfo(documentPath);
    const QString destination =
        documentInfo.absoluteDir().filePath(QStringLiteral("%1-rollback-%2.pdf")
                                                .arg(documentInfo.completeBaseName().left(32),
                                                     QUuid::createUuid().toString(QUuid::WithoutBraces)));

    // One rollback at a time: a second request while the first is still restoring would
    // race the same history and publish two revisions from one document state.
    if (!m_rollbackOutcomes.isEmpty())
    {
        announceDocumentState(tr("A rollback is already in progress."));
        return false;
    }

    // The restore revalidates the recovered bytes through PreflightEngine, which refuses
    // to run on this interactive thread (registered at construction), so the work is
    // scheduled exactly like the Action List run. The worker builds its own stores: Qt SQL
    // connections are thread-affine, so the GUI-thread history opened above must never be
    // touched off-thread.
    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    pdf::PDFJobSpec spec;
    spec.jobId = jobId;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = m_session->revisionSource()->documentKey();
    spec.documentRevision = m_session->facade().currentRevision().toString();
    spec.operationId = QStringLiteral("rollback.%1").arg(point->documentRevisionDigest.left(8));
    spec.checkId = QStringLiteral("rollback");
    spec.progressModel = QStringLiteral("rollback-progress-v1");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    auto outcome = std::make_shared<RollbackJobOutcome>();
    outcome->destinationPath = destination;
    outcome->revision = point->documentRevisionDigest.left(12);
    m_rollbackOutcomes.insert(jobId, outcome);

    const QString submittedId = m_session->scheduler().submit(
        spec,
        [request, historyDirectory, destination, outcome](pdf::PDFJobContext& context)
        {
            if (context.isCancellationRequested())
            {
                return;
            }
            QString openError;
            const QString sourcePath = historyDirectory.left(historyDirectory.size() - QStringLiteral(".loop-history").size());
            if (!pdfinteraction::retainHistoryForArtifact(sourcePath, destination, &openError))
                throw std::runtime_error(openError.toStdString());
            const QString outputHistoryDirectory = destination + QStringLiteral(".loop-history");
            pdf::PDFOperationHistoryStore workerHistory(QDir(outputHistoryDirectory).filePath(QStringLiteral("history.sqlite3")));
            if (!workerHistory.open(&openError))
            {
                throw std::runtime_error(
                    tr("Recorded revisions are unavailable: %1").arg(openError).toStdString());
            }
            pdf::PDFArtifactStore artifacts(outputHistoryDirectory);
            const pdf::PDFOperationResult rollback = workerHistory.rollbackTo(request, artifacts, destination);
            if (!rollback)
            {
                outcome->errorMessage = rollback.getErrorMessage();
                return;
            }
            outcome->ok = true;
            if (context.isCancellationRequested())
            {
                // The restored revision is already published; skipping retention keeps it
                // rather than hiding it behind a cancelled job.
                return;
            }
            const pdf::PDFHistoryRetentionResult retention = workerHistory.enforceRetention({}, artifacts);
            outcome->retentionSuccess = retention.success;
            outcome->retentionError = retention.errorMessage;
        });
    if (submittedId != jobId)
    {
        m_rollbackOutcomes.remove(jobId);
        announceDocumentState(tr("Unable to schedule the rollback."));
        return false;
    }

    refreshFeatureAvailability();
    bumpPresentation();
    return true;
}

void EditorHost::discardActionListPlan()
{
    m_actionListController.discardPlan();
    clearFixReview();
    m_fixPlannedDocumentRevision.clear();
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
        // The shipped recipe is never the default: it is selected only when an operation needs it.
        const QList<pdfinteraction::ActionListRecipeEntry>& recipes = m_actionListCatalog.recipes();
        const auto valid = std::find_if(recipes.cbegin(), recipes.cend(),
                                        [](const pdfinteraction::ActionListRecipeEntry& recipe)
                                        { return recipe.valid && !recipe.builtIn; });
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
        if (!recipe.builtIn && QFileInfo::exists(recipe.source))
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
            choice.diagnostic = tr("Profile JSON is invalid.");
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
        descriptors.append(descriptorToVariant(descriptor, isCommandEnabled(descriptor.id)));
    }
    return descriptors;
}

bool EditorHost::isCommandEnabled(const QString& commandId) const
{
    return commandPermissionAllowed(commandId) && m_session->catalog().isEnabled(commandId);
}

bool EditorHost::commandPermissionAllowed(const QString& commandId) const
{
    const auto permissions = m_session->facade().permissions();
    if (commandId == QLatin1String("actionPrint"))
    {
        return permissions.print;
    }
    if (commandId == QLatin1String("actionCopyText"))
    {
        return permissions.copy;
    }
    if (commandId == MoveSelectionCommandId || commandId == QLatin1String("actionPageGeometry") ||
        commandId == QLatin1String("actionInsertPageNumbers") || commandId == QLatin1String("actionStickyNoteInsert"))
    {
        return permissions.allowsCorrection();
    }
    return true;
}

quint64 EditorHost::invokeCommand(const QString& commandId, const QVariantMap& parameters)
{
    if (!commandPermissionAllowed(commandId))
    {
        return pdfinteraction::InvalidCommandInvocation;
    }
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

QString EditorHost::outputRevision() const
{
    return m_session->facade().currentRevision().toString();
}

bool EditorHost::exportDocumentFileUrl(const QUrl& url, const QString& format, int dpi,
                                       const QString& revision, bool fidelityAcknowledged)
{
    if (!url.isValid() || !url.isLocalFile())
        return false;
    return invokeCommand(format == QLatin1String("print-pdf") ? QStringLiteral("actionPrint") : QStringLiteral("actionExportDocument"),
                         { { QStringLiteral("path"), url.toLocalFile() }, { QStringLiteral("format"), format }, { QStringLiteral("dpi"), dpi }, { QStringLiteral("revision"), revision }, { QStringLiteral("fidelityAcknowledged"), fidelityAcknowledged } }) != pdfinteraction::InvalidCommandInvocation;
}

void EditorHost::beginDocumentOutput(pdfinteraction::CommandInvocationId invocation, const QVariantMap& parameters)
{
    const auto refuse = [this, invocation](const QString& reason)
    {
        m_outputRecord = { { QStringLiteral("error"), reason }, { QStringLiteral("printProof"), false } };
        m_session->catalog().finishInvocation(invocation, pdfinteraction::CommandTerminalState::Failed, reason);
        announceDocumentState(reason);
        bumpPresentation();
        Q_EMIT documentOutputFinished();
    };
    if (!hasDocument() || outputBusy() || m_session->facade().operation().pending ||
        parameters.value(QStringLiteral("revision")).toString() != outputRevision())
    {
        refuse(QStringLiteral("output/stale-or-busy-document"));
        return;
    }
    loopeditor::OutputRequest request;
    const QString format = parameters.value(QStringLiteral("format")).toString();
    if (format == QLatin1String("pdf"))
        request.format = loopeditor::OutputFormat::PublishedPdf;
    else if (format == QLatin1String("print-pdf"))
        request.format = loopeditor::OutputFormat::PrintPdf;
    else if (format == QLatin1String("png"))
        request.format = loopeditor::OutputFormat::Png;
    else if (format == QLatin1String("tiff"))
        request.format = loopeditor::OutputFormat::Tiff;
    else
    {
        refuse(QStringLiteral("output/invalid-format"));
        return;
    }
    const bool printing = request.format == loopeditor::OutputFormat::PrintPdf;
    request.dpi = parameters.value(QStringLiteral("dpi"), 300).toInt();
    if ((printing && (!m_documentModel.canPrint() || (!m_documentModel.canHighResolutionPrint() && request.dpi > 150))) ||
        (!printing && !m_documentModel.canCopy()))
    {
        refuse(QStringLiteral("output/permission-restricted"));
        return;
    }
    request.fidelityAcknowledged = parameters.value(QStringLiteral("fidelityAcknowledged")).toBool();
    if (!pageFidelityIsExact() && !request.fidelityAcknowledged)
    {
        refuse(QStringLiteral("output/fidelity-acknowledgment-required"));
        return;
    }
    request.revision = outputRevision();
    request.document = m_session->context().getDocumentPointer();
    request.sourcePath = m_session->facade().source().path;
    request.page = currentPage();
    if (m_publicationRevision == request.revision)
    {
        request.publishedBytes = m_publishedOutputBytes;
        request.publicationReceipt = m_publicationReceipt;
    }
    if (request.format == loopeditor::OutputFormat::PublishedPdf && request.publishedBytes.isEmpty() &&
        m_session->facade().facets().testFlag(pdfinteraction::DocumentFacet::Dirty))
    {
        refuse(QStringLiteral("output/unpublished-revision"));
        return;
    }
    const QString destination = parameters.value(QStringLiteral("path")).toString();
    const QFileInfo target(destination), source(request.sourcePath);
    if (destination.isEmpty() || target.isDir() ||
        target.absoluteFilePath() == source.absoluteFilePath() ||
        (target.exists() && target.canonicalFilePath() == source.canonicalFilePath()))
    {
        refuse(QStringLiteral("output/invalid-or-source-destination"));
        return;
    }
    m_outputRevision = request.revision;
    m_outputDestination = destination;
    m_outputInvocation = invocation;
    m_outputCancelled = false;
    m_outputJobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_outputRecord = { { QStringLiteral("status"), QStringLiteral("preparing") } };
    m_outputResult = std::make_shared<loopeditor::OutputResult>();
    pdf::PDFJobSpec spec;
    spec.jobId = m_outputJobId;
    spec.kind = pdf::PDFJobKind::Export;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = m_session->revisionSource()->documentKey();
    spec.documentRevision = m_outputRevision;
    spec.operationId = QStringLiteral("quick-output.%1").arg(format);
    const auto result = m_outputResult;
    m_session->scheduler().submit(spec, [request, result](pdf::PDFJobContext& job)
                                  { loopeditor::prepareOutput(request, *result, job); });
    refreshFeatureAvailability();
    bumpPresentation();
}

void EditorHost::finishDocumentOutput(const pdf::PDFJobSnapshot& snapshot)
{
    if (snapshot.jobId != m_outputJobId || !m_outputResult)
        return;
    QString error = m_outputResult->error;
    if (m_outputCancelled || snapshot.status != pdf::PDFJobStatus::Succeeded || !hasDocument() ||
        snapshot.kind != pdf::PDFJobKind::Export || snapshot.documentKey != m_session->revisionSource()->documentKey() ||
        snapshot.documentRevision != outputRevision() || snapshot.documentRevision != m_outputRevision ||
        !m_session->catalog().isPending(m_outputInvocation))
    {
        error = QStringLiteral("output/cancelled-stale-or-failed");
    }
    if (error.isEmpty())
    {
        QFile staged(m_outputResult->stagedPath);
        if (!staged.open(QIODevice::ReadOnly))
            error = QStringLiteral("output/staging-read-failed");
        else
        {
            const QString expectedDigest = snapshot.outputArtifact.sha256;
            const auto written = pdf::PDFSafeFileWriter::writeDevice(m_outputDestination, [&staged, expectedDigest](QIODevice* output)
                                                                     {
                QCryptographicHash digest(QCryptographicHash::Sha256);
                while (!staged.atEnd())
                {
                    const QByteArray chunk = staged.read(65536);
                    if (chunk.isEmpty() || output->write(chunk) != chunk.size()) return false;
                    digest.addData(chunk);
                }
                return staged.error() == QFile::NoError && QString::fromLatin1(digest.result().toHex()) == expectedDigest; }, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
            if (!written)
                error = QStringLiteral("output/commit-failed");
        }
    }
    const bool ok = error.isEmpty();
    m_outputRecord = m_outputResult->record.toVariantMap();
    m_outputRecord.insert(QStringLiteral("status"), ok ? QStringLiteral("saved") : QStringLiteral("failed"));
    m_outputRecord.insert(QStringLiteral("error"), error);
    m_outputRecord.insert(QStringLiteral("printProof"), false);
    m_session->catalog().finishInvocation(m_outputInvocation, ok ? pdfinteraction::CommandTerminalState::Completed : snapshot.status == pdf::PDFJobStatus::Cancelled ? pdfinteraction::CommandTerminalState::Cancelled
                                                                                                                                                                     : pdfinteraction::CommandTerminalState::Failed,
                                          error);
    m_outputJobId.clear();
    m_outputResult.reset();
    m_outputInvocation = pdfinteraction::InvalidCommandInvocation;
    refreshFeatureAvailability();
    announceDocumentState(ok ? tr("Output saved. Artifact %1. This is not print proof.").arg(snapshot.outputArtifact.sha256.left(12)) : error);
    bumpPresentation();
    Q_EMIT documentOutputFinished();
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
                    refreshAnnotationOverlay();
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

    // Measurements and text selections are presentation state the accessible
    // summary reports, so every change re-reads the summary.
    connect(m_session->interaction(),
            &pdfinteraction::InteractionController::measurementCompleted,
            this,
            [this](const pdfinteraction::Measurement& measurement)
            {
                announceDocumentState(tr("Measured %1.").arg(pdfinteraction::measurementReadout(measurement)));
                bumpPresentation();
            });
    connect(m_session->interaction(),
            &pdfinteraction::InteractionController::measurementsCleared,
            this,
            &EditorHost::bumpPresentation);
    connect(m_session->interaction(),
            &pdfinteraction::InteractionController::textSelectionChanged,
            this,
            &EditorHost::bumpPresentation);
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
    for (const QString& id : { QStringLiteral("actionPrint"), QStringLiteral("actionExportDocument") })
    {
        pdfinteraction::CommandCatalog::Handler output;
        output.invoke = [this, id](pdfinteraction::CommandInvocationId invocation, const QVariantMap& parameters)
        {
            if (parameters.isEmpty())
            {
                Q_EMIT documentOutputRequested(id == QLatin1String("actionPrint"));
                m_session->catalog().finishInvocation(invocation, pdfinteraction::CommandTerminalState::Completed);
                return;
            }
            QVariantMap request = parameters;
            if (id == QLatin1String("actionPrint"))
                request.insert(QStringLiteral("format"), QStringLiteral("print-pdf"));
            beginDocumentOutput(invocation, request);
        };
        output.cancel = [this](pdfinteraction::CommandInvocationId invocation)
        {
            if (invocation == m_outputInvocation)
            {
                m_outputCancelled = true;
                m_session->scheduler().cancel(m_outputJobId);
            }
        };
        m_session->catalog().setHandler(id, std::move(output));
    }
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

    // ---- Select, copy and capture (issue #232) -------------------------------------
    // A selection command finishes Failed with a typed code when it applied
    // nothing, so "the menu did nothing" is reported rather than implied.
    auto bindSelectionCommand = [this](const QString& id, std::function<bool()> action)
    {
        pdfinteraction::CommandCatalog::Handler handler;
        handler.invoke = [this, action = std::move(action)](pdfinteraction::CommandInvocationId invocation, const QVariantMap&)
        {
            const bool applied = action();
            m_session->catalog().finishInvocation(invocation,
                                                  applied ? pdfinteraction::CommandTerminalState::Completed
                                                          : pdfinteraction::CommandTerminalState::Failed,
                                                  applied ? QString() : QStringLiteral("text/selection-refused"));
            bumpPresentation();
        };
        m_session->catalog().setHandler(id, std::move(handler));
    };

    bindSelectionCommand(QStringLiteral("actionSelectText"), [this]
                         { return setActiveTool(QStringLiteral("text")); });
    bindSelectionCommand(QStringLiteral("actionSelectTextAll"), [this]
                         { return m_session->interaction()->selectAllText(); });
    bindSelectionCommand(QStringLiteral("actionDeselectText"), [this]
                         { return m_session->interaction()->deselectText(); });
    bindSelectionCommand(QStringLiteral("actionSelectTable"), [this]
                         { return m_session->interaction()->selectTableUnderPointer(); });
    bindSelectionCommand(QStringLiteral("actionGoToNextLine"), [this]
                         { return m_session->interaction()->stepTextSelectionByLine(1); });
    bindSelectionCommand(QStringLiteral("actionGoToPreviousLine"), [this]
                         { return m_session->interaction()->stepTextSelectionByLine(-1); });

    // Copy reports the controller's typed refusal, so a page with no text layer
    // can never be copied as an empty success.
    pdfinteraction::CommandCatalog::Handler copy;
    copy.invoke = [this](pdfinteraction::CommandInvocationId invocation, const QVariantMap&)
    {
        const bool copied = copySelectedText();
        m_session->catalog().finishInvocation(invocation,
                                              copied ? pdfinteraction::CommandTerminalState::Completed
                                                     : pdfinteraction::CommandTerminalState::Failed,
                                              copied ? QString() : QStringLiteral("text/copy-refused"));
        bumpPresentation();
    };
    m_session->catalog().setHandler(QStringLiteral("actionCopyText"), std::move(copy));

    // Screenshot and image extraction need a destination, so the commands ask
    // the shell for one and write nothing themselves.
    bind(QStringLiteral("actionScreenshot"), [this]
         { Q_EMIT screenshotRequested(); });
    bind(QStringLiteral("actionExtractImage"), [this]
         { Q_EMIT imageExtractionRequested(); });

    // Display colour modes (#235 display half). Presentation-only: each filters
    // the pixels the canvas already holds and asks for no render. Invoking the
    // mode that is already in force returns the display to normal, which is the
    // toggle a checkable action used to provide -- there is no sixth action for
    // "normal", and a mode with no way back is a trap.
    for (const pdfquick::CanvasDisplayColorMode mode : { pdfquick::CanvasDisplayColorMode::Invert,
                                                         pdfquick::CanvasDisplayColorMode::Grayscale,
                                                         pdfquick::CanvasDisplayColorMode::HighContrast,
                                                         pdfquick::CanvasDisplayColorMode::Bitonal,
                                                         pdfquick::CanvasDisplayColorMode::Custom })
    {
        const QString actionId = pdfquick::getCanvasDisplayColorModeActionId(mode);
        bind(actionId, [this, mode]
             { setDisplayColorMode(m_displayColor.mode == mode
                                       ? QStringLiteral("normal")
                                       : pdfquick::getCanvasDisplayColorModeName(mode)); });
    }

    // Shell settings, render options and diagnostics (#239). Every one of these
    // is ADVANCED-disposition and developer-gated; binding a handler does not
    // make it reachable in a release profile.
    bind(QStringLiteral("actionOptions"), [this]
         { Q_EMIT settingsPanelRequested(); });
    bind(QStringLiteral("actionResetToFactorySettings"), [this]
         { requestSettingsReset(); });
    bind(QStringLiteral("actionCollectDiagnostics"), [this]
         { Q_EMIT diagnosticsCollectionRequested(); });

    auto bindOptionToggle = [this](const QString& commandId, const QString& optionId)
    {
        pdfinteraction::CommandCatalog::Handler handler;
        handler.invoke = [this, optionId](pdfinteraction::CommandInvocationId invocation, const QVariantMap&)
        {
            const bool enabled = !m_settings->isOptionEnabled(optionId);
            const bool applied = m_settings->setOptionEnabled(optionId, enabled);
            m_session->catalog().finishInvocation(invocation,
                                                  applied ? pdfinteraction::CommandTerminalState::Completed
                                                          : pdfinteraction::CommandTerminalState::Failed,
                                                  applied ? QString() : QStringLiteral("settings/persist-failed"));
            bumpPresentation();
        };
        m_session->catalog().setHandler(commandId, std::move(handler));
    };

    bindOptionToggle(QStringLiteral("actionRenderOptionAntialiasing"), EditorSettings::AntialiasingOptionId);
    bindOptionToggle(QStringLiteral("actionRenderOptionTextAntialiasing"), EditorSettings::TextAntialiasingOptionId);
    bindOptionToggle(QStringLiteral("actionRenderOptionSmoothPictures"), EditorSettings::SmoothPicturesOptionId);
    bindOptionToggle(QStringLiteral("actionRenderOptionDisplayAnnotations"), EditorSettings::DisplayAnnotationsOptionId);
    bindOptionToggle(QStringLiteral("actionRenderOptionIgnoreOptionalContentSettings"), EditorSettings::IgnoreOptionalContentOptionId);
    bindOptionToggle(QStringLiteral("actionRenderOptionDisplayRenderTimes"), EditorSettings::DisplayRenderTimesOptionId);
    bindOptionToggle(QStringLiteral("actionShow_Text_Blocks"), EditorSettings::ShowTextBlocksOptionId);
    bindOptionToggle(QStringLiteral("actionShow_Text_Lines"), EditorSettings::ShowTextLinesOptionId);

    pdfinteraction::CommandCatalog::Handler move;
    move.invoke = [this](pdfinteraction::CommandInvocationId invocation, const QVariantMap& parameters)
    {
        const bool routed = requestMoveSelection(parameters);
        m_session->catalog().finishInvocation(invocation,
                                              routed ? pdfinteraction::CommandTerminalState::Completed
                                                     : pdfinteraction::CommandTerminalState::Failed,
                                              routed ? QString() : QStringLiteral("move/rejected"));
        bumpPresentation();
    };
    m_session->catalog().setHandler(MoveSelectionCommandId, std::move(move));
    for (const QString& command : { QStringLiteral("actionUndo"), QStringLiteral("actionRedo") })
    {
        pdfinteraction::CommandCatalog::Handler navigation;
        navigation.invoke = [this, command](pdfinteraction::CommandInvocationId invocation, const QVariantMap& parameters)
        {
            const QString rollbackId = command == QLatin1String("actionUndo") ? m_historyUndoId : m_historyRedoId;
            bool accepted = command == QLatin1String("actionUndo") ? historyUndoAvailable() : historyRedoAvailable();
            if (accepted && parameters.isEmpty())
            {
                setWorkspace(LoopWorkspace::Fix);
                Q_EMIT historyNavigationRequested(command, rollbackId, m_session->facade().currentRevision().toString());
            }
            else if (accepted)
            {
                accepted = parameters.value(QStringLiteral("rollbackId")).toString() == rollbackId &&
                           parameters.value(QStringLiteral("revision")).toString() == m_session->facade().currentRevision().toString() &&
                           requestFixRollback(rollbackId);
            }
            m_session->catalog().finishInvocation(invocation, accepted ? pdfinteraction::CommandTerminalState::Completed : pdfinteraction::CommandTerminalState::Failed,
                                                  accepted ? QString() : QStringLiteral("history/stale-or-unavailable"));
            bumpPresentation();
        };
        m_session->catalog().setHandler(command, std::move(navigation));
    }

    // ---- Bookmarks and hyperlinks (issue #233, ADR-014) -------------------------------
    // Every edit is one typed direct-edit request on the document facade, which
    // fences the revision and the document's modify permission before Core
    // writes the outline or the link annotation. Nothing here and nothing in
    // QML edits the outline model; it re-reads the revision the edit produced.
    auto bindAuthoringEdit = [this](const QString& id,
                                    std::function<std::optional<pdf::PDFAuthoringEdit>(const QVariantMap&, QString&)> build)
    {
        pdfinteraction::CommandCatalog::Handler handler;
        handler.invoke = [this, build = std::move(build)](pdfinteraction::CommandInvocationId invocation,
                                                          const QVariantMap& parameters)
        {
            QString typedError;
            const std::optional<pdf::PDFAuthoringEdit> edit = build(parameters, typedError);
            runAuthoringEdit(invocation, edit, typedError, parameters);
        };
        m_session->catalog().setHandler(id, std::move(handler));
    };

    bindAuthoringEdit(QStringLiteral("actionBookmarkPage"),
                      [this](const QVariantMap& parameters, QString& typedError)
                      {
                          const int page = currentPage();
                          if (page < 0)
                          {
                              // The viewport has placed no page yet, so there is
                              // no page the operator is on. Refuse with that
                              // reason instead of blaming the document's page
                              // range, which is not what is wrong.
                              typedError = QStringLiteral("bookmark/no-current-page");
                              return std::optional<pdf::PDFAuthoringEdit>();
                          }

                          pdf::PDFAuthoringEdit edit;
                          edit.kind = pdf::PDFAuthoringEdit::Kind::ToggleBookmark;
                          edit.text = parameters.value(QStringLiteral("title")).toString();
                          edit.pageIndex = page;
                          return std::optional<pdf::PDFAuthoringEdit>(edit);
                      });
    bindAuthoringEdit(QStringLiteral("actionBookmarkAutoGenerate"),
                      [](const QVariantMap&, QString&)
                      {
                          pdf::PDFAuthoringEdit edit;
                          edit.kind = pdf::PDFAuthoringEdit::Kind::GenerateBookmarks;
                          return std::optional<pdf::PDFAuthoringEdit>(edit);
                      });
    bindAuthoringEdit(QStringLiteral("actionCreateHyperlink"),
                      [this](const QVariantMap& parameters, QString& typedError)
                      {
                          pdf::PDFAuthoringEdit edit;
                          edit.kind = pdf::PDFAuthoringEdit::Kind::AddUriLink;
                          edit.text = parameters.value(QStringLiteral("uri")).toString();
                          if (edit.text.isEmpty())
                          {
                              typedError = QStringLiteral("hyperlink/uri-required");
                              return std::optional<pdf::PDFAuthoringEdit>();
                          }
                          if (!authoringLinkGeometry(parameters, &edit.pageIndex, &edit.rectangle, typedError))
                          {
                              return std::optional<pdf::PDFAuthoringEdit>();
                          }
                          return std::optional<pdf::PDFAuthoringEdit>(edit);
                      });

    for (const HyperlinkDestinationVariant& variant : HyperlinkDestinationVariants)
    {
        bindAuthoringEdit(QString::fromLatin1(variant.command),
                          [this, variant](const QVariantMap& parameters, QString& typedError)
                          {
                              pdf::PDFAuthoringEdit edit;
                              edit.kind = pdf::PDFAuthoringEdit::Kind::AddDestinationLink;
                              edit.destinationType = variant.type;
                              if (!authoringLinkGeometry(parameters, &edit.pageIndex, &edit.rectangle, typedError))
                              {
                                  return std::optional<pdf::PDFAuthoringEdit>();
                              }

                              edit.targetPageIndex =
                                  parameters.value(QStringLiteral("targetPage"), currentPage()).toInt();
                              const std::optional<QRectF> target =
                                  rectangleFromParameters(parameters,
                                                          QStringLiteral("targetLeft"),
                                                          QStringLiteral("targetBottom"),
                                                          QStringLiteral("targetRight"),
                                                          QStringLiteral("targetTop"),
                                                          typedError);
                              if (!typedError.isEmpty())
                              {
                                  return std::optional<pdf::PDFAuthoringEdit>();
                              }
                              if (target.has_value())
                              {
                                  edit.targetRectangle = *target;
                              }

                              // XYZ without an explicit zoom uses the zoom on
                              // screen; the inherit-zoom variant writes the PDF
                              // null zoom instead, which keeps the viewer's own.
                              edit.zoom = variant.inheritZoom
                                              ? std::numeric_limits<pdf::PDFReal>::quiet_NaN()
                                              : parameters.value(QStringLiteral("zoom"), m_session->viewport().zoom()).toDouble();
                              return std::optional<pdf::PDFAuthoringEdit>(edit);
                          });
    }

    // Import and export carry a file, so the file is read or written here and
    // only the outline replacement is a document edit. Without a path the
    // command asks the shell for one and answers through
    // importBookmarksFileUrl()/exportBookmarksFileUrl().
    pdfinteraction::CommandCatalog::Handler importBookmarks;
    importBookmarks.invoke = [this](pdfinteraction::CommandInvocationId invocation, const QVariantMap& parameters)
    {
        const QString path = parameters.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
        {
            Q_EMIT bookmarkImportRequested();
            m_session->catalog().finishInvocation(invocation, pdfinteraction::CommandTerminalState::Completed);
            return;
        }

        QString typedError;
        std::optional<pdf::PDFBookmarkFile> file;
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly))
        {
            typedError = QStringLiteral("bookmark/file-unreadable");
        }
        else
        {
            file = pdf::PDFBookmarkFile::fromJson(source.readAll(), typedError);
        }

        std::optional<pdf::PDFAuthoringEdit> edit;
        if (file.has_value())
        {
            pdf::PDFAuthoringEdit replacement;
            replacement.kind = pdf::PDFAuthoringEdit::Kind::ReplaceBookmarks;
            replacement.bookmarks = file->bookmarks;
            edit = replacement;
        }
        runAuthoringEdit(invocation, edit, typedError, parameters);
    };
    m_session->catalog().setHandler(QStringLiteral("actionBookmarkImport"), std::move(importBookmarks));

    pdfinteraction::CommandCatalog::Handler exportBookmarks;
    exportBookmarks.invoke = [this](pdfinteraction::CommandInvocationId invocation, const QVariantMap& parameters)
    {
        const QString path = parameters.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
        {
            Q_EMIT bookmarkExportRequested();
            m_session->catalog().finishInvocation(invocation, pdfinteraction::CommandTerminalState::Completed);
            return;
        }

        QString typedError;
        pdf::PDFBookmarkFile file;
        const pdf::PDFDocumentPointer document = m_session->context().getDocumentPointer();
        const QString displayed = parameters.value(QStringLiteral("revision")).toString();
        std::optional<QList<pdf::PDFBookmarkRecord>> records;
        if (!document)
        {
            typedError = QStringLiteral("bookmark/document-not-ready");
        }
        else if (!displayed.isEmpty() && displayed != m_session->facade().currentRevision().toString())
        {
            // The file would describe a revision the document has left.
            typedError = QStringLiteral("bookmark/stale-revision");
        }
        else
        {
            records = pdf::PDFAuthoringEditWriter::readOutline(document.data(), typedError);
        }

        if (records.has_value())
        {
            file.bookmarks = *records;
            const pdf::PDFOperationResult written = pdf::PDFSafeFileWriter::writeData(
                path, file.toJson(), pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
            if (!written)
            {
                typedError = QStringLiteral("bookmark/export-failed");
            }
        }

        const bool exported = typedError.isEmpty() && records.has_value();
        if (exported)
        {
            announceDocumentState(tr("Exported %n bookmark(s).", "", file.count()));
        }
        else
        {
            announceDocumentState(tr("The bookmarks could not be exported: %1").arg(typedError));
        }
        m_session->catalog().finishInvocation(invocation,
                                              exported ? pdfinteraction::CommandTerminalState::Completed
                                                       : pdfinteraction::CommandTerminalState::Failed,
                                              exported ? QString() : typedError);
        bumpPresentation();
    };
    m_session->catalog().setHandler(QStringLiteral("actionBookmarkExport"), std::move(exportBookmarks));

    for (const QString& id : { QStringLiteral("actionGotoNextBookmark"), QStringLiteral("actionGotoPreviousBookmark") })
    {
        pdfinteraction::CommandCatalog::Handler navigation;
        navigation.invoke = [this, id](pdfinteraction::CommandInvocationId invocation, const QVariantMap&)
        {
            QString typedError;
            const bool moved = goToAdjacentBookmark(id == QLatin1String("actionGotoNextBookmark") ? 1 : -1,
                                                    typedError);
            if (moved)
            {
                announceDocumentState(tr("Moved to bookmark on page %1.").arg(currentPage() + 1));
            }
            else
            {
                announceDocumentState(tr("No bookmark to move to: %1").arg(typedError));
            }
            m_session->catalog().finishInvocation(invocation,
                                                  moved ? pdfinteraction::CommandTerminalState::Completed
                                                        : pdfinteraction::CommandTerminalState::Failed,
                                                  moved ? QString() : typedError);
            bumpPresentation();
        };
        m_session->catalog().setHandler(id, std::move(navigation));
    }

    refreshFeatureAvailability();
}

void EditorHost::refreshFeatureAvailability()
{
    const bool ready = hasDocument();
    QHash<pdfinteraction::CommandId, bool> availability;
    for (const QString& id : { QStringLiteral("actionPageLayoutContinuous"), QStringLiteral("actionPageLayoutSinglePage"),
                               QStringLiteral("actionPageLayoutTwoColumns"), QStringLiteral("actionPageLayoutTwoPages"),
                               QStringLiteral("actionFind"), QStringLiteral("actionProperties"), MoveSelectionCommandId,
                               QStringLiteral("actionColorInvert"), QStringLiteral("actionColorGrayscale"),
                               QStringLiteral("actionColorHighContrast"), QStringLiteral("actionColorBitonal"),
                               QStringLiteral("actionColorCustom") })
    {
        availability.insert(id, ready);
    }
    const bool hasSearchResults = ready && m_documentModel.searchResultCount() > 0;
    availability.insert(QStringLiteral("actionFindNext"), hasSearchResults);
    availability.insert(QStringLiteral("actionFindPrevious"), hasSearchResults);
    availability.insert(QStringLiteral("actionFullscreenMode"), true);
    availability.insert(QStringLiteral("actionUndo"), historyUndoAvailable());
    availability.insert(QStringLiteral("actionRedo"), historyRedoAvailable());
    availability.insert(MoveSelectionCommandId, ready && m_session->facade().permissions().allowsCorrection());

    // Select, copy and capture (issue #232). Every one of these is enabled with
    // a document: Copy deliberately stays enabled without a selection so its
    // typed refusal is reachable and visible, rather than the command silently
    // disappearing from the menu.
    for (const QString& id : { QStringLiteral("actionSelectText"), QStringLiteral("actionSelectTextAll"),
                               QStringLiteral("actionDeselectText"), QStringLiteral("actionSelectTable"),
                               QStringLiteral("actionCopyText"), QStringLiteral("actionGoToNextLine"),
                               QStringLiteral("actionGoToPreviousLine"), QStringLiteral("actionScreenshot"),
                               QStringLiteral("actionExtractImage") })
    {
        availability.insert(id, ready);
    }

    // The developer-diagnostics shell surfaces (#239): settings, render options
    // and the diagnostics bundle. They follow the same flag the menu gate
    // reads, so a release profile cannot reach them even by invocation.
    const bool developerDiagnostics = allowDeveloperDiagnostics();
    for (const QString& id : { QStringLiteral("actionOptions"),
                               QStringLiteral("actionResetToFactorySettings"),
                               QStringLiteral("actionCollectDiagnostics"),
                               QStringLiteral("actionRenderOptionAntialiasing"),
                               QStringLiteral("actionRenderOptionTextAntialiasing"),
                               QStringLiteral("actionRenderOptionSmoothPictures"),
                               QStringLiteral("actionRenderOptionDisplayAnnotations"),
                               QStringLiteral("actionRenderOptionIgnoreOptionalContentSettings"),
                               QStringLiteral("actionRenderOptionDisplayRenderTimes"),
                               QStringLiteral("actionShow_Text_Blocks"),
                               QStringLiteral("actionShow_Text_Lines") })
    {
        availability.insert(id, developerDiagnostics);
    }

    availability.insert(QStringLiteral("actionPrint"), ready && !outputBusy() && m_documentModel.canPrint());
    availability.insert(QStringLiteral("actionExportDocument"), ready && !outputBusy() && m_documentModel.canCopy());

    // Bookmarks and hyperlinks (#233). The edits are document mutations, so they
    // follow the document's modify permission; navigation needs an outline to
    // move through, and export follows the same copy permission the document
    // export uses.
    const bool canModify = ready && m_session->facade().permissions().modify;
    for (const QString& id : { QStringLiteral("actionBookmarkPage"),
                               QStringLiteral("actionBookmarkAutoGenerate"),
                               QStringLiteral("actionBookmarkImport"),
                               QStringLiteral("actionCreateHyperlink"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFFit"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFFitB"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFFitBH"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFFitBV"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFFitH"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFFitR"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFFitV"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFXYZ"),
                               QStringLiteral("actionCreateHyperlinkToThisPDFXYZInheritZoom") })
    {
        availability.insert(id, canModify);
    }
    availability.insert(QStringLiteral("actionBookmarkExport"), ready && m_documentModel.canCopy());
    for (const QString& id : { QStringLiteral("actionGotoNextBookmark"), QStringLiteral("actionGotoPreviousBookmark") })
    {
        availability.insert(id, ready && m_documentModel.hasOutline());
    }

    m_session->catalog().setEnabledBatch(availability);
}

void EditorHost::runAuthoringEdit(pdfinteraction::CommandInvocationId invocation,
                                  const std::optional<pdf::PDFAuthoringEdit>& edit,
                                  const QString& buildError,
                                  const QVariantMap& parameters)
{
    QString typedError = buildError;
    pdfinteraction::DocumentEditOutcome outcome;
    if (edit.has_value())
    {
        outcome = submitAuthoringEdit(*edit, parameters, typedError);
    }

    const bool applied = edit.has_value() && outcome.applied;
    if (applied)
    {
        announceDocumentState(authoringEditAnnouncement(*edit, outcome));
    }
    else
    {
        announceDocumentState(tr("The edit was refused: %1").arg(typedError));
    }

    m_session->catalog().finishInvocation(invocation,
                                          applied ? pdfinteraction::CommandTerminalState::Completed
                                                  : pdfinteraction::CommandTerminalState::Failed,
                                          applied ? QString() : typedError);
    bumpPresentation();
    refreshFeatureAvailability();
}

pdfinteraction::DocumentEditOutcome EditorHost::submitAuthoringEdit(const pdf::PDFAuthoringEdit& edit,
                                                                    const QVariantMap& parameters,
                                                                    QString& typedError)
{
    const pdf::PDFRevisionIdentity current = m_session->facade().currentRevision();
    const QString displayed = parameters.value(QStringLiteral("revision")).toString();
    if (!displayed.isEmpty() && displayed != current.toString())
    {
        // The surface acted on a revision the document has left; the edit is
        // refused rather than rebased onto one the operator never saw.
        typedError = QStringLiteral("edit/stale-revision");
        return {};
    }

    pdfinteraction::DocumentEditRequest request;
    request.edit = edit;
    request.revision = current;
    const pdfinteraction::DocumentEditOutcome outcome = m_session->facade().applyAuthoringEdit(request);
    typedError = outcome.typedError;
    return outcome;
}

bool EditorHost::authoringLinkGeometry(const QVariantMap& parameters, int* pageIndex, QRectF* rectangle,
                                       QString& typedError) const
{
    const std::optional<QRectF> explicitRectangle = rectangleFromParameters(parameters,
                                                                            QStringLiteral("left"),
                                                                            QStringLiteral("bottom"),
                                                                            QStringLiteral("right"),
                                                                            QStringLiteral("top"),
                                                                            typedError);
    if (!typedError.isEmpty())
    {
        return false;
    }

    const QVariant page = parameters.value(QStringLiteral("page"));
    if (explicitRectangle.has_value() || page.isValid())
    {
        if (!explicitRectangle.has_value() || !page.isValid())
        {
            typedError = QStringLiteral("hyperlink/incomplete-geometry");
            return false;
        }

        *pageIndex = page.toInt();
        *rectangle = *explicitRectangle;
        return true;
    }

    // No explicit geometry: the current text selection is the shell's own
    // "this region" gesture, already fenced to the revision it was taken from.
    const pdfinteraction::InteractionController* interaction = m_session->interaction();
    if (interaction)
    {
        const std::optional<pdfinteraction::TextSelection>& selection = interaction->state().textSelection();
        if (selection.has_value() && selection->isValid() && selection->available && !selection->geometry.isEmpty())
        {
            QRectF bounds;
            for (const QRectF& span : selection->geometry)
            {
                bounds = bounds.united(span);
            }

            if (bounds.isValid() && !bounds.isEmpty())
            {
                *pageIndex = selection->pageIndex;
                *rectangle = bounds;
                return true;
            }
        }
    }

    typedError = QStringLiteral("hyperlink/link-rectangle-required");
    return false;
}

bool EditorHost::goToAdjacentBookmark(int direction, QString& typedError)
{
    if (!hasDocument())
    {
        typedError = QStringLiteral("bookmark/document-not-ready");
        return false;
    }

    const pdf::PDFDocumentPointer document = m_session->context().getDocumentPointer();
    const QList<int> pages = pdf::PDFAuthoringEditWriter::bookmarkPages(document.data());
    if (pages.isEmpty())
    {
        typedError = QStringLiteral("bookmark/none");
        return false;
    }

    // Outline order, not page order: the first bookmark after (or before) the
    // current page, wrapping at the ends like the shell's historical bookmark
    // cursor. The page the operator is already on is never its own destination.
    const int current = currentPage();
    int target = -1;
    if (direction > 0)
    {
        for (const int page : pages)
        {
            if (page > current)
            {
                target = page;
                break;
            }
        }
        if (target < 0)
        {
            target = pages.constFirst();
        }
    }
    else
    {
        for (auto it = pages.crbegin(); it != pages.crend(); ++it)
        {
            if (*it < current)
            {
                target = *it;
                break;
            }
        }
        if (target < 0)
        {
            target = pages.constLast();
        }
    }

    goToPage(target);
    return true;
}

QString EditorHost::authoringEditAnnouncement(const pdf::PDFAuthoringEdit& edit,
                                              const pdfinteraction::DocumentEditOutcome& outcome) const
{
    switch (outcome.resolution)
    {
        case pdf::PDFAuthoringEditResult::Kind::BookmarkAdded:
            return tr("Bookmark added for page %1.").arg(edit.pageIndex + 1);

        case pdf::PDFAuthoringEditResult::Kind::BookmarkRemoved:
            return tr("Bookmark removed for page %1.").arg(edit.pageIndex + 1);

        case pdf::PDFAuthoringEditResult::Kind::BookmarksGenerated:
            return tr("Generated %n bookmark(s).", "", outcome.affectedCount);

        case pdf::PDFAuthoringEditResult::Kind::BookmarksReplaced:
            return tr("Imported %n bookmark(s).", "", outcome.affectedCount);

        case pdf::PDFAuthoringEditResult::Kind::LinkCreated:
            return tr("Hyperlink created.");

        case pdf::PDFAuthoringEditResult::Kind::Refused:
            break;
    }

    return QString();
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
    applyDisplayColorToCanvas();
    Q_EMIT presentationChanged();
}

void EditorHost::bumpCommandEpoch()
{
    ++m_commandEpoch;
    Q_EMIT commandEpochChanged();
}

void EditorHost::onDocumentReady()
{
    // The persisted render options must be in force before the first surfaces
    // are requested, so no page is rendered twice on open.
    applyRenderOptions();
    m_session->prepareDocumentView();

    syncRevisionModels();
    m_documentModel.setDocument(&m_session->context());
    syncDocumentLifecycle();
    m_searchRow = -1;
    refreshHitTestSources();
    reloadProductionPlates();
    m_documentBound = true;
    bindCanvas();
    updateCanvasAccessibilitySummary();
    applyEmptyCanvasInspectorSelection();
    refreshAnnotationOverlay();
    submitSignatureCheck();
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
    if (outputBusy())
        m_session->scheduler().cancel(m_outputJobId);
    m_publishedOutputBytes.clear();
    m_publicationReceipt = {};
    m_publicationRevision.clear();
    m_outputRecord.clear();
    m_publishedHistoryPath.clear();
    m_publishedHistoryRevision.clear();
    cancelPreflight();
    cancelActionList();
    if (!m_signatureCheckJobId.isEmpty())
    {
        m_session->scheduler().cancel(m_signatureCheckJobId);
        m_signatureCheckJobId.clear();
    }
    m_signatureCheckOutcome.reset();
    m_fixSignatureInvalidationAcknowledgedDigest.clear();
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
    refreshAnnotationOverlay();
    m_searchRow = -1;
    m_preview.clear();
    m_production.clear();
    // Proofing and the plate list describe the document that just closed, so
    // neither may outlive it: a soft proof of a document nobody has open would
    // keep the canvas on a simulation with no revision behind it.
    m_softProofEnabled = false;
    m_softProofStateName = QStringLiteral("off");
    m_softProofProfile = SoftProofProfile();
    m_plates.clear();
    m_plateVisible.clear();
    m_isolatedPlateId.clear();
    m_inkCoverageReadout = QVariantMap();
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

    const auto profile = std::find_if(m_preflightProfiles.cbegin(), m_preflightProfiles.cend(),
                                      [this](const PreflightProfileChoice& choice)
                                      { return choice.id == m_selectedPreflightProfileId; });
    if (!hasDocument() || !m_session->revisionSource() || snapshot.kind != pdf::PDFJobKind::Preflight ||
        snapshot.documentKey != m_preflight.documentKey() ||
        snapshot.documentKey != m_session->revisionSource()->documentKey() ||
        snapshot.documentRevision != m_preflight.documentRevision() ||
        snapshot.documentRevision != m_session->facade().currentRevision().toString() ||
        profile == m_preflightProfiles.cend() || !profile->valid ||
        profile->digest != m_preflight.profileDigest() ||
        snapshot.operationId != QStringLiteral("preflight.%1").arg(profile->id))
    {
        m_preflight.markProfileStale();
        return;
    }

    switch (snapshot.status)
    {
        case pdf::PDFJobStatus::Succeeded:
            if (outcome)
            {
                if (outcome->effectiveProfileDigest.isEmpty() ||
                    outcome->result.effectiveProfileDigest != outcome->effectiveProfileDigest ||
                    outcome->documentPath != m_session->facade().source().path)
                {
                    m_preflight.failRun(snapshot.jobId, snapshot.documentRevision,
                                        tr("Preflight result identity did not match the request."));
                    break;
                }
                const pdf::PDFOperationHistoryStatus auditStatus =
                    pdf::reducePreflightVerdict(outcome->result).state == pdf::PreflightVerdictState::Error
                        ? pdf::PDFOperationHistoryStatus::Failed
                        : pdf::PDFOperationHistoryStatus::Accepted;
                const pdf::PDFOperationResult auditResult = pdf::appendPreflightAuditRun(
                    outcome->documentPath, outcome->auditBytes, outcome->result, auditStatus,
                    QStringLiteral("LoopEditor"), outcome->auditSummary);
                if (!auditResult)
                {
                    m_preflight.failRun(snapshot.jobId, snapshot.documentRevision, auditResult.getErrorMessage());
                    break;
                }
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

    const pdfinteraction::ActionListRecipeEntry* recipe = m_actionListCatalog.recipe(m_selectedActionListRecipeId);
    if (!hasDocument() || !m_session->revisionSource() || snapshot.kind != pdf::PDFJobKind::Other ||
        snapshot.documentKey != m_actionListController.documentKey() ||
        snapshot.documentKey != m_session->revisionSource()->documentKey() ||
        snapshot.documentRevision != m_actionListController.documentRevision() ||
        snapshot.documentRevision != m_session->facade().currentRevision().toString() ||
        m_selectedActionListRecipeId != m_actionListController.recipeId() ||
        !recipe || !recipe->valid ||
        snapshot.operationId != QStringLiteral("action-list.%1").arg(recipe->actionList.id) ||
        snapshot.checkId != recipe->actionList.name)
    {
        m_actionListController.markRecipeStale();
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
            if (state != pdfinteraction::ActionListController::State::Validating &&
                outcome->executionResult.recipeHash != recipe->recipeHash)
            {
                m_actionListController.failRun(snapshot.jobId, snapshot.documentRevision,
                                               tr("Action List result did not match the recipe."));
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
                    // The plan belongs to the revision it was produced for, so a review decision
                    // can never transfer to a newer revision.
                    m_fixPlannedDocumentRevision = snapshot.documentRevision;
                    m_fixPlannedPlanDigest = outcome->executionResult.planDigest;
                    clearFixReview();
                }
            }
            else if (state == pdfinteraction::ActionListController::State::Running)
            {
                if (!outcome->candidate)
                {
                    QStringList reasons;
                    const auto appendDiagnostics = [&reasons](const QJsonArray& diagnostics)
                    {
                        for (const QJsonValue& diagnostic : diagnostics)
                        {
                            reasons.append(diagnostic.toObject().value(QStringLiteral("message")).toString());
                        }
                    };
                    appendDiagnostics(outcome->executionResult.diagnostics);
                    for (const pdf::PDFActionListStepResult& step : outcome->executionResult.steps)
                    {
                        if (step.status != pdf::PDFActionListStepStatus::Failed)
                        {
                            continue;
                        }
                        appendDiagnostics(step.diagnostics);
                        for (const QJsonValue& failure : step.repairResult.value(QStringLiteral("validation_failures")).toArray())
                        {
                            reasons.append(failure.toString());
                        }
                    }
                    reasons.removeAll(QString());
                    reasons.removeDuplicates();
                    m_actionListController.failRun(snapshot.jobId, snapshot.documentRevision,
                                                   reasons.isEmpty() ? tr("Action List produced no document.")
                                                                     : reasons.join(QLatin1Char(' ')));
                    break;
                }
                if (m_acceptActionListResults &&
                    m_actionListController.acceptExecution(snapshot.jobId, snapshot.documentRevision, outcome->executionResult))
                {
                    m_session->context().setDocument(outcome->candidate);
                    m_session->scheduler().setCurrentRevision(m_session->revisionSource()->documentKey(), m_session->facade().currentRevision().toString());
                    m_publishedOutputBytes = outcome->publishedBytes;
                    m_publicationReceipt = outcome->executionResult.governed.value(QStringLiteral("receipt")).toObject();
                    m_publicationRevision = m_session->facade().currentRevision().toString();
                    m_session->scheduler().setCurrentRevision(m_session->revisionSource()->documentKey(),
                                                              m_session->facade().currentRevision().toString());
                    m_publishedHistoryPath = outcome->publishedPath;
                    m_publishedHistoryRevision = outcome->publishedPath.isEmpty() ? QString()
                                                                                  : m_session->facade().currentRevision().toString();
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

void EditorHost::finishRollbackJob(const pdf::PDFJobSnapshot& snapshot)
{
    const std::shared_ptr<RollbackJobOutcome> outcome = m_rollbackOutcomes.take(snapshot.jobId);
    if (!outcome)
    {
        return;
    }

    if (snapshot.status == pdf::PDFJobStatus::Stale || snapshot.status == pdf::PDFJobStatus::Cancelled ||
        !hasDocument() || !m_session->revisionSource() ||
        snapshot.documentKey != m_session->revisionSource()->documentKey() ||
        snapshot.documentRevision != m_session->facade().currentRevision().toString())
    {
        return;
    }

    if (snapshot.status == pdf::PDFJobStatus::Succeeded && outcome->ok)
    {
        const QString destinationName = QFileInfo(outcome->destinationPath).fileName();
        if (outcome->retentionSuccess)
        {
            announceDocumentState(tr("Returned to revision %1 as %2.").arg(outcome->revision, destinationName));
        }
        else
        {
            announceDocumentState(tr("Returned to revision %1 as %2, but history retention could not be enforced: %3")
                                      .arg(outcome->revision, destinationName, outcome->retentionError));
        }
        openFileUrl(QUrl::fromLocalFile(outcome->destinationPath));
        refreshFixRollbackPoints();
        return;
    }

    const QString message = outcome->errorMessage.isEmpty() ? snapshot.errorMessage : outcome->errorMessage;
    refreshFeatureAvailability();
    announceDocumentState(tr("The document was not rolled back: %1").arg(message));
    bumpPresentation();
}

void EditorHost::finishSignatureCheckJob(const pdf::PDFJobSnapshot& snapshot)
{
    if (snapshot.jobId.isEmpty() || snapshot.jobId != m_signatureCheckJobId || !m_signatureCheckOutcome)
    {
        return;
    }

    const std::shared_ptr<SignatureCheckOutcome> outcome = m_signatureCheckOutcome;
    m_signatureCheckJobId.clear();
    m_signatureCheckOutcome.reset();

    if (snapshot.kind != pdf::PDFJobKind::Other ||
        snapshot.operationId != QStringLiteral("document.verify-signatures"))
    {
        return;
    }

    QString transitionError;
    if (snapshot.status == pdf::PDFJobStatus::Succeeded && outcome->ok)
    {
        // The facade refuses a completion that belongs to another request,
        // generation or revision; a refused completion presents nothing.
        if (m_session->facade().completeSignatureCheck(outcome->token, outcome->results, transitionError))
        {
            bumpPresentation();
        }
        return;
    }

    QString detail = outcome->detail;
    if (detail.isEmpty())
    {
        detail = snapshot.status == pdf::PDFJobStatus::Failed
                     ? tr("Signature verification failed for this document.")
                     : tr("Signature verification did not complete for this document.");
    }
    if (m_session->facade().failSignatureCheck(outcome->token, detail, transitionError))
    {
        bumpPresentation();
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
    if (outputBusy() && documentRevision != m_outputRevision)
        m_session->scheduler().cancel(m_outputJobId);
    if (documentRevision != m_publicationRevision)
    {
        m_publishedOutputBytes.clear();
        m_publicationReceipt = {};
        m_publicationRevision.clear();
    }
    m_actionListController.setCurrentRevision(documentKey, documentRevision);
    m_inspector.setCurrentRevision(documentKey, documentRevision);
    m_preview.setCurrentRevision(documentKey, documentRevision);
    m_production.setCurrentRevision(documentKey, documentRevision);
    // A review decision belongs to the revision it was made for and never survives a revision
    // change as current evidence. The plan itself is remembered so a superseded one is reported
    // as stale instead of silently vanishing.
    clearFixReview();
    refreshFixRollbackPointsFromHistory();
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

void EditorHost::refreshAnnotationOverlay()
{
    // Read-only review marks (issue #125). Every annotation row becomes one
    // page-space rectangle; the selected row carries the selection treatment.
    // The marks are not interaction targets: their kind stays None, so nothing
    // hit-tests them and nothing routes a click through them.
    const QuickAnnotationModel& annotations = m_documentModel.annotationModel();
    const QList<QuickAnnotation>& rows = annotations.rows();

    QList<pdfinteraction::InteractionTarget> marks;
    marks.reserve(rows.size());
    for (const QuickAnnotation& row : rows)
    {
        pdfinteraction::InteractionTarget target;
        target.kind = pdfinteraction::InteractionTargetKind::None;
        target.pageIndex = row.pageIndex;
        target.id = QStringLiteral("annotation:") + row.reference;
        target.pageBounds = row.bounds;
        marks.append(target);
    }

    const int selectedRow = annotations.selectedRow();
    const QString selectedId = selectedRow >= 0 && selectedRow < rows.size()
                                   ? QStringLiteral("annotation:") + rows.at(selectedRow).reference
                                   : QString();

    m_session->overlays()->setAnnotations(std::move(marks));
    m_session->overlays()->setSelectedAnnotationId(selectedId);
    m_session->interaction()->refreshOverlay();
}

void EditorHost::submitSignatureCheck()
{
    if (!hasDocument() || !m_session->revisionSource())
    {
        return;
    }

    const pdf::PDFDocumentPointer document = m_session->context().getDocumentPointer();
    if (!document)
    {
        return;
    }

    QString beginError;
    const std::optional<pdfinteraction::DocumentSignatureCheckToken> token =
        m_session->facade().beginSignatureCheck(beginError);
    if (!token.has_value())
    {
        return;
    }

    const QString jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    pdf::PDFJobSpec spec;
    spec.jobId = jobId;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = m_session->revisionSource()->documentKey();
    spec.documentRevision = m_session->facade().currentRevision().toString();
    spec.operationId = QStringLiteral("document.verify-signatures");
    spec.checkId = QStringLiteral("signature-verification");
    spec.progressModel = QStringLiteral("signature-check-progress-v1");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    const QString documentPath = m_session->facade().source().path;
    const QByteArray expectedSourceHash = m_session->context().getDocumentIdentity().sourceDataHash;

    auto outcome = std::make_shared<SignatureCheckOutcome>();
    outcome->token = *token;
    m_signatureCheckJobId = jobId;
    m_signatureCheckOutcome = outcome;

    const QString submittedId = m_session->scheduler().submit(
        spec,
        [document, documentPath, expectedSourceHash, outcome](pdf::PDFJobContext& context)
        {
            if (context.isCancellationRequested())
            {
                return;
            }

            // Verification needs the exact source bytes a signature covers, and
            // the loaded document retains only their digest. Re-reading the file
            // is admitted only when the bytes still hash to the revision the
            // document was loaded from; a changed file is a failed check, never
            // a verdict about the open revision.
            QFile file(documentPath);
            if (!file.open(QIODevice::ReadOnly))
            {
                outcome->ok = false;
                outcome->detail = QStringLiteral("The source file could not be read for signature verification.");
                return;
            }
            const QByteArray sourceData = file.readAll();
            if (QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256) != expectedSourceHash)
            {
                outcome->ok = false;
                outcome->detail = QStringLiteral("The file changed since the document was loaded; its signatures were not verified.");
                return;
            }

            try
            {
                pdf::PDFCertificateStore certificateStore;
                pdf::PDFSignatureHandler::Parameters parameters;
                parameters.store = &certificateStore;
                parameters.dss = &document->getCatalog()->getDocumentSecurityStore();
                parameters.enableVerification = true;
                parameters.useSystemCertificateStore = true;
                const pdf::PDFForm form =
                    pdf::PDFForm::parse(document.get(), document->getCatalog()->getFormObject());
                outcome->results = pdf::PDFSignatureHandler::verifySignatures(form, sourceData, parameters);
                outcome->ok = true;
            }
            catch (const std::exception&)
            {
                outcome->ok = false;
                outcome->detail = QStringLiteral("Signature verification could not complete for this document.");
            }
        });
    if (submittedId != jobId)
    {
        m_signatureCheckJobId.clear();
        m_signatureCheckOutcome.reset();
        QString failError;
        m_session->facade().failSignatureCheck(*token,
                                               tr("Signature verification could not be scheduled."),
                                               failError);
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
    QString summary = tr("Document canvas. Page %1 of %2. Zoom %3 percent.").arg(pageNumber).arg(pages).arg(zoomPercent);

    // Measurements and text selections are readouts, so the screen reader gets
    // them from the same summary the canvas already exposes.
    const pdfinteraction::InteractionController* interaction = m_session->interaction();
    if (interaction)
    {
        const pdfinteraction::InteractionState& state = interaction->state();
        const std::optional<pdfinteraction::TextSelection>& selection = state.textSelection();
        if (selection.has_value())
        {
            if (!selection->available)
            {
                summary += QLatin1Char(' ') + tr("Text selection unavailable: %1").arg(selection->unavailableReason);
            }
            else if (!selection->text.isEmpty())
            {
                summary += QLatin1Char(' ') + tr("Selected %n character(s).", "", selection->text.size());
            }
        }

        if (!state.measurements().isEmpty())
        {
            summary += QLatin1Char(' ') + tr("Measurements: %1. Latest: %2.")
                                              .arg(state.measurements().size())
                                              .arg(pdfinteraction::measurementReadout(state.measurements().last()));
        }
    }

    // The outline is the one document structure the canvas can announce (issue
    // #233): its count is read from the same Core projection bookmark
    // navigation moves through.
    const QList<int> bookmarkPages =
        pdf::PDFAuthoringEditWriter::bookmarkPages(m_session->context().getDocumentPointer().data());
    if (!bookmarkPages.isEmpty())
    {
        summary += QLatin1Char(' ') + tr("%n bookmark(s).", "", bookmarkPages.size());
    }

    m_canvas->setAccessibleDocumentSummary(summary);
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
    // be reached through QVariant/int callers. Reject out-of-range values
    // fail-closed; every registered destination resolves to a real surface.
    return workspace >= LoopWorkspace::Document && workspace <= LoopWorkspace::Compare;
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

bool EditorHost::requestMoveSelection(const QVariantMap& parameters)
{
    if (!m_session->facade().permissions().allowsCorrection())
    {
        return false;
    }
    if (parameters.value(QStringLiteral("targetKind")).toString() != QLatin1String("PageBox"))
    {
        announceDocumentState(tr("Only page boxes can be moved."));
        return false;
    }

    const QString box = parameters.value(QStringLiteral("targetId")).toString();
    const int page = parameters.value(QStringLiteral("page")).toInt();
    const double dx = parameters.value(QStringLiteral("dx")).toDouble();
    const double dy = parameters.value(QStringLiteral("dy")).toDouble();
    if (page < 0 || !std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.0 && dy == 0.0))
    {
        return false;
    }

    // The move is only proposed here: the recipe, plan, approval and execute
    // flow of the Fix workspace stays the one place a document is changed.
    setWorkspace(LoopWorkspace::Fix);
    m_actionListBindings = QJsonObject{ { QStringLiteral("box"), box },
                                        { QStringLiteral("page_index"), page },
                                        { QStringLiteral("dx"), dx },
                                        { QStringLiteral("dy"), dy } };
    return selectActionListRecipeForOperation(TranslatePageBoxOperationId);
}

QString EditorHost::dragRefusalMessage(pdfinteraction::InteractionTargetKind kind)
{
    switch (kind)
    {
        case pdfinteraction::InteractionTargetKind::Finding:
            return tr("A finding can't be moved: its position comes from the document. Fix the cause in the Fix workspace.");
        case pdfinteraction::InteractionTargetKind::Guide:
            return tr("A guide can't be moved yet: guide editing has no approved contract.");
        case pdfinteraction::InteractionTargetKind::DragHandle:
            return tr("A handle can't be dragged yet: transforms have no approved contract.");
        case pdfinteraction::InteractionTargetKind::PageBox:
        case pdfinteraction::InteractionTargetKind::Page:
        case pdfinteraction::InteractionTargetKind::None:
            break;
    }
    return tr("This can't be moved.");
}

void EditorHost::onDragCompleted(pdfinteraction::DragSession session)
{
    if (hasDocument() && pdfinteraction::getDragCommitDisposition(session.target.kind) == pdfinteraction::DragCommitDisposition::Refused)
    {
        announceDocumentState(dragRefusalMessage(session.target.kind));
        Q_EMIT dragRefused(QString::fromLatin1(pdfinteraction::getInteractionTargetKindName(session.target.kind)), session.target.id);
    }
    else if (hasDocument())
    {
        if (session.fence.revision != m_session->facade().currentRevision())
        {
            announceDocumentState(tr("Move discarded: the document changed during the drag."));
        }
        else
        {
            m_session->catalog().invoke(MoveSelectionCommandId,
                                        { { QStringLiteral("targetKind"), QStringLiteral("PageBox") },
                                          { QStringLiteral("targetId"), session.target.id },
                                          { QStringLiteral("page"), session.target.pageIndex },
                                          { QStringLiteral("dx"), session.pageDelta.x() },
                                          { QStringLiteral("dy"), session.pageDelta.y() } });
        }
    }
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
        { QStringLiteral("document"), tr("Document"), displayTitle() },
        { QStringLiteral("pages"), tr("Pages"), QString::number(pageCount()) },
        { QStringLiteral("document-status"), tr("Document status"), documentShellStatus() },
        { QStringLiteral("preflight"), tr("Preflight"), preflightStateName() },
        { QStringLiteral("production"), tr("Production"), productionStateName() },
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
            { QStringLiteral("id"), tr("Image"), target.id.mid(6) },
            { QStringLiteral("page"), tr("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("bounds"), tr("Bounds"),
              QStringLiteral("%1,%2 %3x%4")
                  .arg(QString::number(target.pageBounds.x()),
                       QString::number(target.pageBounds.y()),
                       QString::number(target.pageBounds.width()),
                       QString::number(target.pageBounds.height())) },
            { QStringLiteral("dpi"), tr("Effective DPI"), tr("pending") },
            { QStringLiteral("colour-space"), tr("Colour space"), tr("pending") },
            { QStringLiteral("compression"), tr("Compression"), tr("pending") },
            { QStringLiteral("mask"), tr("Mask"), tr("pending") },
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
            { QStringLiteral("name"), tr("Ink"), target.id.mid(11) },
            { QStringLiteral("page"), tr("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("coverage"), tr("Ink coverage"), tr("pending") },
            { QStringLiteral("kind"), tr("Process / spot"), tr("pending") },
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
            { QStringLiteral("page"), tr("Page"), QString::number(target.pageIndex + 1) },
            { QStringLiteral("box"), tr("Box"), target.id },
            { QStringLiteral("size"), tr("Size"),
              QStringLiteral("%1 x %2")
                  .arg(QString::number(target.pageBounds.width()), QString::number(target.pageBounds.height())) },
            { QStringLiteral("rotation"), tr("Rotation"), QStringLiteral("%1°").arg(rotationDegrees()) },
            { QStringLiteral("ocg"), tr("Optional content"), m_documentModel.hasOptionalContent() ? tr("present") : tr("none") },
        };
        m_inspector.setSelection(selection);
        return;
    }

    applyEmptyCanvasInspectorSelection();
}
