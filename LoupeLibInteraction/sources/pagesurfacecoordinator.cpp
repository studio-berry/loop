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

#include "pagesurfacecoordinator.h"

#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace pdfinteraction
{

namespace
{

/// Bytes a request is expected to produce, used for the in-flight bound before
/// the real surface exists. Four bytes per pixel matches
/// QImage::Format_ARGB32_Premultiplied.
qint64 estimatedBytes(const PageSurfaceKey& key)
{
    return static_cast<qint64>(key.targetPixelSize.width()) * static_cast<qint64>(key.targetPixelSize.height()) * 4;
}

}   // namespace

PageSurfaceCoordinator::PageSurfaceCoordinator(IDocumentRevisionSource& revisions,
                                               IJobSubmitter& submitter,
                                               IPageSurfaceRenderer& renderer,
                                               ViewportController& viewport,
                                               PageSurfaceBounds bounds,
                                               QObject* parent) :
    QObject(parent),
    m_submitter(&submitter),
    m_renderer(&renderer),
    m_revisions(&revisions),
    m_viewport(&viewport),
    m_bounds(bounds),
    m_relay(new JobRelay, [](JobRelay* relay)
            { relay->deleteLater(); })
{
    connect(m_viewport, &ViewportController::demandChanged, this, &PageSurfaceCoordinator::onDemandChanged);
    connect(m_viewport, &ViewportController::placementsChanged, this, &PageSurfaceCoordinator::rebuildSnapshot);

    // Stamp the snapshot token before the first admission so the presenter does
    // not treat the initial empty state as a replaced document.
    rebuildSnapshot();
}

PageSurfaceCoordinator::~PageSurfaceCoordinator()
{
    // Detach before anything else, exactly as DocumentFacade does: after this no
    // queued completion can reach a half-destroyed coordinator, and the relay
    // itself lives until the last worker lambda releases its shared_ptr.
    m_relay->detach();

    for (const InFlight& entry : std::as_const(m_inFlight))
    {
        m_submitter->cancel(entry.jobId);
    }
}

void PageSurfaceCoordinator::setDocumentKey(QString documentKey)
{
    m_documentKey = std::move(documentKey);
}

void PageSurfaceCoordinator::setRenderSettings(PageSurfaceRenderSettings settings)
{
    if (m_settings.features == settings.features && m_settings.colorOutputIdentity == settings.colorOutputIdentity)
    {
        return;
    }

    m_settings = std::move(settings);

    // Different pixels for the same page: every key built before this point is a
    // different key now, so nothing in flight is wanted any more.
    cancelInFlight();
    rebuildSnapshot();
}

void PageSurfaceCoordinator::onDemandChanged()
{
    // The viewport superseded prior demand. Requests that are still wanted are
    // resubmitted with the new generation by requestSurfaces(); the rest are
    // cancelled there too.
    requestSurfaces();
}

std::optional<PageSurfaceKey> PageSurfaceCoordinator::keyForPage(int pageIndex) const
{
    const QRect placedRect = m_viewport->placedPageRect(pageIndex);
    if (placedRect.isEmpty())
    {
        return std::nullopt;
    }

    const pdf::PDFRevisionIdentity revision = m_revisions->currentRevision();
    if (!revision.isValid())
    {
        return std::nullopt;
    }

    const qreal devicePixelRatio = m_viewport->devicePixelRatio();
    const QSize targetPixelSize(qRound(placedRect.width() * devicePixelRatio), qRound(placedRect.height() * devicePixelRatio));

    const QString colorOutputIdentity = m_authoritativePages.contains(pageIndex)
                                            ? withAuthoritativeOverprintMarker(m_settings.colorOutputIdentity)
                                            : m_settings.colorOutputIdentity;

    return makePageSurfaceKey(revision, pageIndex, m_viewport->rotation(), m_settings.features, colorOutputIdentity, m_viewport->zoom(), targetPixelSize, devicePixelRatio);
}

void PageSurfaceCoordinator::setPageAuthoritativeOverprint(int pageIndex, bool enabled)
{
    if (m_authoritativePages.contains(pageIndex) == enabled)
    {
        return;
    }

    if (enabled)
    {
        m_authoritativePages.insert(pageIndex);
    }
    else
    {
        m_authoritativePages.remove(pageIndex);
    }

    // keyForPage(pageIndex) now returns a different key, so requestSurfaces()'s
    // own coalescing cancels this page's stale in-flight request. Rebuild first
    // so an approximate snapshot is not presented as authoritative while the
    // accurate surface is pending (or vice versa when toggling back).
    rebuildSnapshot();
    requestSurfaces();
}

std::optional<pdf::PDFRenderDiagnostics> PageSurfaceCoordinator::diagnosticsForPage(int pageIndex) const
{
    const std::optional<PageSurfaceKey> wanted = keyForPage(pageIndex);
    if (!wanted.has_value())
    {
        return std::nullopt;
    }

    const auto it = m_cache.find(wanted.value());
    if (it == m_cache.end())
    {
        return std::nullopt;
    }

    return it->second.diagnostics;
}

void PageSurfaceCoordinator::requestSurfaces()
{
    const QList<int> visiblePages = m_viewport->visiblePages();
    const QList<int> activePages = m_viewport->activePages();
    const quint64 requestGeneration = m_viewport->requestGeneration();

    struct Demand
    {
        PageSurfaceKey key;
        pdf::PDFJobPriority priority;
    };

    QList<Demand> demand;
    demand.reserve(activePages.size());

    for (int pageIndex : activePages)
    {
        const std::optional<PageSurfaceKey> key = keyForPage(pageIndex);
        if (!key.has_value())
        {
            continue;
        }

        demand.push_back(Demand{ key.value(), visiblePages.contains(pageIndex) ? pdf::PDFJobPriority::VisiblePage : pdf::PDFJobPriority::NearViewport });
    }

    // Coalesce first: anything in flight for a key nobody wants any more is
    // cancelled before new work is submitted, so a superseded render never
    // occupies a worker that current demand needs.
    const QList<quint64> requestIds = m_inFlight.keys();
    for (quint64 requestId : requestIds)
    {
        const InFlight& entry = m_inFlight[requestId];
        const bool stillWanted = std::any_of(demand.cbegin(), demand.cend(), [&entry](const Demand& item)
                                             { return item.key == entry.key; });

        if (!stillWanted)
        {
            requestCancellation(requestId);
        }
    }

    bool snapshotDirty = false;
    bool shedRequested = false;

    for (const Demand& item : demand)
    {
        const auto cacheHit = m_cache.find(item.key);
        if (cacheHit != m_cache.end())
        {
            ++m_counters.cacheHitsExact;
            m_lru.splice(m_lru.begin(), m_lru, cacheHit->second.lru);
            snapshotDirty = true;
            continue;
        }

        const bool alreadyInFlight = std::any_of(m_inFlight.cbegin(), m_inFlight.cend(), [&item](const InFlight& entry)
                                                 { return entry.key == item.key; });
        if (alreadyInFlight)
        {
            continue;
        }

        ++m_counters.cacheMisses;

        const bool overRequestBound = static_cast<int>(m_inFlight.size()) >= m_bounds.maxInFlightRequests;
        const bool overByteBound = inFlightBytes() + estimatedBytes(item.key) > m_bounds.maxInFlightBytes;
        const bool overPrefetchBound = item.priority == pdf::PDFJobPriority::NearViewport && inFlightCount(pdf::PDFJobPriority::NearViewport) >= m_bounds.maxNearViewportRequests;

        if (overRequestBound || overByteBound || overPrefetchBound)
        {
            // Shed look-ahead before visible work, and tell the render path to
            // give memory back. Shedding is a counted, typed outcome, not a
            // silent drop.
            ++m_counters.shed;

            if (overByteBound && !shedRequested)
            {
                m_renderer->shedPrefetchAndQuality();
                shedRequested = true;
            }

            if (item.priority == pdf::PDFJobPriority::NearViewport)
            {
                continue;
            }

            if (overRequestBound || overByteBound)
            {
                // Visible work outranks prefetch: reclaim one prefetch slot for it.
                bool reclaimed = false;
                const QList<quint64> candidates = m_inFlight.keys();
                for (quint64 candidate : candidates)
                {
                    if (m_inFlight[candidate].priority == pdf::PDFJobPriority::NearViewport)
                    {
                        // Dropped now rather than on a queued resolve: the slot is
                        // needed for this submission. If the prefetch was already
                        // running, its completion arrives unregistered and admit()
                        // rejects it.
                        cancelAndDrop(candidate);
                        reclaimed = true;
                        break;
                    }
                }

                if (!reclaimed)
                {
                    if (item.priority == pdf::PDFJobPriority::VisiblePage)
                    {
                        m_retrySurfaceRequest = true;
                    }
                    continue;
                }
            }
        }

        PageSurfaceRequest request;
        request.key = item.key;
        request.token = RevisionFencedToken{ requestGeneration, item.key.revision };
        request.priority = item.priority;
        submit(request);
    }

    if (snapshotDirty)
    {
        rebuildSnapshot();
    }

    if (m_retrySurfaceRequest)
    {
        scheduleSurfaceRetry();
    }
}

void PageSurfaceCoordinator::submit(const PageSurfaceRequest& request)
{
    const std::shared_ptr<JobRelay> relay = m_relay;
    IPageSurfaceRenderer* renderer = m_renderer;
    const auto workStarted = std::make_shared<std::atomic_bool>(false);
    const quint64 requestId = ++m_requestSequence;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Rendering;
    spec.priority = request.priority;
    spec.documentKey = m_documentKey;
    spec.documentRevision = request.key.revision.toString();
    spec.operationId = QStringLiteral("page-surface");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    const QString jobId = m_submitter->submit(
        spec,
        [this, relay, renderer, request, requestId, workStarted](pdf::PDFJobContext& jobContext)
        {
            workStarted->store(true, std::memory_order_release);

            PageSurfaceResult result = renderer->render(request, jobContext);

            // The relay is always queued, so this runs after submit() returned and
            // registered the request, even when the submitter ran the work inline.
            relay->post([this, requestId, result = std::move(result)]() mutable
                        { admit(requestId, std::move(result)); });
        });

    if (jobId.isEmpty())
    {
        // The submitter refused the job. Nothing is in flight and nothing will
        // report, so there is no entry to register.
        ++m_counters.failed;
        Q_EMIT surfaceTerminal(request.key, SurfaceTerminalState::Failed);
        return;
    }

    ++m_counters.requested;
    m_inFlight.insert(requestId, InFlight{ jobId, request.key, request.token, request.priority, workStarted });
    m_counters.inFlight = static_cast<int>(m_inFlight.size());
}

void PageSurfaceCoordinator::countTerminal(SurfaceTerminalState state)
{
    switch (state)
    {
        case SurfaceTerminalState::Complete:
            break;
        case SurfaceTerminalState::Cancelled:
            ++m_counters.cancelled;
            break;
        case SurfaceTerminalState::Failed:
            ++m_counters.failed;
            break;
        case SurfaceTerminalState::Stale:
            ++m_counters.stale;
            break;
        case SurfaceTerminalState::BudgetExhausted:
            ++m_counters.budgetExhausted;
            break;
    }
}

bool PageSurfaceCoordinator::removeInFlight(quint64 requestId)
{
    if (m_inFlight.remove(requestId) == 0)
    {
        return false;
    }

    m_counters.inFlight = static_cast<int>(m_inFlight.size());
    return true;
}

void PageSurfaceCoordinator::finishInFlight(quint64 requestId, SurfaceTerminalState state)
{
    const auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end())
    {
        return;
    }

    const PageSurfaceKey key = it->key;
    removeInFlight(requestId);

    countTerminal(state);
    Q_EMIT surfaceTerminal(key, state);
    scheduleSurfaceRetry();
}

