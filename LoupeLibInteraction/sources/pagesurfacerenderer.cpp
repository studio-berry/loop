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

#include "pagesurfacerenderer.h"

#include "pdfdocument.h"
#include "pdfdocumentsession.h"
#include "pdfexception.h"
#include "pdfpainter.h"
#include "pdfprocessingbudget.h"
#include "pdfrenderer.h"
#include "pdftransparencyrenderer.h"
#include "pdfcolorconvertor.h"

#include <QColor>
#include <QImage>
#include <QPainter>

#include <exception>
#include <utility>

namespace pdfinteraction
{

namespace
{

PageSurfaceResult makeTerminal(const PageSurfaceRequest& request, SurfaceTerminalState state, QString typedError)
{
    PageSurfaceResult result;
    result.key = request.key;
    result.token = request.token;
    result.state = state;
    result.typedError = std::move(typedError);
    return result;
}

QImage applySurfaceFeatures(QImage image,
                            const pdf::PDFPage* page,
                            const QTransform& pagePointToDevice,
                            pdf::PDFRenderer::Features features,
                            const pdf::PDFCMS* cms)
{
    pdf::PDFColorConvertor convertor = cms->getColorConvertor();
    pdf::PDFRenderer::applyFeaturesToColorConvertor(features, convertor);
    if (convertor.isActive())
    {
        image = convertor.convert(image);
    }

    if (features.testFlag(pdf::PDFRenderer::ClipToCropBox))
    {
        const QRectF cropBox = page->getCropBox();
        if (cropBox.isValid())
        {
            QImage clipped(image.size(), QImage::Format_ARGB32_Premultiplied);
            clipped.fill(cms->getPaperColor());

            QPainter painter(&clipped);
            QPainterPath path;
            path.addPolygon(pagePointToDevice.map(cropBox));
            painter.setClipPath(path);
            painter.drawImage(0, 0, image);
            painter.end();

            image = std::move(clipped);
        }
    }

    return image;
}

/// Renders one page through PDFTransparencyRenderer with
/// PDFRenderPolicy::forOutputPreview(), for a page the caller has marked (via
/// withAuthoritativeOverprintMarker) as wanting an authoritative,
/// overprint-accurate render instead of the fast approximate one. Mirrors the
/// construction pattern used by PDFInkCoverageProbe and PDFColorInventory,
/// the other production callers of this renderer.
PageSurfaceResult renderAuthoritativeOverprint(const PageSurfaceRequest& request,
                                               const pdf::PDFPage* page,
                                               const pdf::PDFDocument* document,
                                               pdf::PDFDocumentSession* session,
                                               pdf::PDFJobContext& jobContext,
                                               QSize pixelSize,
                                               pdf::PDFRenderer::Features features)
{
    if (jobContext.isCancellationRequested())
    {
        return makeTerminal(request, SurfaceTerminalState::Cancelled, QStringLiteral("page-surface/cancelled"));
    }

    pdf::PDFInkMapper inkMapper(nullptr, document);
    inkMapper.createSpotColors(true);

    pdf::PDFTransparencyRendererSettings settings;
    settings.flags.setFlag(pdf::PDFTransparencyRendererSettings::SeparationSimulation, true);
    settings.flags.setFlag(pdf::PDFTransparencyRendererSettings::SmoothImageTransformation,
                           features.testFlag(pdf::PDFRenderer::SmoothImages));
    settings.renderPolicy = pdf::PDFRenderPolicy::forOutputPreview();

    const QRectF deviceRect(QPointF(0.0, 0.0), QSizeF(pixelSize));
    const QTransform pagePointToDevice = pdf::PDFRenderer::createPagePointToDevicePointMatrix(page, deviceRect, request.key.rotation);

    pdf::PDFTransparencyRenderer renderer(page,
                                          document,
                                          session->getFontCache(),
                                          session->getCMS(),
                                          session->getOptionalContentActivity(),
                                          &inkMapper,
                                          settings,
                                          pagePointToDevice);
    renderer.setOperationControl(jobContext.operationControl());

    renderer.beginPaint(pixelSize);
    renderer.processContents();
    renderer.endPaint();

    if (jobContext.isCancellationRequested())
    {
        return makeTerminal(request, SurfaceTerminalState::Cancelled, QStringLiteral("page-surface/cancelled"));
    }

    const QColor paperColor = session->getCMS()->getPaperColor();
    QImage image = renderer.toImage(false,
                                    true,
                                    pdf::PDFRGB{ static_cast<pdf::PDFColorComponent>(paperColor.redF()),
                                                 static_cast<pdf::PDFColorComponent>(paperColor.greenF()),
                                                 static_cast<pdf::PDFColorComponent>(paperColor.blueF()) });
    if (image.isNull())
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/render-failed"));
    }

