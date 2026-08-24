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

#include "documentcontextsource.h"
#include "hittestsource.h"
#include "interactioncontroller.h"
#include "loupecanvasitem.h"
#include "overlaybuilder.h"
#include "pagesurfacecoordinator.h"

#include "pdfpage.h"

#include <QGuiApplication>
#include <QKeySequence>
#include <QMetaEnum>
#include <QScreen>
#include <QUrl>

#include <QCoreApplication>

namespace
{

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

QVariantMap descriptorToVariant(const pdfinteraction::CommandDescriptor& descriptor)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("id"), descriptor.id);
    entry.insert(QStringLiteral("labelKey"), descriptor.labelKey);
    entry.insert(QStringLiteral("implemented"), descriptor.isImplemented());

    QVariantMap shortcut;
    shortcut.insert(QStringLiteral("standardKey"), descriptor.shortcut.standardKey);
    shortcut.insert(QStringLiteral("sequence"), descriptor.shortcut.sequence);
    entry.insert(QStringLiteral("shortcut"), shortcut);
    return entry;
}

}   // namespace

EditorHost::EditorHost(QObject* parent) :
    QObject(parent),
    m_context(nullptr),
    m_submitter(m_scheduler),
    m_facade(m_context, m_submitter, m_loader, m_writer, m_catalog, this),
    m_renderer(m_context),
    m_commandBridge(m_catalog, m_facade, m_viewport, this)
{
    m_revisionSource = std::make_unique<pdfinteraction::PDFDocumentContextSource>(&m_context, this);
    m_hitTest = std::make_unique<pdfinteraction::HitTestDispatcher>();
    m_overlays = std::make_unique<pdfinteraction::OverlayBuilder>(m_viewport);
    m_interaction = std::make_unique<pdfinteraction::InteractionController>(*m_revisionSource,
                                                                          m_viewport,
                                                                          *m_hitTest,
                                                                          *m_overlays,
                                                                          this);
    m_surfaces = std::make_unique<pdfinteraction::PageSurfaceCoordinator>(*m_revisionSource,
                                                                          m_submitter,
                                                                          m_renderer,
                                                                          m_viewport,
                                                                          pdfinteraction::PageSurfaceBounds::conservativeDefaults(),
                                                                          this);

    m_viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);

    if (QScreen* screen = QGuiApplication::primaryScreen())
    {
        m_viewport.setPixelPerMM(screen->physicalDotsPerInchX() / 25.4);
        m_viewport.setDevicePixelRatio(screen->devicePixelRatio());
    }

    m_commandBridge.setCoordinator(m_surfaces.get());

    connectFacade();
    connectViewport();
    connectCatalog();
}

EditorHost::~EditorHost()
{
    unbindCanvas();
    m_renderer.detach();
}

QString EditorHost::documentState() const
{
    return QString::fromLatin1(pdfinteraction::getDocumentStateName(m_facade.state()));
}

bool EditorHost::hasDocument() const
{
    return m_facade.state() == pdfinteraction::DocumentState::Ready;
}

QString EditorHost::displayTitle() const
{
    return m_facade.source().displayLabel();
}

QString EditorHost::typedError() const
{
    return m_facade.typedError();
}

int EditorHost::pageCount() const
{
    return m_viewport.pageCount();
}

int EditorHost::currentPage() const
{
    return m_viewport.currentPage();
}

qreal EditorHost::zoom() const
{
    return m_viewport.zoom();
}

int EditorHost::rotationDegrees() const
{
    return rotationToDegrees(m_viewport.rotation());
}

bool EditorHost::incomplete() const
{
    return m_facade.facets().testFlag(pdfinteraction::DocumentFacet::Incomplete);
}

bool EditorHost::cancelled() const
{
    return m_facade.facets().testFlag(pdfinteraction::DocumentFacet::Cancelled);
}

bool EditorHost::unsupported() const
{
    return m_facade.facets().testFlag(pdfinteraction::DocumentFacet::Unsupported);
}

QVariantList EditorHost::commandDescriptors() const
{
    QVariantList descriptors;
    descriptors.reserve(m_catalog.descriptors().size());
    for (const pdfinteraction::CommandDescriptor& descriptor : m_catalog.descriptors())
    {
        descriptors.append(descriptorToVariant(descriptor));
    }
    return descriptors;
}

bool EditorHost::isCommandEnabled(const QString& commandId) const
{
    return m_catalog.isEnabled(commandId);
}