void PageSurfaceCoordinator::scheduleSurfaceRetry()
{
    if (!m_retrySurfaceRequest)
    {
        return;
    }

    m_retrySurfaceRequest = false;
    m_relay->post([this]()
                  { requestSurfaces(); });
}

void PageSurfaceCoordinator::requestCancellation(quint64 requestId)
{
    const auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end())
    {
        return;
    }

    const QString jobId = it->jobId;
    const std::shared_ptr<std::atomic_bool> workStarted = it->workStarted;
    m_submitter->cancel(jobId);

    // Queued rather than resolved inline: work that is already running still
    // reports its own terminal state, and that report must be allowed to arrive
    // first.
    m_relay->post([this, requestId, workStarted]()
                  { resolveCancellation(requestId, workStarted); });
}

void PageSurfaceCoordinator::cancelAndDrop(quint64 requestId)
{
    const auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end())
    {
        return;
    }

    m_submitter->cancel(it->jobId);
    finishInFlight(requestId, SurfaceTerminalState::Cancelled);
}

void PageSurfaceCoordinator::resolveCancellation(quint64 requestId, std::shared_ptr<std::atomic_bool> workStarted)
{
    const auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end())
    {
        return;
    }

    if (workStarted && workStarted->load(std::memory_order_acquire))
    {
        // Already running: it reports its own terminal state.
        return;
    }

    const pdf::PDFJobSnapshot snapshot = m_submitter->snapshot(it->jobId);
    if (snapshot.status == pdf::PDFJobStatus::Queued || snapshot.status == pdf::PDFJobStatus::Running)
    {
        m_relay->post([this, requestId, workStarted]()
                      { resolveCancellation(requestId, workStarted); });
        return;
    }

    // Terminal without the work ever starting: the scheduler dropped it from the
    // queue, so no completion is coming and this entry would otherwise hold a
    // slot forever.
    finishInFlight(requestId, SurfaceTerminalState::Cancelled);
}