    image = applySurfaceFeatures(std::move(image), page, pagePointToDevice, features, session->getCMS());

    image.setDevicePixelRatio(request.key.devicePixelRatio1000 / 1000.0);

    PageSurfaceResult result;
    result.key = request.key;
    result.token = request.token;
    result.state = SurfaceTerminalState::Complete;
    result.pixels = makeSurfaceBuffer(std::move(image));
    result.pixelSize = pixelSize;
    result.byteSize = result.pixels ? result.pixels->byteSize : 0;
    result.diagnostics = renderer.getRenderDiagnostics();

    if (!result.pixels)
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/empty-surface"));
    }

    return result;
}

}   // namespace

PDFSessionPageSurfaceRenderer::PDFSessionPageSurfaceRenderer(pdf::PDFDocumentContext& context) :
    m_context(&context)
{
}

PDFSessionPageSurfaceRenderer::~PDFSessionPageSurfaceRenderer()
{
    detach();
}

void PDFSessionPageSurfaceRenderer::detach()
{
    std::scoped_lock lock(m_mutex);
    m_context = nullptr;
}

void PDFSessionPageSurfaceRenderer::shedPrefetchAndQuality()
{
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_context)
    {
        return;
    }

    if (pdf::PDFDocumentSession* session = m_context->getSession())
    {
        session->shedPrefetchAndQuality();
    }
}

