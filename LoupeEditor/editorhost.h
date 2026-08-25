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

#ifndef EDITORHOST_H
#define EDITORHOST_H

#include "interactionstate.h"

#include "commandcatalog.h"
#include "documentfacade.h"
#include "documentloader.h"
#include "hittestsource.h"
#include "inspectormodel.h"
#include "jobsubmitter.h"
#include "pagesurfacerenderer.h"
#include "preflightcontroller.h"
#include "preflightoverlaybridge.h"
#include "previewstatemodel.h"
#include "viewportcommandbridge.h"
#include "viewportcontroller.h"

#include "focusrestoration.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"

#include <QObject>
#include <QPointer>
#include <QStyleHints>
#include <QVariantMap>

#include <memory>

namespace pdfinteraction
{
class HitTestDispatcher;
class InteractionController;
class OverlayBuilder;
class PageSurfaceCoordinator;
class PDFDocumentContextSource;
class PDFDocumentPageGeometrySource;
}   // namespace pdfinteraction

namespace pdfquick
{
class LoupeCanvasItem;
}   // namespace pdfquick

/// C++ presentation host for the packaged Loupe.Quick shell (P4-S7).
///
/// Owns the one document context, scheduler adapter, command catalog, lifecycle
/// facade, viewport, surfaces, and interaction stack. QML sees presentation
/// properties and catalog invoke() only; Core objects never cross the boundary.
class EditorHost final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString documentState READ documentState NOTIFY presentationChanged)
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY presentationChanged)
    Q_PROPERTY(QString displayTitle READ displayTitle NOTIFY presentationChanged)
    Q_PROPERTY(QString typedError READ typedError NOTIFY presentationChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY presentationChanged)
    Q_PROPERTY(int currentPage READ currentPage NOTIFY presentationChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY presentationChanged)
    Q_PROPERTY(int rotationDegrees READ rotationDegrees NOTIFY presentationChanged)
    Q_PROPERTY(bool incomplete READ incomplete NOTIFY presentationChanged)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY presentationChanged)
    Q_PROPERTY(bool unsupported READ unsupported NOTIFY presentationChanged)
    Q_PROPERTY(int commandEpoch READ commandEpoch NOTIFY commandEpochChanged)
    Q_PROPERTY(QObject* preflight READ preflight CONSTANT)
    Q_PROPERTY(QObject* inspector READ inspector CONSTANT)
    Q_PROPERTY(QObject* preview READ preview CONSTANT)
    Q_PROPERTY(QObject* focusRestoration READ focusRestoration CONSTANT)
    Q_PROPERTY(QString preflightStateName READ preflightStateName NOTIFY presentationChanged)
    Q_PROPERTY(QString previewSummary READ previewSummary NOTIFY presentationChanged)
    Q_PROPERTY(QString inspectorTitle READ inspectorTitle NOTIFY presentationChanged)
    Q_PROPERTY(bool preferReducedMotion READ preferReducedMotion NOTIFY presentationChanged)
    Q_PROPERTY(bool highContrast READ highContrast NOTIFY presentationChanged)

