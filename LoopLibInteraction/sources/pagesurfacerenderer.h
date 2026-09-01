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

#ifndef PAGESURFACERENDERER_H
#define PAGESURFACERENDERER_H

#include "pagesurfacekey.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"

#include <mutex>
#include <QtGlobal>

namespace pdfinteraction
{

/// Turns one page surface request into one terminal result.
///
/// Called on a pdf::PDFJobScheduler worker thread. Implementations must honour
/// jobContext.isCancellationRequested() and must never throw: a budget or render
/// failure is a typed terminal state, because a throw out of the work lambda
/// reaches the scheduler as an untyped Failed and the caller loses the reason.
class IPageSurfaceRenderer
{
public:
    virtual ~IPageSurfaceRenderer() = default;

    virtual PageSurfaceResult render(const PageSurfaceRequest& request, pdf::PDFJobContext& jobContext) = 0;

    /// Advisory: the coordinator is over budget and is asking the render path to
    /// give memory back. Called on the owner thread and must not block.
    virtual void shedPrefetchAndQuality() {}
};

/// Renders through the document context's existing pdf::PDFDocumentSession.
///
/// pdf::PDFDocumentSession states that it is not thread-safe: compile and decode
/// writes and invalidate() must not run concurrently. One mutex therefore
/// serializes every render for a context, and that same mutex is what makes
/// teardown safe -- detach() takes it, so it either waits for the render in
/// flight or observes that none is running, and after it returns no worker can
/// reach the context again. This is the Phase 4 baseline the plan of record
/// calls for: serialize access to the one session rather than clone document
/// state into worker-local caches.
///
/// The lock is never taken on the owner thread except in detach(), which is a
/// bounded wait for a single tile.
class PDFSessionPageSurfaceRenderer final : public IPageSurfaceRenderer
{
public:
    explicit PDFSessionPageSurfaceRenderer(pdf::PDFDocumentContext& context);
    ~PDFSessionPageSurfaceRenderer() override;

    PDFSessionPageSurfaceRenderer(const PDFSessionPageSurfaceRenderer&) = delete;
    PDFSessionPageSurfaceRenderer& operator=(const PDFSessionPageSurfaceRenderer&) = delete;

    /// Owner thread. Stops using the context; blocks until an in-flight render
    /// returns. Idempotent, and called by the destructor.
    void detach();

    PageSurfaceResult render(const PageSurfaceRequest& request, pdf::PDFJobContext& jobContext) override;

    /// Owner thread. Forwards to pdf::PDFDocumentSession::shedPrefetchAndQuality,
    /// but only if the session is idle: shedding is advisory, and waiting on a
    /// worker here would stall the very thread the shedding is meant to protect.
    void shedPrefetchAndQuality() override;

    /// Owner thread. Acquires m_mutex before lowering the session's cache limit so
    /// the eviction inside setCacheLimit cannot race a worker's use of a compilePage
    /// pointer. Blocks until any in-flight render returns.
    void setCacheLimit(qsizetype totalBytes);

private:
    mutable std::mutex m_mutex;

    /// Guarded by m_mutex. Null after detach().
    pdf::PDFDocumentContext* m_context = nullptr;
};

}   // namespace pdfinteraction

#endif   // PAGESURFACERENDERER_H