void PageSurfaceCoordinator::admit(quint64 requestId, PageSurfaceResult result)
{
    if (!removeInFlight(requestId))
    {
        // The request was cancelled or coalesced away while this was rendering.
        // It is counted and dropped; a completion nobody is waiting for must not
        // be able to win a race against the one that replaced it.
        ++m_counters.rejectedSuperseded;
        Q_EMIT surfaceTerminal(result.key, SurfaceTerminalState::Stale);
        return;
    }

    if (result.state != SurfaceTerminalState::Complete)
    {
        resetAuthoritativePageAfterFailure(result.key, result.state);
        countTerminal(result.state);
        Q_EMIT surfaceTerminal(result.key, result.state);
        return;
    }

    if (result.token.generation != m_viewport->requestGeneration())
    {
        // Superseded demand: the viewport asked for something else before this
        // finished.
        ++m_counters.rejectedSuperseded;
        Q_EMIT surfaceTerminal(result.key, SurfaceTerminalState::Stale);
        return;
    }

    if (!m_revisions->isCurrent(result.key.revision))
    {
        ++m_counters.rejectedStaleRevision;
        Q_EMIT surfaceTerminal(result.key, SurfaceTerminalState::Stale);
        return;
    }

    const std::optional<PageSurfaceKey> wanted = keyForPage(result.key.pageIndex);
    if (!wanted.has_value() || !(wanted.value() == result.key))
    {
        // The key no longer describes what the viewport wants: a rotation, zoom
        // or resize landed while this was rendering.
        ++m_counters.rejectedDemand;
        Q_EMIT surfaceTerminal(result.key, SurfaceTerminalState::Stale);
        return;
    }

    if (!result.pixels || result.pixels->byteSize <= 0)
    {
        resetAuthoritativePageAfterFailure(result.key, SurfaceTerminalState::Failed);
        ++m_counters.failed;
        Q_EMIT surfaceTerminal(result.key, SurfaceTerminalState::Failed);
        return;
    }

    if (!insertIntoCache(result.key, result.pixels, result.diagnostics))
    {
        resetAuthoritativePageAfterFailure(result.key, SurfaceTerminalState::Failed);
        ++m_counters.rejectedOversize;
        Q_EMIT surfaceTerminal(result.key, SurfaceTerminalState::Failed);
        return;
    }

    ++m_counters.admitted;
    Q_EMIT surfaceTerminal(result.key, SurfaceTerminalState::Complete);
    rebuildSnapshot();
}

