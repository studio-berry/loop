// MIT License
#include "documentviewsession.h"

#include "documentcontextsource.h"
#include "interactioncontroller.h"
#include "overlaybuilder.h"
#include "pagesurfacecoordinator.h"
#include "pdfdocumentsession.h"
#include "pdfpagecachebudget.h"

#include <QGuiApplication>
#include <QScreen>

DocumentViewSession::DocumentViewSession(QObject* parent) :
    QObject(parent),
    m_scheduler(std::make_unique<pdf::PDFJobScheduler>()),
    m_submitter(*m_scheduler),
    m_context(nullptr),
    m_facade(std::make_unique<pdfinteraction::DocumentFacade>(m_context,
                                                              m_submitter,
                                                              m_loader,
                                                              m_writer,
                                                              m_catalog,
                                                              this)),
    m_renderer(m_context),
    m_commandBridge(m_catalog, *m_facade, m_viewport, this),
    m_pageBoxSource(&m_context),
    m_cacheLimit(pdf::PDFPageCacheBudget::total(DefaultCacheLimit))
{
    m_revisionSource = std::make_unique<pdfinteraction::PDFDocumentContextSource>(&m_context, this);
    m_hitTest = std::make_unique<pdfinteraction::HitTestDispatcher>();
    m_surfaces = std::make_unique<pdfinteraction::PageSurfaceCoordinator>(*m_revisionSource,
                                                                          m_submitter,
                                                                          m_renderer,
                                                                          m_viewport,
                                                                          pdfinteraction::PageSurfaceBounds::conservativeDefaults(),
                                                                          this);
    m_surfaces->setPageCacheBudget(m_context.getSharedPageCacheBudget());
    setCacheLimit(DefaultCacheLimit);
    m_overlays = std::make_unique<pdfinteraction::OverlayBuilder>(m_viewport, m_surfaces->renderSettings());
    m_interaction = std::make_unique<pdfinteraction::InteractionController>(*m_revisionSource,
                                                                            m_viewport,
                                                                            *m_hitTest,
                                                                            *m_overlays,
                                                                            this);

    m_viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);
    if (QScreen* screen = QGuiApplication::primaryScreen())
    {
        m_viewport.setPixelPerMM(screen->physicalDotsPerInchX() / 25.4);
        m_viewport.setDevicePixelRatio(screen->devicePixelRatio());
    }

    m_commandBridge.setCoordinator(m_surfaces.get());
}

DocumentViewSession::~DocumentViewSession()
{
    // Coordinators detach their completion relays and cancel admitted work.
    // Destroy them before joining the scheduler so no completion can address a
    // session object during teardown. The captured adapters remain alive until
    // after reset() has joined every worker.
    m_commandBridge.setCoordinator(nullptr);
    m_interaction.reset();
    m_overlays.reset();
    m_surfaces.reset();
    m_facade.reset();
    m_scheduler.reset();
    m_renderer.detach();
}

void DocumentViewSession::prepareDocumentView()
{
    m_interaction->invalidate();
    m_geometry = std::make_unique<pdfinteraction::PDFDocumentPageGeometrySource>(&m_context);
    m_viewport.setGeometrySource(m_geometry.get());
    m_viewport.invalidateLayout();
    m_surfaces->setDocumentKey(m_revisionSource->documentKey());

    if (pdf::PDFDocumentSession* session = m_context.getSession())
    {
        m_surfaces->setResourceBudget(session->getSharedResourceBudget());
    }

    // The context keeps the shared page-cache budget across document-session
    // replacement; the resource envelope is refreshed from the new session.
    m_surfaces->refreshPageCacheBudget();
    m_surfaces->invalidate(m_facade->currentRevision());
    m_surfaces->requestSurfaces();
}

void DocumentViewSession::clearDocumentView()
{
    m_interaction->invalidate();
    m_viewport.setGeometrySource(nullptr);
    m_geometry.reset();
    m_surfaces->invalidate(m_facade->currentRevision());
}

void DocumentViewSession::setSurfaceRenderFeatures(pdf::PDFRenderer::Features features)
{
    pdfinteraction::PageSurfaceRenderSettings settings = m_surfaces->renderSettings();
    settings.features = features;
    // OverlayBuilder reads denyExtraGraphics() straight out of the shared
    // RenderPresentationPolicy this call just mutated, so there is nothing to push.
    m_surfaces->setRenderSettings(settings);
}

void DocumentViewSession::setCacheLimit(qsizetype totalBytes)
{
    const qsizetype normalized = pdf::PDFPageCacheBudget::total(totalBytes);
    m_cacheLimit = normalized;
    if (pdf::PDFDocumentSession* session = m_context.getSession())
    {
        if (m_surfaces)
        {
            m_surfaces->setResourceBudget(session->getSharedResourceBudget());
        }
    }
    // Route through the renderer so the eviction inside setCacheLimit cannot
    // race a worker thread that holds m_renderer.m_mutex and is using a
    // compilePage pointer.
    m_renderer.setCacheLimit(normalized);
    if (m_surfaces)
    {
        m_surfaces->refreshPageCacheBudget();
    }
}

qsizetype DocumentViewSession::cacheLimit() const noexcept
{
    if (const pdf::PDFDocumentSession* session = m_context.getSession())
    {
        return session->cacheLimit();
    }
    return m_cacheLimit;
}