PageSurfaceResult PDFSessionPageSurfaceRenderer::render(const PageSurfaceRequest& request, pdf::PDFJobContext& jobContext)
{
    if (jobContext.isCancellationRequested())
    {
        return makeTerminal(request, SurfaceTerminalState::Cancelled, QStringLiteral("page-surface/cancelled"));
    }

    if (!request.key.isValid())
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/invalid-key"));
    }

    if (!request.key.pageTileBounds.isNull())
    {
        // The key carries tile bounds so a tiled canvas will not need a key
        // change, but sub-rectangle rendering is issue #54's deferred backend
        // work and is deliberately not implemented here. Failing loudly beats
        // quietly returning a whole page for a tile request.
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/tiling-unsupported"));
    }

    std::scoped_lock lock(m_mutex);

    if (!m_context)
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/context-gone"));
    }

    pdf::PDFDocumentSession* session = m_context->getSession();
    if (!session || !session->isValid())
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/session-unavailable"));
    }

    if (!session->isCurrent(request.key.revision))
    {
        // The document moved on between submission and the worker starting. The
        // scheduler's own fence catches most of these; this is the one that
        // closes the window after it.
        return makeTerminal(request, SurfaceTerminalState::Stale, QStringLiteral("page-surface/stale-revision"));
    }

    const pdf::PDFDocument* document = m_context->getDocument();
    if (!document || request.key.pageIndex >= static_cast<int>(document->getCatalog()->getPageCount()))
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/no-such-page"));
    }

    const pdf::PDFPage* page = document->getCatalog()->getPage(static_cast<size_t>(request.key.pageIndex));
    if (!page)
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/no-such-page"));
    }

    const auto features = static_cast<pdf::PDFRenderer::Features>(request.key.featureBits);
    const QSize pixelSize = request.key.targetPixelSize;

    try
    {
        // Charge the tile against the job's own budget rather than counting bytes
        // in a parallel accounting scheme. Pool RasterTile already exists for
        // exactly this work.
        jobContext.processingBudget().chargeRenderPixels(static_cast<std::uint64_t>(pixelSize.width()) * static_cast<std::uint64_t>(pixelSize.height()), QStringLiteral("page surface"));

        if (hasAuthoritativeOverprintMarker(request.key.colorOutputIdentity))
        {
            // The one-page authoritative escalation from issue #49: a full
            // PDFTransparencyRenderer pass instead of the fast QPainter path
            // below, for exactly the page the caller marked.
            return renderAuthoritativeOverprint(request, page, document, session, jobContext, pixelSize, features);
        }

        // Renderer features are session configuration and changing them here would
        // call PDFDocumentContext::invalidateCaches() from a worker, advancing the
        // cache generation and making every in-flight key -- including this one --
        // stale. The compile uses whatever the session is configured with; the
        // requested features apply to the draw, which is where the ColorAdjust
        // modes act. Keeping the two in step is the owner's job.
        const pdf::PDFPrecompiledPage* compiledPage = session->compilePage(static_cast<size_t>(request.key.pageIndex));
        if (!compiledPage || !compiledPage->isValid())
        {
            return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/compile-failed"));
        }

        if (jobContext.isCancellationRequested())
        {
            return makeTerminal(request, SurfaceTerminalState::Cancelled, QStringLiteral("page-surface/cancelled"));
        }

        QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
        {
            return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/allocation-failed"));
        }

        image.setDevicePixelRatio(request.key.devicePixelRatio1000 / 1000.0);
        image.fill(compiledPage->getPaperColor());

        {
            QPainter painter(&image);
            const QRectF deviceRect(QPointF(0.0, 0.0), QSizeF(pixelSize));
            const QTransform matrix = pdf::PDFRenderer::createPagePointToDevicePointMatrix(page, deviceRect, request.key.rotation);
            compiledPage->draw(&painter, page->getCropBox(), matrix, features, 1.0);
        }

        // The compiled page pointer stays valid only until the next compilePage()
        // on this session, and the drawing above is the last use of it while the
        // lock is still held.

        if (jobContext.isCancellationRequested())
        {
            return makeTerminal(request, SurfaceTerminalState::Cancelled, QStringLiteral("page-surface/cancelled"));
        }

        PageSurfaceResult result;
        result.key = request.key;
        result.token = request.token;
        result.state = SurfaceTerminalState::Complete;
        result.pixels = makeSurfaceBuffer(std::move(image));
        result.pixelSize = pixelSize;
        result.byteSize = result.pixels ? result.pixels->byteSize : 0;
        result.diagnostics = pdf::PDFRenderDiagnostics::forApproximateOverprint(compiledPage->containsOverprint());

        if (!result.pixels)
        {
            return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/empty-surface"));
        }

        return result;
    }
    catch (const pdf::PDFBudgetExceededException&)
    {
        // A budget failure is incomplete work, never a pass and never a plain
        // error: docs/RESOURCE_BUDGETS.md requires the distinct reason to survive.
        return makeTerminal(request, SurfaceTerminalState::BudgetExhausted, QStringLiteral("page-surface/budget-exhausted"));
    }
    catch (const pdf::PDFException&)
    {
        // The exception message can quote document content, so it is not
        // forwarded to a presentation host.
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/render-failed"));
    }
    catch (const std::exception&)
    {
        // pdf::PDFRendererException is a separate hierarchy, and this contract
        // says a renderer never throws.
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/render-failed"));
    }
    catch (...)
    {
        return makeTerminal(request, SurfaceTerminalState::Failed, QStringLiteral("page-surface/render-failed"));
    }
}

}   // namespace pdfinteraction