quint64 EditorHost::invokeCommand(const QString& commandId, const QVariantMap& parameters)
{
    const pdfinteraction::CommandInvocationId invocation = m_catalog.invoke(commandId, parameters);
    if (invocation != pdfinteraction::InvalidCommandInvocation)
    {
        bumpCommandEpoch();
    }
    return invocation;
}

bool EditorHost::cancelCommand(quint64 invocationId)
{
    return m_catalog.cancelInvocation(invocationId);
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
    if (m_facade.reopen() != pdfinteraction::InvalidCommandInvocation)
    {
        bumpCommandEpoch();
    }
}

void EditorHost::cancelPendingOperation()
{
    if (m_facade.cancelPendingOperation())
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
    if (pixelPerMM > 0.0)
    {
        m_viewport.setPixelPerMM(pixelPerMM);
    }

    if (devicePixelRatio > 0.0)
    {
        m_viewport.setDevicePixelRatio(devicePixelRatio);
    }

    if (widthPx > 0 && heightPx > 0)
    {
        m_viewport.setViewportSizePx(QSize(widthPx, heightPx));
        if (m_documentBound)
        {
            m_surfaces->requestSurfaces();
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
    const pdfinteraction::CommandDescriptor* descriptor = m_catalog.descriptor(commandId);
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
    connect(&m_facade, &pdfinteraction::DocumentFacade::stateChanged, this, [this](pdfinteraction::DocumentState state)
            {
                if (state == pdfinteraction::DocumentState::Ready)
                {
                    onDocumentReady();
                }
                else if (state == pdfinteraction::DocumentState::Empty || state == pdfinteraction::DocumentState::Error)
                {
                    onDocumentGone();
                }

                bumpPresentation();
                bumpCommandEpoch();
            });

    connect(&m_facade, &pdfinteraction::DocumentFacade::facetsChanged, this, [this](pdfinteraction::DocumentFacets)
            { bumpPresentation(); });

    connect(&m_facade, &pdfinteraction::DocumentFacade::documentReplaced, this, [this](quint64)
            {
                onDocumentGone();
                onDocumentReady();
                bumpPresentation();
                bumpCommandEpoch();
            });

    connect(&m_facade, &pdfinteraction::DocumentFacade::documentClosed, this, [this](quint64)
            {
                onDocumentGone();
                bumpPresentation();
                bumpCommandEpoch();
            });
}

void EditorHost::connectViewport()
{
    connect(&m_viewport, &pdfinteraction::ViewportController::placementsChanged, this, &EditorHost::bumpPresentation);
    connect(&m_viewport, &pdfinteraction::ViewportController::demandChanged, this, [this](quint64)
            {
                if (m_documentBound)
                {
                    m_surfaces->requestSurfaces();
                }
                bumpPresentation();
            });
}

void EditorHost::connectCatalog()
{
    connect(&m_catalog, &pdfinteraction::CommandCatalog::availabilityChanged, this, &EditorHost::bumpCommandEpoch);
}

void EditorHost::bumpPresentation()
{
    Q_EMIT presentationChanged();
}

void EditorHost::bumpCommandEpoch()
{
    ++m_commandEpoch;
    Q_EMIT commandEpochChanged();
}

void EditorHost::onDocumentReady()
{
    m_geometry = std::make_unique<pdfinteraction::PDFDocumentPageGeometrySource>(&m_context);
    m_viewport.setGeometrySource(m_geometry.get());
    m_viewport.invalidateLayout();

    if (m_revisionSource)
    {
        m_surfaces->setDocumentKey(m_revisionSource->documentKey());
    }

    m_surfaces->invalidate(m_facade.currentRevision());
    m_surfaces->requestSurfaces();

    m_documentBound = true;
    bindCanvas();
}

void EditorHost::onDocumentGone()
{
    unbindCanvas();
    m_viewport.setGeometrySource(nullptr);
    m_geometry.reset();
    m_surfaces->invalidate(m_facade.currentRevision());
    m_documentBound = false;
}

void EditorHost::bindCanvas()
{
    if (!m_canvas || !m_documentBound)
    {
        return;
    }

    m_canvas->bind(&m_viewport, m_interaction.get(), m_surfaces.get());
}

void EditorHost::unbindCanvas()
{
    if (!m_canvas)
    {
        return;
    }

    m_canvas->bind(nullptr, nullptr, nullptr);
}