public:
    explicit EditorHost(QObject* parent = nullptr);
    ~EditorHost() override;

    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    QString documentState() const;
    bool hasDocument() const;
    QString displayTitle() const;
    QString typedError() const;
    int pageCount() const;
    int currentPage() const;
    qreal zoom() const;
    int rotationDegrees() const;
    bool incomplete() const;
    bool cancelled() const;
    bool unsupported() const;
    int commandEpoch() const noexcept { return m_commandEpoch; }

    QObject* preflight();
    QObject* inspector();
    QObject* preview();
    FocusRestoration* focusRestoration() { return &m_focusRestoration; }

    QString preflightStateName() const;
    QString previewSummary() const;
    QString inspectorTitle() const;
    bool preferReducedMotion() const;
    bool highContrast() const;

    Q_INVOKABLE void selectFinding(const QString& findingId);
    Q_INVOKABLE void announceDocumentState(const QString& message);

    Q_INVOKABLE QVariantList commandDescriptors() const;
    Q_INVOKABLE bool isCommandEnabled(const QString& commandId) const;
    Q_INVOKABLE quint64 invokeCommand(const QString& commandId, const QVariantMap& parameters = {});
    Q_INVOKABLE bool cancelCommand(quint64 invocationId);

    /// QML FileDialog passes a file URL; paths stay in C++.
    Q_INVOKABLE void openFileUrl(const QUrl& url);
    Q_INVOKABLE void saveAsFileUrl(const QUrl& url);
    Q_INVOKABLE void reopenDocument();
    Q_INVOKABLE void cancelPendingOperation();

    /// Observed LoupeCanvas item from CanvasPane.qml. Binding lifetime is owned
    /// here: replace/close unbinds before geometry is dropped.
    Q_INVOKABLE void attachCanvas(QObject* canvasObject);
    Q_INVOKABLE void detachCanvas();

    /// Legacy geometry hook for headless tests without a LoupeCanvas item.
    Q_INVOKABLE void setViewportGeometry(qreal pixelPerMM, qreal devicePixelRatio, int widthPx, int heightPx);

    /// Opens a positional CLI path after the shell is loaded.
    Q_INVOKABLE void openInitialPath(const QString& path);
    Q_INVOKABLE QString shortcutForCommand(const QString& commandId) const;

signals:
    void presentationChanged();
    void commandEpochChanged();

private:
    void connectFacade();
    void connectViewport();
    void connectCatalog();
    void connectInteraction();
    void registerShellHandlers();
    void refreshHitTestSources();
    void bumpPresentation();
    void bumpCommandEpoch();

    void onDocumentReady();
    void onDocumentGone();
    void bindCanvas();
    void unbindCanvas();
    void syncRevisionModels();
    void updateCanvasAccessibilitySummary();
    void onPreflightNavigation(pdfinteraction::PreflightController::EvidenceNavigationRequest request);
    void onDragCompleted(pdfinteraction::DragSession session);

    pdf::PDFJobScheduler m_scheduler;
    pdfinteraction::PDFJobSchedulerSubmitter m_submitter;
    pdfinteraction::CommandCatalog m_catalog;
    pdf::PDFDocumentContext m_context;
    pdfinteraction::PDFReaderDocumentLoader m_loader;
    pdfinteraction::PDFDocumentFileWriter m_writer;
    pdfinteraction::DocumentFacade m_facade;
    std::unique_ptr<pdfinteraction::PDFDocumentContextSource> m_revisionSource;
    pdfinteraction::ViewportController m_viewport;
    std::unique_ptr<pdfinteraction::PDFDocumentPageGeometrySource> m_geometry;
    pdfinteraction::PDFSessionPageSurfaceRenderer m_renderer;
    std::unique_ptr<pdfinteraction::PageSurfaceCoordinator> m_surfaces;
    std::unique_ptr<pdfinteraction::HitTestDispatcher> m_hitTest;
    std::unique_ptr<pdfinteraction::OverlayBuilder> m_overlays;
    std::unique_ptr<pdfinteraction::InteractionController> m_interaction;
    pdfinteraction::ViewportCommandBridge m_commandBridge;
    pdfinteraction::PreflightController m_preflight;
    pdfinteraction::PreflightOverlayBridge m_preflightOverlayBridge;
    pdfinteraction::InspectorModel m_inspector;
    pdfinteraction::PreviewStateModel m_preview;
    FocusRestoration m_focusRestoration;
    pdfinteraction::PageBoxHitTestSource m_pageBoxSource;
    pdfinteraction::FindingListHitTestSource m_findingsHitTest;

    QPointer<pdfquick::LoupeCanvasItem> m_canvas;
    int m_commandEpoch = 0;
    bool m_documentBound = false;
};

#endif   // EDITORHOST_H
