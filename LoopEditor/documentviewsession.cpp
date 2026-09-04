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

#include <cstdio>

namespace
{

bool headlessQpaPlatformRequested()
{
    const QByteArray requested = qgetenv("QT_QPA_PLATFORM");
    if (requested.isEmpty())
    {
        return QGuiApplication::platformName() == QLatin1String("offscreen");
    }

    for (const QByteArray& part : requested.split(','))
    {
        if (part.trimmed() == "offscreen")
        {
            return true;
        }
    }

    return false;
}

void applyInitialViewportScreenMetrics(pdfinteraction::ViewportController& viewport)
{
#if defined(Q_OS_WIN)
    // Windows headless packaging smoke can fault inside primaryScreen() metrics even
    // when QT_QPA_PLATFORM=offscreen. ViewportController is designed for injected DPI.
    Q_UNUSED(headlessQpaPlatformRequested);
    viewport.setPixelPerMM(96.0 / 25.4);
    viewport.setDevicePixelRatio(1.0);
#else
    if (headlessQpaPlatformRequested())
    {
        viewport.setPixelPerMM(96.0 / 25.4);
        viewport.setDevicePixelRatio(1.0);
    }
    else if (QScreen* screen = QGuiApplication::primaryScreen())
    {
        viewport.setPixelPerMM(screen->physicalDotsPerInchX() / 25.4);
        viewport.setDevicePixelRatio(screen->devicePixelRatio());
    }
#endif
}

void logDocumentViewSessionInit(const char* stage)
{
    fprintf(stderr, "loop-editor documentviewsession init %s\n", stage);
    fflush(stderr);
}

std::unique_ptr<pdf::PDFJobScheduler> makeDocumentViewScheduler()
{
    logDocumentViewSessionInit("scheduler_begin");
    auto scheduler = std::make_unique<pdf::PDFJobScheduler>();
    logDocumentViewSessionInit("scheduler_end");
    return scheduler;
}

std::unique_ptr<pdfinteraction::DocumentFacade> makeDocumentFacade(pdf::PDFDocumentContext& context,
                                                                   pdfinteraction::PDFJobSchedulerSubmitter& submitter,
                                                                   pdfinteraction::PDFReaderDocumentLoader& loader,
                                                                   pdfinteraction::PDFDocumentFileWriter& writer,
                                                                   pdfinteraction::CommandCatalog& catalog,
                                                                   QObject* parent)
{
    logDocumentViewSessionInit("facade_begin");
    auto facade = std::make_unique<pdfinteraction::DocumentFacade>(context,
                                                                     submitter,
                                                                     loader,
                                                                     writer,
                                                                     catalog,
                                                                     parent);
    logDocumentViewSessionInit("facade_end");
    return facade;
}

}   // namespace

DocumentViewSession::DocumentViewSession(QObject* parent) :
    QObject(parent),
    m_scheduler(makeDocumentViewScheduler()),
    m_submitter(*m_scheduler),
    m_context(([]() {
        logDocumentViewSessionInit("context_begin");
        pdf::PDFDocumentContext context(nullptr);
        logDocumentViewSessionInit("context_end");
        return context;
    }())),
    m_facade(makeDocumentFacade(m_context, m_submitter, m_loader, m_writer, m_catalog, this)),
    m_renderer((logDocumentViewSessionInit("renderer"), m_context)),
    m_commandBridge((logDocumentViewSessionInit("command_bridge"), m_catalog), *m_facade, m_viewport, this),
    m_pageBoxSource(&m_context),
    m_cacheLimit(pdf::PDFPageCacheBudget::total(DefaultCacheLimit))
{
    logDocumentViewSessionInit("body_begin");
    m_revisionSource = std::make_unique<pdfinteraction::PDFDocumentContextSource>(&m_context, this);
    m_hitTest = std::make_unique<pdfinteraction::HitTestDispatcher>();
    fprintf(stderr, "loop-editor documentviewsession before surfaces\n");
    fflush(stderr);
    m_surfaces = std::make_unique<pdfinteraction::PageSurfaceCoordinator>(*m_revisionSource,
                                                                          m_submitter,
                                                                          m_renderer,
                                                                          m_viewport,
                                                                          pdfinteraction::PageSurfaceBounds::conservativeDefaults(),
                                                                          this);
    m_surfaces->setPageCacheBudget(m_context.getSharedPageCacheBudget());
    setCacheLimit(DefaultCacheLimit);
    fprintf(stderr, "loop-editor documentviewsession after surfaces\n");
    fflush(stderr);
    m_overlays = std::make_unique<pdfinteraction::OverlayBuilder>(m_viewport, m_surfaces->renderSettings());
    m_interaction = std::make_unique<pdfinteraction::InteractionController>(*m_revisionSource,
                                                                            m_viewport,
                                                                            *m_hitTest,
                                                                            *m_overlays,
                                                                            this);

    m_viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);
    applyInitialViewportScreenMetrics(m_viewport);

    m_commandBridge.setCoordinator(m_surfaces.get());
    fprintf(stderr, "loop-editor documentviewsession constructed\n");
    fflush(stderr);
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
