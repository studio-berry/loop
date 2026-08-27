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

#ifndef PDFDOCUMENTSESSION_H
#define PDFDOCUMENTSESSION_H

#include "pdfglobal.h"
#include "pdfdocument.h"
#include "pdfdocumentcontext.h"
#include "pdfrenderer.h"
#include "pdfpainter.h"
#include "pdfpagecachebudget.h"
#include "pdfcms.h"
#include "pdfprocessingbudget.h"
#include "pdfresourcebudget.h"

#include <QByteArray>
#include <QtGlobal>

#include <deque>
#include <map>
#include <memory>
#include <tuple>

namespace pdf
{

class PDFFontCache;
class PDFCMSManager;
class PDFCMS;
class PDFOptionalContentActivity;
class PDFProcessingBudget;

/// Shared session for a PDF document that caches expensive intermediate
/// artifacts used by preflight, rendering, and analysis tools. The session
/// borrows the document (it does not own it) and must be invalidated when the
/// document is mutated.
///
/// Current caches:
///   - Page compilation cache (`PDFPrecompiledPage` per page index)
///   - Decoded stream cache (`QByteArray` per stream object reference)
///
/// The session also owns the rendering helpers (font cache, CMS, optional
/// content activity, renderer) so that multiple checks can reuse them without
/// recreating them per page.
///
/// Thread-safety: the session is not thread-safe. Writes (compile, decode)
/// and invalidate() must not be called concurrently. Const reads from the
/// cache after writes complete are safe.
class LOOPLIBCORESHARED_EXPORT PDFDocumentSession
{
public:
    explicit PDFDocumentSession(PDFDocument* document,
                                PDFDocumentContext* context = nullptr,
                                std::shared_ptr<PDFPageCacheBudget> pageCacheBudget = nullptr);
    ~PDFDocumentSession();

    /// Estimates the resident model owned by a parsed document, including raw
    /// object content and parsed catalog containers. The same estimate is used
    /// by editor admission and unattended PdfTool qualification.
    static qsizetype estimateDocumentModelBytes(const PDFDocument* document);

    PDFDocumentSession(const PDFDocumentSession&) = delete;
    PDFDocumentSession& operator=(const PDFDocumentSession&) = delete;

    /// Returns the document associated with this session.
    PDFDocument* getDocument() const;
    PDFRevisionIdentity getRevision() const;
    bool isCurrent(const PDFRevisionIdentity& revision) const;

    /// Returns true if the session is associated with a non-null document.
    bool isValid() const;

    /// Sets the renderer features used when compiling pages. Changing features
    /// invalidates the compile cache because compiled pages depend on them.
    void setRendererFeatures(PDFRenderer::Features features);
    PDFRenderer::Features getRendererFeatures() const;

    /// Returns the precompiled page for the given page index, compiling it on
    /// first access and caching the result. Returns nullptr if the page does
    /// not exist or compilation fails.
    const PDFPrecompiledPage* compilePage(size_t pageIndex);

    /// Returns the decoded stream bytes for the given stream object reference,
    /// decoding it on first access and caching the result. Returns an empty
    /// byte array if the reference is invalid or the stream cannot be decoded.
    QByteArray getDecodedStream(PDFObjectReference reference);

    PDFPageCacheBudget* getPageCacheBudget() const { return m_pageCacheBudget.get(); }
    std::shared_ptr<PDFPageCacheBudget> getSharedPageCacheBudget() const { return m_pageCacheBudget; }

    PDFProcessingBudget* getProcessingBudget() const;
    PDFResourceBudget* getResourceBudget() const;
    std::shared_ptr<PDFResourceBudget> getSharedResourceBudget() const;
    const PDFProcessingLimits& getProcessingLimits() const;
    void setProcessingLimits(const PDFProcessingLimits& limits);
    void resetProcessingBudget();

    /// Under memory or time pressure, drop prefetch and quality work before
    /// interaction. Compile/stream cache caps shrink; a compiled page that is
    /// larger than the current compiled-page byte share is rejected.
    void shedPrefetchAndQuality();
    bool prefetchEnabled() const { return m_prefetchEnabled; }
    int qualityPercent() const { return m_qualityPercent; }
    bool qualityPrefetchShed() const { return m_qualityPrefetchShed; }
    size_t compileCacheLimit() const { return m_compileCacheLimit; }
    size_t streamCacheLimit() const { return m_streamCacheLimit; }

