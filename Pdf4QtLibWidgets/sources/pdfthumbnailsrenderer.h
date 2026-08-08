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

#ifndef PDFTHUMBNAILSRENDERER_H
#define PDFTHUMBNAILSRENDERER_H

#include "pdfwidgetsglobal.h"
#include "pdfrenderer.h"
#include "pdfmeshqualitysettings.h"

#include <QCache>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>

#include <memory>
#include <vector>

namespace pdf
{
class PDFDocument;
class PDFFontCache;
class PDFCMSManager;
class PDFOptionalContentActivity;

/// Asynchronous renderer for page thumbnail images.
///
/// The renderer accepts thumbnail requests from the sidebar thumbnails model,
/// keeps an in-memory image cache, and renders missing thumbnails in background
/// worker threads. It lives in the GUI thread; heavy rendering is executed
/// asynchronously and finished thumbnails are delivered via @ref pageImageReady.
///
/// Internally, requests are tagged by an epoch. The epoch is increased whenever
/// the document is replaced or the cache is invalidated, so stale results are
/// ignored safely.
class PDF4QTLIBWIDGETSSHARED_EXPORT PDFThumbnailsRenderer : public QObject
{
    Q_OBJECT

private:
    static constexpr int THUMBNAIL_CACHE_LIMIT_BYTES = 128 * 1024 * 1024;

public:
    /**
     * @brief Creates a thumbnail renderer.
     * @param document Document whose pages are rendered. Null is allowed.
     * @param parent QObject parent.
     */
    explicit PDFThumbnailsRenderer(const PDFDocument* document, QObject* parent = nullptr);
    virtual ~PDFThumbnailsRenderer() override;

    /**
     * @brief Sets the document whose pages are rendered.
     *
     * This function may block until the currently running render batch finishes,
     * so the previous document can be safely released after the call.
     * @param document New document.
     */
    void setDocument(const PDFDocument* document);

    /**
     * @brief Returns the cached thumbnail for a page, or a null image if it is
     *        not cached. If the thumbnail is missing, then a render request is
     *        scheduled asynchronously and @ref pageImageReady is emitted later.
     * @param pageIndex Page index.
     * @param pixelSize Thumbnail size in device pixels.
     */
    QImage getPageImage(int pageIndex, int pixelSize);

    /**
     * @brief Blocks until the currently running render batch finishes.
     *
     * Must be called before the rendered document is released by the owner.
     */
    void waitForFinished();

    /**
     * @brief Clears the image cache and pending requests and advances the epoch.
     */
    void clear();

    /**
     * @brief Marks the cached image for a page as invalid so it is re-rendered
     *        on the next request.
     * @param pageIndex Page index.
     */
    void invalidatePage(int pageIndex);

signals:
    /**
     * @brief Emitted when a thumbnail image becomes available.
     * @param pageIndex Page index of the finished thumbnail.
     */
    void pageImageReady(int pageIndex);

private:
    /// Immutable data required to render one thumbnail.
    struct RenderRequest
    {
        QString key;
        int pageIndex = -1;
        int pixelSize = 1;
        quint64 epoch = 0;
    };

    /// Result of one finished render request.
    struct RenderResult
    {
        QString key;
        int pageIndex = -1;
        QImage image;
        quint64 epoch = 0;
    };
    using RenderBatchResult = std::vector<RenderResult>;

    /// Returns the cache key for the given page and pixel size.
    QString getKey(int pageIndex, int pixelSize) const;

    /// Creates/reuses the rendering context for the current document.
    bool ensureContextLocked();

    /// Starts the next render batch if the renderer is idle.
    void startNextRequest();

    /// Waits for a currently running render batch.
    void waitForCurrentRender();

    /// Renders a single request (executed in a worker context).
    RenderResult renderPageAsync(const RenderRequest& request) const;

    /// Renders a batch of requests.
    RenderBatchResult renderBatchAsync(QList<RenderRequest> requests) const;

private slots:
    void onRenderFinished();

private:
    const PDFDocument* m_document;
    QCache<QString, QImage> m_pageImageCache;
    QSet<QString> m_pendingKeys;
    QList<RenderRequest> m_requestQueue;
    QFutureWatcher<RenderBatchResult> m_renderWatcher;
    bool m_renderInProgress = false;
    quint64 m_renderEpoch = 0;

    /// Cache keys (including pixel size) per page index, used to invalidate
    /// cached images for a page when its content changes.
    QHash<int, QSet<QString>> m_keysByPage;

    struct DocumentRenderContext;
    mutable QMutex m_contextMutex;
    std::unique_ptr<DocumentRenderContext> m_context;
};

}   // namespace pdf

#endif // PDFTHUMBNAILSRENDERER_H