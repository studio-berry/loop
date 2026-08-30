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

#include "pdfdocumentsession.h"

#include "pdfconstants.h"
#include "pdfcms.h"
#include "pdffont.h"
#include "pdfoptionalcontent.h"
#include "pdfpagecachebudget.h"
#include "pdfprocessingbudget.h"

#include <tuple>
#include <QtGlobal>

namespace pdf
{

PDFDocumentSession::PDFDocumentSession(PDFDocument* document, PDFDocumentContext* context) :
    m_document(document),
    m_context(context),
    m_localDocumentIdentity(PDFDocumentIdentity::fromDocument(document)),
    m_features(PDFRenderer::getDefaultFeatures()),
    m_processingBudget(std::make_unique<PDFProcessingBudget>()),
    m_compiledCacheByteLimit(CompiledCacheByteLimitDefault),
    m_compiledCacheBytes(0),
    m_cacheLimit(CompiledCacheByteLimitDefault * 2)
{
    initializeRendering();
}

PDFDocumentSession::~PDFDocumentSession() = default;

PDFDocument* PDFDocumentSession::getDocument() const
{
    return m_document;
}

PDFRevisionIdentity PDFDocumentSession::getRevision() const
{
    if (m_context)
    {
        return m_context->getRevision();
    }

    PDFRevisionIdentity revision;
    revision.document = m_localDocumentIdentity;
    revision.documentRevision = m_localDocumentRevision;
    revision.cacheGeneration = m_localCacheGeneration;
    return revision;
}

bool PDFDocumentSession::isCurrent(const PDFRevisionIdentity& revision) const
{
    return revision == getRevision();
}

bool PDFDocumentSession::isValid() const
{
    return m_document != nullptr;
}

void PDFDocumentSession::setRendererFeatures(PDFRenderer::Features features)
{
    if (m_features == features)
    {
        return;
    }

    m_features = features;
    if (m_context)
    {
        m_context->invalidateCaches();
    }
    else
    {
        ++m_localCacheGeneration;
        m_compileCache.clear();
        m_compileCacheOrder.clear();
        m_compileCacheBytes.clear();
        m_compiledCacheBytes = 0;
    }

    if (m_renderer)
    {
        PDFMeshQualitySettings meshQualitySettings;
        m_renderer = std::make_unique<PDFRenderer>(m_document,
                                                   m_fontCache.get(),
                                                   m_cms.get(),
                                                   m_optionalContentActivity.get(),
                                                   m_features,
                                                   meshQualitySettings,
                                                   m_processingBudget.get());
    }
}

PDFRenderer::Features PDFDocumentSession::getRendererFeatures() const
{
    return m_features;
}

PDFProcessingBudget* PDFDocumentSession::getProcessingBudget() const
{
    return m_processingBudget.get();
}

const PDFProcessingLimits& PDFDocumentSession::getProcessingLimits() const
{
    return m_processingBudget->limits();
}

void PDFDocumentSession::setProcessingLimits(const PDFProcessingLimits& limits)
{
    m_processingBudget = std::make_unique<PDFProcessingBudget>(limits);
    if (m_context)
    {
        m_context->invalidateCaches();
    }
    else
    {
        invalidate();
    }
    m_renderer.reset();
    m_fontCache.reset();
    m_cms.reset();
    m_cmsManager.reset();
    m_optionalContentActivity.reset();
    initializeRendering();
}

void PDFDocumentSession::resetProcessingBudget()
{
    m_processingBudget->reset();
}

void PDFDocumentSession::shedPrefetchAndQuality()
{
    m_prefetchEnabled = false;
    m_qualityPercent = qMin(m_qualityPercent, ShedQualityPercent);
    m_qualityPrefetchShed = true;
    m_compileCacheLimit = qMin(m_compileCacheLimit, ShedCompileCacheLimit);
    m_streamCacheLimit = qMin(m_streamCacheLimit, ShedStreamCacheLimit);
    m_compiledCacheByteLimit = qMin(m_compiledCacheByteLimit, static_cast<qsizetype>(ShedCompiledCacheByteLimit));
    trimCachesToLimits();
}

qsizetype PDFDocumentSession::compiledCacheBytes() const
{
    return m_compiledCacheBytes;
}

qsizetype PDFDocumentSession::compiledCacheByteLimit() const
{
    return m_compiledCacheByteLimit;
}

void PDFDocumentSession::setCompiledCacheByteLimit(qsizetype bytes)
{
    bytes = qMax<qsizetype>(0, bytes);
    m_compiledCacheByteLimit = bytes;
    // Keep total in sync when compiled limit is set directly: total is at least compiled*2
    // (odd totals lose one byte when reconstructed, so store even total).
    m_cacheLimit = m_compiledCacheByteLimit * 2;
    trimCachesToLimits();
}

void PDFDocumentSession::setCacheLimit(qsizetype totalBytes)
{
    totalBytes = PDFPageCacheBudget::total(totalBytes);
    m_cacheLimit = totalBytes;
    m_compiledCacheByteLimit = PDFPageCacheBudget::compiledPages(totalBytes);
    trimCachesToLimits();
}

qsizetype PDFDocumentSession::cacheLimit() const
{
    return m_cacheLimit;
}

void PDFDocumentSession::trimCachesToLimits()
{
    while (!m_compileCacheOrder.empty() && (m_compileCacheOrder.size() > m_compileCacheLimit || m_compiledCacheBytes > m_compiledCacheByteLimit))
    {
        const PageCacheKey key = m_compileCacheOrder.front();
        auto bytesIt = m_compileCacheBytes.find(key);
        if (bytesIt != m_compileCacheBytes.end())
        {
            m_compiledCacheBytes -= bytesIt->second;
            m_compileCacheBytes.erase(bytesIt);
        }
        m_compileCache.erase(key);
        m_compileCacheOrder.pop_front();
    }
    while (!m_streamCacheOrder.empty() && m_streamCacheOrder.size() > m_streamCacheLimit)
    {
        m_streamCache.erase(m_streamCacheOrder.front());
        m_streamCacheOrder.pop_front();
    }
}

const PDFPrecompiledPage* PDFDocumentSession::compilePage(size_t pageIndex)
{
    if (!isValid())
    {
        return nullptr;
    }

    const PageCacheKey key{ getRevision(), pageIndex };
    auto it = m_compileCache.find(key);
    if (it != m_compileCache.cend())
    {
        return &it->second;
    }

    const PDFCatalog* catalog = m_document->getCatalog();
    if (!catalog || pageIndex >= static_cast<size_t>(catalog->getPageCount()))
    {
        return nullptr;
    }

    PDFPrecompiledPage compiledPage;
    m_renderer->compile(&compiledPage, pageIndex);

    qsizetype estimate = static_cast<qsizetype>(compiledPage.getMemoryConsumptionEstimate());
    if (estimate <= 0)
    {
        // A finalized precompiled page always reports at least sizeof(*this).
        // Do not admit an entry whose resident size cannot be accounted for.
        return nullptr;
    }

    if (m_compileCacheLimit == 0 || estimate > m_compiledCacheByteLimit)
    {
        // An entry larger than its entire partition cannot be made resident
        // without violating the shared byte cap. Returning nullptr is safe for
        // callers: no cache entry or dangling pointer is exposed.
        return nullptr;
    }

    // Evict before inserting, so the pointer returned below is never the entry
    // this call dropped. Evict by both entry count and byte budget.
    while (!m_compileCacheOrder.empty() &&
           (m_compileCacheOrder.size() >= m_compileCacheLimit ||
            m_compiledCacheBytes > m_compiledCacheByteLimit - estimate))
    {
        const PageCacheKey evictKey = m_compileCacheOrder.front();
        auto bytesIt = m_compileCacheBytes.find(evictKey);
        if (bytesIt != m_compileCacheBytes.end())
        {
            m_compiledCacheBytes -= bytesIt->second;
            m_compileCacheBytes.erase(bytesIt);
        }
        m_compileCache.erase(evictKey);
        m_compileCacheOrder.pop_front();
    }

    m_compileCacheOrder.push_back(key);
    auto result = m_compileCache.emplace(key, std::move(compiledPage));
    m_compileCacheBytes[key] = estimate;
    m_compiledCacheBytes += estimate;
    return &result.first->second;
}

QByteArray PDFDocumentSession::getDecodedStream(PDFObjectReference reference)
{
    if (!isValid())
    {
        return QByteArray();
    }

    const StreamCacheKey key{ getRevision(), reference };
    auto it = m_streamCache.find(key);
    if (it != m_streamCache.cend())
    {
        return it->second;
    }

    const PDFObject& object = m_document->getObjectByReference(reference);
    if (!object.isStream())
    {
        return QByteArray();
    }

    QByteArray decoded = m_document->getStorage().getDecodedStream(object.getStream(), m_processingBudget.get());

    while (!m_streamCacheOrder.empty() && m_streamCacheOrder.size() >= m_streamCacheLimit)
    {
        m_streamCache.erase(m_streamCacheOrder.front());
        m_streamCacheOrder.pop_front();
    }

    m_streamCacheOrder.push_back(key);
    auto result = m_streamCache.emplace(key, std::move(decoded));
    return result.first->second;
}

void PDFDocumentSession::invalidate()
{
    if (!m_context)
    {
        ++m_localCacheGeneration;
    }
    m_compileCache.clear();
    m_compileCacheOrder.clear();
    m_compileCacheBytes.clear();
    m_compiledCacheBytes = 0;
    m_streamCache.clear();
    m_streamCacheOrder.clear();
}

PDFRenderer* PDFDocumentSession::getRenderer() const
{
    return m_renderer.get();
}

PDFFontCache* PDFDocumentSession::getFontCache() const
{
    return m_fontCache.get();
}

PDFCMS* PDFDocumentSession::getCMS() const
{
    return m_cms.get();
}

PDFOptionalContentActivity* PDFDocumentSession::getOptionalContentActivity() const
{
    return m_optionalContentActivity.get();
}

void PDFDocumentSession::initializeRendering()
{
    if (!isValid())
    {
        return;
    }

    m_optionalContentActivity = std::make_unique<PDFOptionalContentActivity>(m_document, OCUsage::Export, nullptr);

    m_cmsManager = std::make_unique<PDFCMSManager>(nullptr);
    m_cmsManager->setDocument(m_document);
    m_cms = m_cmsManager->getCurrentCMS();

    m_fontCache = std::make_unique<PDFFontCache>(DEFAULT_FONT_CACHE_LIMIT, DEFAULT_REALIZED_FONT_CACHE_LIMIT);
    PDFModifiedDocument modifiedDocument(m_document, m_optionalContentActivity.get());
    m_fontCache->setDocument(modifiedDocument);
    m_fontCache->setCacheShrinkEnabled(nullptr, false);

    PDFMeshQualitySettings meshQualitySettings;
    m_renderer = std::make_unique<PDFRenderer>(m_document,
                                               m_fontCache.get(),
                                               m_cms.get(),
                                               m_optionalContentActivity.get(),
                                               m_features,
                                               meshQualitySettings,
                                               m_processingBudget.get());
}

}   // namespace pdf