bool PageSurfaceCoordinator::insertIntoCache(const PageSurfaceKey& key, SurfaceBufferPointer pixels, pdf::PDFRenderDiagnostics diagnostics)
{
    if (!pixels || pixels->byteSize <= 0)
    {
        return false;
    }

    if (pixels->byteSize > m_bounds.maxAdmittedBytes)
    {
        // An entry that cannot fit is refused outright. Evicting the whole cache
        // to make room for something that still would not leave space is worse
        // than not caching it.
        return false;
    }

    const auto existing = m_cache.find(key);
    if (existing != m_cache.end())
    {
        m_counters.admittedBytes -= existing->second.cost;
        m_lru.erase(existing->second.lru);
        m_cache.erase(existing);
    }

    CacheEntry entry;
    entry.pixels = std::move(pixels);
    entry.diagnostics = std::move(diagnostics);
    entry.cost = entry.pixels->byteSize;
    entry.accessSequence = ++m_accessSequence;

    m_counters.admittedBytes += entry.cost;
    m_lru.push_front(key);
    entry.lru = m_lru.begin();
    m_cache.emplace(key, std::move(entry));

    trimCacheToBudget();

    m_counters.admittedBytesHighWater = qMax(m_counters.admittedBytesHighWater, m_counters.admittedBytes);

    return m_cache.find(key) != m_cache.end();
}

void PageSurfaceCoordinator::trimCacheToBudget()
{
    bool snapshotDirty = false;
    while (m_counters.admittedBytes > m_bounds.maxAdmittedBytes && !m_cache.empty())
    {
        const PageSurfaceKey oldestKey = m_lru.back();
        const auto oldest = m_cache.find(oldestKey);
        if (oldest == m_cache.end())
        {
            m_lru.pop_back();
            continue;
        }

        m_counters.admittedBytes -= oldest->second.cost;
        m_cache.erase(oldest);
        m_lru.pop_back();
        ++m_counters.evictions;
        snapshotDirty = true;
    }

    if (snapshotDirty)
    {
        // CacheEntry owns the immutable pixel buffer shared by the snapshot.
        // Rebuild immediately so the snapshot cannot keep presenting an entry
        // that admission has already released from the cache budget.
        rebuildSnapshot();
    }
}

