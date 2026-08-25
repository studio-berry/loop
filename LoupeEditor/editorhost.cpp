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
#include "loupecanvasitem.h"
#include "preflightcontroller.h"
#include "previewstatemodel.h"

#include "pdfpage.h"

#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QAccessibilityHints>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMetaEnum>
#include <QScreen>
#include <QUrl>

namespace
{

constexpr auto QuitCommandId = QStringLiteral("actionQuit");

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
    }
    return QStringLiteral("not-checked");
}

QVariantMap descriptorToVariant(const pdfinteraction::CommandDescriptor& descriptor, bool enabled)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("id"), descriptor.id);
    entry.insert(QStringLiteral("labelKey"), descriptor.labelKey);
    entry.insert(QStringLiteral("implemented"), descriptor.isImplemented());
    entry.insert(QStringLiteral("enabled"), enabled);

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
    m_preflight(&m_session->scheduler(), this)
{
    connectFacade();
    connectViewport();
    connectCatalog();
    connectInteraction();
    registerShellHandlers();

    m_preflightOverlayBridge.setFindingsModel(m_preflight.findingsModel());
    m_preflightOverlayBridge.setOverlayBuilder(m_session->overlays());
    m_preflightOverlayBridge.setInteractionController(m_session->interaction());

    connect(&m_preflight, &pdfinteraction::PreflightController::stateChanged, this, &EditorHost::bumpPresentation);
    connect(m_preflight.findingsModel(), &pdfinteraction::PreflightFindingsModel::findingsReplaced, this, &EditorHost::refreshHitTestSources);
    connect(&m_preflight, &pdfinteraction::PreflightController::navigationRequested, this, &EditorHost::onPreflightNavigation);
    connect(&m_inspector, &pdfinteraction::InspectorModel::selectionChanged, this, &EditorHost::bumpPresentation);
    connect(&m_preview, &pdfinteraction::PreviewStateModel::stateChanged, this, &EditorHost::bumpPresentation);
}

EditorHost::~EditorHost()
{
    unbindCanvas();
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

QString EditorHost::preflightStateName() const
{
    return preflightStateToString(m_preflight.state());
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

void EditorHost::selectFinding(const QString& findingId)
{
    if (!m_session->revisionSource() || findingId.isEmpty())
    {
        return;
    }

    const QString documentKey = m_session->revisionSource()->documentKey();
    const QString documentRevision = m_session->facade().currentRevision().toString();
    m_preflight.findingsModel()->setSelectedFinding(findingId);
    m_inspector.setFindingSelection(*m_preflight.findingsModel(), findingId, documentRevision);

    pdfinteraction::PreflightController::EvidenceNavigationRequest request;
    if (!m_preflight.navigationFor(findingId, &request))
    {
        bumpPresentation();
        return;
    }

    onPreflightNavigation(request);
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
    m_canvas = qobject_cast<pdfquick::LoupeCanvasItem*>(canvasObject);
    if (m_documentBound)
    {
        bindCanvas();
    }
}

void EditorHost::detachCanvas()
{
    unbindCanvas();
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
    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::stateChanged, this, [this](pdfinteraction::DocumentState state)
            {
                if (state == pdfinteraction::DocumentState::Empty || state == pdfinteraction::DocumentState::Error)
                {
                    onDocumentGone();
                }

                bumpPresentation();
                bumpCommandEpoch();
            });

    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::facetsChanged, this, [this](pdfinteraction::DocumentFacets)
            { bumpPresentation(); });

    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::documentReplaced, this, [this](quint64)
            {
                onDocumentGone();
                onDocumentReady();
                bumpPresentation();
                bumpCommandEpoch();
            });

    connect(&m_session->facade(), &pdfinteraction::DocumentFacade::documentClosed, this, [this](quint64)
            {
                onDocumentGone();
                bumpPresentation();
                bumpCommandEpoch();
            });
}

void EditorHost::connectViewport()
{
    connect(&m_session->viewport(), &pdfinteraction::ViewportController::placementsChanged, this, &EditorHost::bumpPresentation);
    connect(&m_session->viewport(), &pdfinteraction::ViewportController::demandChanged, this, &EditorHost::bumpPresentation);
}

void EditorHost::connectInteraction()
{
    connect(m_session->interaction(),
            &pdfinteraction::InteractionController::dragCompleted,
            this,
            &EditorHost::onDragCompleted);
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
    refreshHitTestSources();
    m_documentBound = true;
    bindCanvas();
    updateCanvasAccessibilitySummary();
    announceDocumentState(tr("Document ready."));
}

void EditorHost::onDocumentGone()
{
    unbindCanvas();
    m_session->clearDocumentView();
    m_preflight.findingsModel()->clear();
    m_inspector.clearSelection();
    m_preview.clear();
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

void EditorHost::syncRevisionModels()
{
    if (!m_session->revisionSource())
    {
        return;
    }

    const QString documentKey = m_session->revisionSource()->documentKey();
    const QString documentRevision = m_session->facade().currentRevision().toString();
    m_preflight.setCurrentRevision(documentKey, documentRevision);
    m_inspector.setCurrentRevision(documentKey, documentRevision);
    m_preview.setCurrentRevision(documentKey, documentRevision);

    if (hasDocument())
    {
        m_preview.setState(documentKey,
                           documentRevision,
                           pdfinteraction::PreviewStateModel::Authority::Approximate,
                           tr("Production preview is approximate until proof mode is active."),
                           tr("The current view uses the standard render path."),
                           QString());
    }
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
    if (!m_session->interaction() || request.page <= 0)
    {
        return;
    }

    m_session->commandBridge().goToPage(request.page - 1);

    pdfinteraction::InteractionTarget target;
    target.kind = pdfinteraction::InteractionTargetKind::Finding;
    target.pageIndex = request.page - 1;
    target.id = request.findingId;
    target.pageBounds = request.bbox;
    m_session->interaction()->selectTarget(target);
    bumpPresentation();
}

void EditorHost::onDragCompleted(pdfinteraction::DragSession session)
{
    Q_UNUSED(session);
    if (m_session->interaction())
    {
        m_session->interaction()->refreshOverlay();
    }
}
