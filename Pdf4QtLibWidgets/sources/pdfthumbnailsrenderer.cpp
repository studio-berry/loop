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

#include "pdfthumbnailsrenderer.h"

#include "pdfannotation.h"
#include "pdfcms.h"
#include "pdfconstants.h"
#include "pdfexecutionpolicy.h"
#include "pdffont.h"
#include "pdfjobscheduler.h"
#include "pdfoptionalcontent.h"
#include "pdfpainter.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace pdf
{

struct PDFThumbnailsRenderer::DocumentRenderContext
{
    const PDFDocument* document = nullptr;
    std::unique_ptr<PDFFontCache> fontCache;
    std::unique_ptr<PDFCMSManager> cmsManager;
    std::unique_ptr<PDFOptionalContentActivity> optionalContentActivity;
    PDFMeshQualitySettings meshQualitySettings;
    PDFRevisionIdentity revision;
    PDFRenderer::Features features = PDFRenderer::getDefaultFeatures();
    std::unique_ptr<PDFRasterizerPool> rasterizerPool;
};

namespace
{

/// Disables font cache shrinking while worker threads render, so the shared
/// font cache is not shrunk concurrently from another thread.
class FontCacheShrinkGuard
{
public:
    FontCacheShrinkGuard(const void* source, PDFFontCache* fontCache) :
        m_source(source),
        m_fontCache(fontCache)
    {
        if (m_fontCache)
        {
            m_fontCache->setCacheShrinkEnabled(m_source, false);
        }
    }

    ~FontCacheShrinkGuard()
    {
        if (m_fontCache)
        {
            m_fontCache->setCacheShrinkEnabled(m_source, true);
        }
    }

private:
    const void* m_source;
    PDFFontCache* m_fontCache;
};

} // namespace

PDFThumbnailsRenderer::PDFThumbnailsRenderer(const PDFDocument* document, QObject* parent) :
    QObject(parent),
    m_document(document),
    m_pageImageCache(THUMBNAIL_CACHE_LIMIT_BYTES),
    m_revision(PDFRevisionIdentity { PDFDocumentIdentity::fromDocument(document), 0, 0, QString() })
{
    connect(&PDFJobScheduler::global(), &PDFJobScheduler::jobFinished, this, [this](const PDFJobSnapshot& snapshot)
    {
        if (snapshot.jobId == m_renderJobId)
        {
            onRenderFinished();
        }
    });
}

PDFThumbnailsRenderer::~PDFThumbnailsRenderer()
{
    waitForFinished();
}

void PDFThumbnailsRenderer::setDocument(const PDFDocument* document)
{
    // Wait for the currently running batch, otherwise the context (and thereby
    // the old document) would be released while still being rendered.
    waitForCurrentRender();

    {
        QMutexLocker guard(&m_contextMutex);
        QObject::disconnect(m_contextConnection);
        m_documentContext = nullptr;
        m_document = document;
        m_context.reset();
    }

    m_pageImageCache.clear();
    m_pendingKeys.clear();
    m_requestQueue.clear();
    m_keysByPage.clear();
    ++m_renderEpoch;
    m_revision = PDFRevisionIdentity { PDFDocumentIdentity::fromDocument(document), 0, m_renderEpoch, QString() };
}

void PDFThumbnailsRenderer::setDocumentContext(PDFDocumentContext* context)
{
    waitForCurrentRender();

    {
        QMutexLocker guard(&m_contextMutex);
        QObject::disconnect(m_contextConnection);
        m_documentContext = context;
        m_document = context ? context->getDocument() : nullptr;
        m_context.reset();
        if (context)
        {
            m_revision = context->getRevision();
            m_contextConnection = connect(context, &PDFDocumentContext::revisionChanged, this, &PDFThumbnailsRenderer::onContextRevisionChanged, Qt::UniqueConnection);
        }
    }

    m_pageImageCache.clear();
    m_pendingKeys.clear();
    m_requestQueue.clear();
    m_keysByPage.clear();
    ++m_renderEpoch;
}

void PDFThumbnailsRenderer::waitForFinished()
{
    waitForCurrentRender();
}

void PDFThumbnailsRenderer::clear()
{
    m_pageImageCache.clear();
    m_pendingKeys.clear();
    m_requestQueue.clear();
    m_keysByPage.clear();
    ++m_renderEpoch;
    if (!m_documentContext)
    {
        m_revision.cacheGeneration = m_renderEpoch;
    }
}

QImage PDFThumbnailsRenderer::getPageImage(int pageIndex, int pixelSize)
{
    const QString key = getKey(pageIndex, pixelSize);

    if (const QImage* cachedImage = m_pageImageCache.object(key))
    {
        return *cachedImage;
    }

    if (m_pendingKeys.contains(key))
    {
        return QImage();
    }

    const PDFDocument* document = nullptr;
    {
        QMutexLocker guard(&m_contextMutex);
        document = m_document;
    }
    if (!document || pageIndex < 0 || pageIndex >= static_cast<int>(document->getCatalog()->getPageCount()))
    {
        return QImage();
    }

    RenderRequest request;
    request.key = key;
    request.pageIndex = pageIndex;
    request.pixelSize = pixelSize;
    request.epoch = m_renderEpoch;
    request.revision = currentRevision();

    m_pendingKeys.insert(key);
    m_requestQueue.push_back(std::move(request));

    auto& keys = m_keysByPage[pageIndex];
    keys.insert(key);

    startNextRequest();
    return QImage();
}

QString PDFThumbnailsRenderer::getKey(int pageIndex, int pixelSize) const
{
    return QStringLiteral("%1/%2@%3#e%4").arg(currentRevision().toString()).arg(pageIndex).arg(pixelSize).arg(m_renderEpoch);
}

PDFRevisionIdentity PDFThumbnailsRenderer::currentRevision() const
{
    return m_documentContext ? m_documentContext->getRevision() : m_revision;
}

bool PDFThumbnailsRenderer::ensureContextLocked()
{
    if (m_context)
    {
        return true;
    }

    if (!m_document)
    {
        return false;
    }

    auto context = std::make_unique<DocumentRenderContext>();
    context->document = m_document;
    context->revision = currentRevision();
    context->fontCache = std::make_unique<PDFFontCache>(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    context->cmsManager = std::make_unique<PDFCMSManager>(nullptr);
    context->optionalContentActivity = std::make_unique<PDFOptionalContentActivity>(m_document, OCUsage::View, nullptr);
    context->fontCache->setDocument(PDFModifiedDocument(const_cast<PDFDocument*>(m_document), const_cast<PDFOptionalContentActivity*>(context->optionalContentActivity.get())));
    context->cmsManager->setDocument(m_document);

    const int threadHint = PDFExecutionPolicy::getMaxThreadCount(PDFExecutionPolicy::Scope::Page);
    const int rasterizerCount = PDFRasterizerPool::getCorrectedRasterizerCount(threadHint);
    context->rasterizerPool = std::make_unique<PDFRasterizerPool>(m_document,
                                                                  context->fontCache.get(),
                                                                  context->cmsManager.get(),
                                                                  context->optionalContentActivity.get(),
                                                                  context->features,
                                                                  context->meshQualitySettings,
                                                                  rasterizerCount,
                                                                  RendererEngine::Blend2D_SingleThread,
                                                                  nullptr);

    m_context = std::move(context);
    return true;
}

void PDFThumbnailsRenderer::startNextRequest()
{
    if (m_renderInProgress)
    {
        return;
    }

    QList<RenderRequest> requests;
    {
        QMutexLocker guard(&m_contextMutex);
        if (!ensureContextLocked() || m_requestQueue.isEmpty())
        {
            return;
        }

        const int maxBatchSize = qMax(1, PDFExecutionPolicy::getMaxThreadCount(PDFExecutionPolicy::Scope::Page));

        requests.reserve(maxBatchSize);
        while (!m_requestQueue.isEmpty() && requests.size() < maxBatchSize)
        {
            const RenderRequest request = m_requestQueue.takeFirst();
            if (!m_pendingKeys.contains(request.key))
            {
                // A task with this key has been removed meanwhile (e.g. by clear()).
                continue;
            }
            requests.push_back(request);
        }
    }

    if (requests.isEmpty())
    {
        return;
    }

    m_renderInProgress = true;
    PDFJobSpec spec;
    spec.kind = PDFJobKind::Thumbnail;
    spec.priority = PDFJobPriority::NearViewport;
    spec.documentKey = currentRevision().document.documentId;
    spec.documentRevision = currentRevision().toString();
    spec.staleResultPolicy = PDFJobStaleResultPolicy::Discard;
    PDFJobScheduler::global().setCurrentRevision(spec.documentKey, spec.documentRevision);
    m_renderJobId = PDFJobScheduler::global().submit(spec, [this, requests = std::move(requests)](PDFJobContext& context) mutable
    {
        if (context.isCancellationRequested())
        {
            return;
        }
        RenderBatchResult results = renderBatchAsync(std::move(requests));
        QMutexLocker guard(&m_contextMutex);
        m_batchResult = std::move(results);
    });
}

void PDFThumbnailsRenderer::waitForCurrentRender()
{
    if (!m_renderJobId.isEmpty())
    {
        PDFJobScheduler::global().cancel(m_renderJobId);
        PDFJobScheduler::global().waitForFinished(m_renderJobId, 5000);
        m_renderJobId.clear();
    }

    m_renderInProgress = false;
}

PDFThumbnailsRenderer::RenderResult PDFThumbnailsRenderer::renderPageAsync(const RenderRequest& request) const
{
    RenderResult result;
    result.key = request.key;
    result.pageIndex = request.pageIndex;
    result.epoch = request.epoch;
    result.revision = request.revision;

    DocumentRenderContext* context = nullptr;
    {
        QMutexLocker guard(&m_contextMutex);
        context = m_context.get();
    }

    if (!context || !context->document)
    {
        return result;
    }

    const PDFCatalog* catalog = context->document->getCatalog();
    if (!catalog || request.pageIndex < 0 || request.pageIndex >= static_cast<int>(catalog->getPageCount()))
    {
        return result;
    }

    const PDFPage* page = catalog->getPage(request.pageIndex);
    if (!page)
    {
        return result;
    }

    PDFCMSPointer cms = context->cmsManager->getCurrentCMS();
    if (cms.isNull())
    {
        return result;
    }

    QRectF pageRect = page->getRotatedMediaBox();
    QSizeF pageSize = pageRect.size();
    pageSize.scale(request.pixelSize, request.pixelSize, Qt::KeepAspectRatio);
    QSize imageSize = pageSize.toSize();
    if (!imageSize.isValid())
    {
        return result;
    }

    PDFPrecompiledPage compiledPage;
    PDFRenderer renderer(context->document,
                         context->fontCache.get(),
                         cms.data(),
                         context->optionalContentActivity.get(),
                         context->features,
                         context->meshQualitySettings);
    renderer.compile(&compiledPage, request.pageIndex);
    if (!compiledPage.isValid())
    {
        return result;
    }

    // We can const-cast here, because we do not modify the document in the
    // annotation manager. A fresh annotation manager is created per page, because
    // it is not thread safe and multiple pages can be rendered concurrently.
    // Target::View matches how the widget's annotation manager renders thumbnails.
    PDFModifiedDocument modifiedDocument(const_cast<PDFDocument*>(context->document),
                                         const_cast<PDFOptionalContentActivity*>(context->optionalContentActivity.get()));
    PDFAnnotationManager annotationManager(context->fontCache.get(),
                                           context->cmsManager.get(),
                                           context->optionalContentActivity.get(),
                                           context->meshQualitySettings,
                                           context->features,
                                           PDFAnnotationManager::Target::View,
                                           nullptr);
    annotationManager.setDocument(modifiedDocument);

    PDFRasterizer* rasterizer = context->rasterizerPool->acquire();
    QImage image = rasterizer->render(request.pageIndex,
                                      page,
                                      &compiledPage,
                                      imageSize,
                                      context->features,
                                      &annotationManager,
                                      cms.data(),
                                      PageRotation::None);
    context->rasterizerPool->release(rasterizer);

    if (!image.isNull())
    {
        result.image = std::move(image);
    }
    return result;
}

PDFThumbnailsRenderer::RenderBatchResult PDFThumbnailsRenderer::renderBatchAsync(QList<RenderRequest> requests) const
{
    DocumentRenderContext* context = nullptr;
    PDFFontCache* fontCache = nullptr;
    {
        QMutexLocker guard(&m_contextMutex);
        context = m_context.get();
        if (context)
        {
            fontCache = context->fontCache.get();
        }
    }

    if (!context)
    {
        return RenderBatchResult();
    }

    FontCacheShrinkGuard fontCacheShrinkGuard(this, fontCache);

    RenderBatchResult results;
    results.resize(requests.size());

    std::vector<int> indices(requests.size());
    std::iota(indices.begin(), indices.end(), 0);

    auto processRequest = [this, &requests, &results](int requestIndex)
    {
        results[requestIndex] = renderPageAsync(requests[requestIndex]);
    };

    PDFExecutionPolicy::execute(PDFExecutionPolicy::Scope::Page, indices.cbegin(), indices.cend(), processRequest);
    return results;
}

void PDFThumbnailsRenderer::onRenderFinished()
{
    PDFJobSnapshot snapshot;
    RenderBatchResult results;
    {
        QMutexLocker guard(&m_contextMutex);
        snapshot = PDFJobScheduler::global().snapshot(m_renderJobId);
        results = std::move(m_batchResult);
        m_batchResult.clear();
        m_renderJobId.clear();
    }
    m_renderInProgress = false;
    if (snapshot.status != PDFJobStatus::Succeeded)
    {
        startNextRequest();
        return;
    }

    for (const RenderResult& result : results)
    {
        m_pendingKeys.remove(result.key);

        if (!result.image.isNull())
        {
            const bool isCurrentResult = result.epoch == m_renderEpoch && result.revision == currentRevision();
            if (isCurrentResult)
            {
                if (!m_pageImageCache.contains(result.key))
                {
                    const int cost = qMax(1, int(qMin<qint64>(result.image.sizeInBytes(), std::numeric_limits<int>::max())));
                    m_pageImageCache.insert(result.key, new QImage(result.image), cost);
                }
                Q_EMIT pageImageReady(result.pageIndex);
            }
        }
    }

    startNextRequest();
}

void PDFThumbnailsRenderer::onContextRevisionChanged(const PDFRevisionIdentity& previous, const PDFRevisionIdentity& current)
{
    Q_UNUSED(previous);
    waitForCurrentRender();
    {
        QMutexLocker guard(&m_contextMutex);
        m_document = m_documentContext ? m_documentContext->getDocument() : nullptr;
        m_context.reset();
        m_revision = current;
    }
    clear();
}

void PDFThumbnailsRenderer::invalidatePage(int pageIndex)
{
    const auto it = m_keysByPage.find(pageIndex);
    if (it != m_keysByPage.cend())
    {
        for (const QString& key : it.value())
        {
            m_pageImageCache.remove(key);
            m_pendingKeys.remove(key);
        }
        m_keysByPage.erase(it);
    }

    QList<RenderRequest> filteredQueue;
    filteredQueue.reserve(m_requestQueue.size());
    for (RenderRequest& request : m_requestQueue)
    {
        if (request.pageIndex != pageIndex)
        {
            filteredQueue.push_back(std::move(request));
        }
        else
        {
            m_pendingKeys.remove(request.key);
        }
    }
    m_requestQueue.swap(filteredQueue);
    ++m_renderEpoch;
}

}   // namespace pdf