    qsizetype compiledCacheBytes() const;
    qsizetype compiledCacheByteLimit() const;
    qsizetype decodedStreamCacheBytes() const;
    qsizetype decodedStreamCacheByteLimit() const;
    void setCompiledCacheByteLimit(qsizetype bytes);
    void setCacheLimit(qsizetype totalBytes);
    qsizetype cacheLimit() const;

    /// Clears all caches. Call this when the underlying document is mutated.
    void invalidate();

    /// Cache bounds. Both caches evict in insertion order once full, so a
    /// document-wide sequential pass costs a fixed amount of memory instead of
    /// retaining one compiled page (and every decoded stream) for the lifetime
    /// of the session. Compiled pages are additionally bounded by their
    /// getMemoryConsumptionEstimate() values; decoded streams remain an
    /// independent entry-count cache.
    ///
    /// Eviction happens before insertion, so the pointer returned by
    /// compilePage() is never the entry evicted by that same call. It may be
    /// invalidated by a later compilePage() call — use it before compiling
    /// another page.
    static constexpr size_t CompileCacheLimit = 8;
    static constexpr size_t StreamCacheLimit = 256;
    static constexpr size_t ShedCompileCacheLimit = 2;
    static constexpr size_t ShedStreamCacheLimit = 16;
    static constexpr int ShedQualityPercent = 25;

    static constexpr qsizetype CompiledCacheByteLimitDefault = 64 * 1024 * 1024;
    static constexpr qsizetype ShedCompiledCacheByteLimit = 8 * 1024 * 1024;

    /// Low-level access to the renderer and its dependencies. Prefer the
    /// compilePage() helper; these accessors are exposed for tools that need
    /// direct rasterization control.
    PDFRenderer* getRenderer() const;

    PDFFontCache* getFontCache() const;
    PDFCMS* getCMS() const;
    PDFOptionalContentActivity* getOptionalContentActivity() const;

private:
    struct PageCacheKey
    {
        PDFRevisionIdentity revision;
        size_t pageIndex = 0;

        bool operator<(const PageCacheKey& other) const
        {
            return std::tie(revision, pageIndex) < std::tie(other.revision, other.pageIndex);
        }
    };

    struct StreamCacheKey
    {
        PDFRevisionIdentity revision;
        PDFObjectReference reference;

        bool operator<(const StreamCacheKey& other) const
        {
            return std::tie(revision, reference) < std::tie(other.revision, other.reference);
        }
    };

    void initializeRendering();
    void trimCachesToLimits();
    void clearCompiledCache();
    void clearDecodedStreamCache();

    PDFDocument* m_document;
    PDFDocumentContext* m_context;
    PDFDocumentIdentity m_localDocumentIdentity;
    DocumentRevision m_localDocumentRevision = 0;
    quint64 m_localCacheGeneration = 0;
    PDFRenderer::Features m_features;
    std::unique_ptr<PDFProcessingBudget> m_processingBudget;
    std::shared_ptr<PDFResourceBudget> m_resourceBudget;
    PDFResourceReservation m_documentModelReservation;
    std::shared_ptr<PDFPageCacheBudget> m_pageCacheBudget;
    size_t m_compileCacheLimit = CompileCacheLimit;
    size_t m_streamCacheLimit = StreamCacheLimit;
    bool m_prefetchEnabled = true;
    int m_qualityPercent = 100;
    bool m_qualityPrefetchShed = false;

    qsizetype m_streamCacheByteLimit = 256 * PDFResourceBudgetConfig::MiB;
    qsizetype m_streamCacheBytes = 0;
    qsizetype m_compiledCachePressureLimit = CompiledCacheByteLimitDefault;
    std::map<PageCacheKey, qsizetype> m_compileCacheBytes;

    std::unique_ptr<PDFOptionalContentActivity> m_optionalContentActivity;
    std::unique_ptr<PDFCMSManager> m_cmsManager;
    PDFCMSPointer m_cms;
    std::unique_ptr<PDFFontCache> m_fontCache;
    std::unique_ptr<PDFRenderer> m_renderer;

    std::map<PageCacheKey, PDFPrecompiledPage> m_compileCache;
    std::deque<PageCacheKey> m_compileCacheOrder;
    std::map<StreamCacheKey, QByteArray> m_streamCache;
    std::deque<StreamCacheKey> m_streamCacheOrder;
};

}   // namespace pdf

#endif   // PDFDOCUMENTSESSION_H
