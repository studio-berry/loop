// MIT License
#ifndef DOCUMENTVIEWSESSION_H
#define DOCUMENTVIEWSESSION_H

#include "commandcatalog.h"
#include "documentfacade.h"
#include "documentloader.h"
#include "hittestsource.h"
#include "jobsubmitter.h"
#include "pagesurfacerenderer.h"
#include "viewportcommandbridge.h"
#include "viewportcontroller.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfrenderer.h"

#include <QObject>
#include <QtGlobal>
#include <memory>

namespace pdfinteraction
{
class HitTestDispatcher;
class InteractionController;
class OverlayBuilder;
class PageSurfaceCoordinator;
class PDFDocumentContextSource;
class PDFDocumentPageGeometrySource;
}

/// Owns the document-bound view machinery used by EditorHost.
///
/// The session is the single owner of document lifecycle, viewport state,
/// revision fencing, rendering, surfaces, and interaction. EditorHost remains
/// a presentation adapter and does not become a second semantic owner.
class DocumentViewSession final : public QObject
{
    Q_OBJECT

public:
    explicit DocumentViewSession(QObject* parent = nullptr);
    ~DocumentViewSession() override;

    DocumentViewSession(const DocumentViewSession&) = delete;
    DocumentViewSession& operator=(const DocumentViewSession&) = delete;

    pdf::PDFJobScheduler& scheduler() noexcept { return *m_scheduler; }
    pdfinteraction::PDFJobSchedulerSubmitter& submitter() noexcept { return m_submitter; }
    pdfinteraction::CommandCatalog& catalog() noexcept { return m_catalog; }
    pdf::PDFDocumentContext& context() noexcept { return m_context; }
    pdfinteraction::PDFReaderDocumentLoader& loader() noexcept { return m_loader; }
    pdfinteraction::PDFDocumentFileWriter& writer() noexcept { return m_writer; }
    pdfinteraction::DocumentFacade& facade() noexcept { return *m_facade; }
    const pdfinteraction::DocumentFacade& facade() const noexcept { return *m_facade; }
    pdfinteraction::PDFDocumentContextSource* revisionSource() const noexcept { return m_revisionSource.get(); }
    pdfinteraction::ViewportController& viewport() noexcept { return m_viewport; }
    const pdfinteraction::ViewportController& viewport() const noexcept { return m_viewport; }
    pdfinteraction::PDFSessionPageSurfaceRenderer& renderer() noexcept { return m_renderer; }
    pdfinteraction::PageSurfaceCoordinator* surfaces() const noexcept { return m_surfaces.get(); }
    pdfinteraction::HitTestDispatcher* hitTest() const noexcept { return m_hitTest.get(); }
    pdfinteraction::OverlayBuilder* overlays() const noexcept { return m_overlays.get(); }
    pdfinteraction::InteractionController* interaction() const noexcept { return m_interaction.get(); }
    pdfinteraction::ViewportCommandBridge& commandBridge() noexcept { return m_commandBridge; }
    pdfinteraction::PageBoxHitTestSource& pageBoxSource() noexcept { return m_pageBoxSource; }

    /// Total resident budget for compiled pages and admitted page surfaces.
    /// The compiled/surface shares are derived by PDFPageCacheBudget.
    static constexpr qsizetype DefaultCacheLimit = 256ll * 1024 * 1024;
    void setCacheLimit(qsizetype totalBytes);
    qsizetype cacheLimit() const noexcept;

    void prepareDocumentView();
    void clearDocumentView();
    void setSurfaceRenderFeatures(pdf::PDFRenderer::Features features);

private:
    // Reset explicitly in the destructor so every worker is joined while the
    // loader, writer, and renderer captured by jobs are still alive.
    std::unique_ptr<pdf::PDFJobScheduler> m_scheduler;
    pdfinteraction::PDFJobSchedulerSubmitter m_submitter;
    pdfinteraction::CommandCatalog m_catalog;
    pdf::PDFDocumentContext m_context;
    pdfinteraction::PDFReaderDocumentLoader m_loader;
    pdfinteraction::PDFDocumentFileWriter m_writer;
    std::unique_ptr<pdfinteraction::DocumentFacade> m_facade;
    std::unique_ptr<pdfinteraction::PDFDocumentContextSource> m_revisionSource;
    pdfinteraction::ViewportController m_viewport;
    std::unique_ptr<pdfinteraction::PDFDocumentPageGeometrySource> m_geometry;
    pdfinteraction::PDFSessionPageSurfaceRenderer m_renderer;
    std::unique_ptr<pdfinteraction::PageSurfaceCoordinator> m_surfaces;
    std::unique_ptr<pdfinteraction::HitTestDispatcher> m_hitTest;
    std::unique_ptr<pdfinteraction::OverlayBuilder> m_overlays;
    std::unique_ptr<pdfinteraction::InteractionController> m_interaction;
    pdfinteraction::ViewportCommandBridge m_commandBridge;
    pdfinteraction::PageBoxHitTestSource m_pageBoxSource;
    qsizetype m_cacheLimit = DefaultCacheLimit;   // total authority (partitioned via PDFPageCacheBudget)
};

#endif