void PageSurfaceCoordinator::resetAuthoritativePageAfterFailure(const PageSurfaceKey& key, SurfaceTerminalState state)
{
    if (state != SurfaceTerminalState::Failed && state != SurfaceTerminalState::BudgetExhausted)
    {
        return;
    }

    if (!hasAuthoritativeOverprintMarker(key.colorOutputIdentity) || !m_authoritativePages.contains(key.pageIndex))
    {
        return;
    }

    // The accurate request is terminally unsuccessful. Drop the intent and
    // return to the cached fast surface, if one exists, instead of claiming
    // that an approximation is authoritative.
    m_authoritativePages.remove(key.pageIndex);
    rebuildSnapshot();
    requestSurfaces();
}

qint64 PageSurfaceCoordinator::inFlightBytes() const
{
    qint64 bytes = 0;
    for (const InFlight& entry : m_inFlight)
    {
        bytes += estimatedBytes(entry.key);
    }

    return bytes;
}

int PageSurfaceCoordinator::inFlightCount(pdf::PDFJobPriority priority) const
{
    int count = 0;
    for (const InFlight& entry : m_inFlight)
    {
        if (entry.priority == priority)
        {
            ++count;
        }
    }

    return count;
}

void PageSurfaceCoordinator::cancelInFlight()
{
    ++m_generation;

    const QList<quint64> requestIds = m_inFlight.keys();
    for (quint64 requestId : requestIds)
    {
        m_submitter->cancel(m_inFlight[requestId].jobId);
        finishInFlight(requestId, SurfaceTerminalState::Cancelled);
    }
}

pdf::PDFRevisionIdentity PageSurfaceCoordinator::currentRevision() const
{
    return m_revisions->currentRevision();
}

void PageSurfaceCoordinator::invalidate(const pdf::PDFRevisionIdentity& current)
{
    cancelInFlight();

    // Both production call sites (DocumentViewSession::prepareDocumentView,
    // ::clearDocumentView) are document open/close boundaries, never a
    // same-document edit -- so a page index toggled authoritative in one
    // document must not silently carry over onto the same page index in
    // whatever opens next.
    m_authoritativePages.clear();

    // Revision-selective rather than a blanket clear: surfaces rendered for the
    // state that is still current stay usable, and only they do.
    for (auto it = m_cache.begin(); it != m_cache.end();)
    {
        if (!(it->first.revision == current))
        {
            m_counters.admittedBytes -= it->second.cost;
            m_lru.erase(it->second.lru);
            it = m_cache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    rebuildSnapshot();
}

void PageSurfaceCoordinator::rebuildSnapshot()
{
    CanvasSnapshot snapshot;
    snapshot.token = RevisionFencedToken{ m_viewport->requestGeneration(), m_revisions->currentRevision() };

    for (int pageIndex : m_viewport->visiblePages())
    {
        const std::optional<PageSurfaceKey> wanted = keyForPage(pageIndex);
        if (!wanted.has_value())
        {
            continue;
        }

        const auto exact = m_cache.find(wanted.value());
        if (exact != m_cache.end())
        {
            exact->second.accessSequence = ++m_accessSequence;
            snapshot.tiles.push_back(CanvasTile{ exact->first, exact->second.pixels, m_viewport->placedPageRect(pageIndex), true });
            continue;
        }

        // No exact surface yet. A compatible one -- same document state, page,
        // rotation and output path, different resolution -- may be scaled into
        // place so a pan or a zoom keeps showing the page while the fidelity
        // render is in flight. This is issue #142's continuous-correspondence
        // requirement, and it is the only substitution allowed.
        const CacheEntry* best = nullptr;
        const PageSurfaceKey* bestKey = nullptr;
        int bestDistance = 0;

        for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
        {
            if (!it->first.compatibleWith(wanted.value()))
            {
                continue;
            }

            const int distance = qAbs(it->first.zoomBucket - wanted->zoomBucket);
            if (!best || distance < bestDistance)
            {
                best = &it->second;
                bestKey = &it->first;
                bestDistance = distance;
            }
        }

        if (best && bestKey)
        {
            ++m_counters.cacheHitsInexact;
            snapshot.tiles.push_back(CanvasTile{ *bestKey, best->pixels, m_viewport->placedPageRect(pageIndex), false });
        }
    }

    m_snapshot = std::move(snapshot);
    Q_EMIT snapshotChanged(m_snapshot.token.generation);
}

}   // namespace pdfinteraction
