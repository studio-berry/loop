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

#ifndef PAGESURFACECOORDINATOR_H
#define PAGESURFACECOORDINATOR_H

#include "canvassnapshot.h"
#include "documentcontextsource.h"
#include "jobrelay.h"
#include "jobsubmitter.h"
#include "pagesurfacerenderer.h"
#include "pdfpagecachebudget.h"
#include "pdfresourcebudget.h"
#include "renderpresentationpolicy.h"
#include "viewportcontroller.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <list>
#include <unordered_map>

namespace pdfinteraction
{

using PageSurfaceRenderSettings = RenderPresentationPolicy;

/// Hard limits, pre-registered rather than discovered under load.
struct PageSurfaceBounds
{
    /// Renders in flight at once, all priorities.
    int maxInFlightRequests = 8;

    /// Of those, how many may be prefetch. Interactive work must never be
    /// crowded out by look-ahead.
    int maxNearViewportRequests = 2;

    /// Estimated bytes of the renders in flight.
    qint64 maxInFlightBytes = 64LL * 1024 * 1024;

    /// Bytes of admitted surfaces held for reuse. In production this is a
    /// diagnostic projection of the shared PDFPageCacheBudget; standalone
    /// coordinators may still use it as their local fallback bound.
    qint64 maxAdmittedBytes = 128LL * 1024 * 1024;

    static PageSurfaceBounds conservativeDefaults() { return PageSurfaceBounds(); }
};

/// Everything a diagnostic needs, and what the tests assert against.
///
/// The rejection counters are the interesting ones: they say how often the
/// admission rule fired, and which clause fired. Missing telemetry stays absent
/// rather than being reported as zero work.
struct PageSurfaceCounters
{
    int requested = 0;
    int inFlight = 0;
    int admitted = 0;
    int rejectedSuperseded = 0;
    int rejectedStaleRevision = 0;
    int rejectedDemand = 0;
    int rejectedOversize = 0;
    int stale = 0;
    int cancelled = 0;
    int failed = 0;
    int budgetExhausted = 0;
    int shed = 0;
    int evictions = 0;
    int cacheHitsExact = 0;
    int cacheHitsInexact = 0;
    int cacheMisses = 0;
    qint64 admittedBytes = 0;
    qint64 admittedBytesHighWater = 0;
};

/// Derives page-surface demand from the viewport, runs it on the one scheduler,
/// and admits results on the owner thread under the complete key.
///
/// It owns no renderer, no document, no queue and no thread. pdf::PDFJobScheduler
/// stays the only scheduler (reached through IJobSubmitter),
/// pdf::PDFDocumentContext stays the revision fence (reached through
/// IDocumentRevisionSource), and the actual rasterization is
/// IPageSurfaceRenderer's problem.
///
/// The admission rule is the whole point of the class. A completed render enters
/// the snapshot only when all five hold: the request is still registered, so a
/// cancelled or coalesced one cannot arrive late and win; the request generation
/// is still the viewport's; the revision is still current; the key still equals
/// what the viewport wants for that page now; and the byte budget can hold the
/// surface. Anything else is counted and dropped. A tile from a superseded state
/// is never patched into the current frame, at any priority, for any reason.
class PageSurfaceCoordinator final : public QObject
{
    Q_OBJECT

public:
    PageSurfaceCoordinator(IDocumentRevisionSource& revisions,
                           IJobSubmitter& submitter,
                           IPageSurfaceRenderer& renderer,
                           ViewportController& viewport,
                           PageSurfaceBounds bounds = PageSurfaceBounds::conservativeDefaults(),
                           QObject* parent = nullptr);

    ~PageSurfaceCoordinator() override;

    PageSurfaceCoordinator(const PageSurfaceCoordinator&) = delete;
    PageSurfaceCoordinator& operator=(const PageSurfaceCoordinator&) = delete;

    /// The document key the lifecycle published the scheduler's revision fence
    /// under. Empty means the scheduler cannot fence this work -- a key with no
    /// entry is never stale -- and only this class's own admission applies.
    void setDocumentKey(QString documentKey);
    QString documentKey() const { return m_documentKey; }

    void setRenderSettings(PageSurfaceRenderSettings settings);
    const PageSurfaceRenderSettings& renderSettings() const noexcept { return m_settings; }

    /// Diagnostic projection of the current budget partition. Prefer
    /// cacheLimit()/setCacheLimit() as the authority; maxAdmittedBytes is
    /// derived from the total via pdf::PDFPageCacheBudget::pageSurfaces().
    /// Requests (or releases) the authoritative, overprint-accurate render of
    /// one page instead of the fast approximate one. Idempotent. The page gets
    /// its own cache slot (see withAuthoritativeOverprintMarker), so toggling
    /// it neither invalidates nor is served by the approximate surface already
    /// cached for the same page.
    void setPageAuthoritativeOverprint(int pageIndex, bool enabled);
    bool isPageAuthoritativeOverprint(int pageIndex) const { return m_authoritativePages.contains(pageIndex); }

    /// Diagnostics for the surface currently admitted for \p pageIndex, if any.
    /// Reflects whichever render path actually produced that surface -- the
    /// standard path's cached-flag approximation, or the authoritative
    /// renderer's own verdict.
    std::optional<pdf::PDFRenderDiagnostics> diagnosticsForPage(int pageIndex) const;

    const PageSurfaceBounds& bounds() const noexcept { return m_bounds; }

    /// Receives the production total cache budget. The shared object is the
    /// authority for both compiled pages and admitted surfaces.
    void setCacheLimit(qsizetype totalBytes);
    qsizetype cacheLimit() const noexcept
    {
        return m_pageCacheBudget ? m_pageCacheBudget->total() : m_cacheLimit;
    }

    /// Attaches the document session's shared resource authority. Existing
    /// admitted surfaces are dropped when the authority changes so the new
    /// authority never starts with an unaccounted resident cache.
    void setResourceBudget(std::shared_ptr<pdf::PDFResourceBudget> budget);
    pdf::PDFResourceBudget* resourceBudget() const noexcept { return m_resourceBudget.get(); }
    std::shared_ptr<pdf::PDFResourceBudget> sharedResourceBudget() const noexcept { return m_resourceBudget; }

    /// bounds() is derived from the total via PDFPageCacheBudget partition;
    /// prefer cacheLimit()/setCacheLimit() as the authority and treat
    /// PageSurfaceBounds::maxAdmittedBytes as the surface half.
    void setPageCacheBudget(std::shared_ptr<pdf::PDFPageCacheBudget> budget);
    std::shared_ptr<pdf::PDFPageCacheBudget> sharedPageCacheBudget() const noexcept { return m_pageCacheBudget; }
    /// Refreshes the diagnostic surface projection and trims after the shared
    /// authority's total changes.
    void refreshPageCacheBudget();

    /// Submits what the viewport wants and cancels what it no longer wants.
    /// Idempotent: calling it twice with an unchanged viewport submits nothing.
    void requestSurfaces();

    /// Cancels everything in flight and supersedes it. The cache survives -- a
    /// cancelled request says nothing about the pixels already admitted.
    void cancelInFlight();

    /// A new document state. Supersedes everything in flight and drops every
    /// cached surface that was not rendered for `current`.
    void invalidate(const pdf::PDFRevisionIdentity& current);

    /// How many times demand has been superseded wholesale -- a cancel, a render
    /// settings change, or a new document state. Exposed for diagnostics; the
    /// per-result fence is the registration and key check in admit().
    quint64 generation() const noexcept { return m_generation; }

    const CanvasSnapshot& snapshot() const noexcept { return m_snapshot; }
    const PageSurfaceCounters& counters() const noexcept { return m_counters; }

    /// The revision the fence is currently against, read from the one
    /// IDocumentRevisionSource this class already holds.
    ///
    /// It exists so a presentation host can refuse to draw a snapshot or an
    /// overlay frame from a superseded document without inventing a second
    /// revision truth of its own. The host is not the fence -- admit() is -- but
    /// a host that has retained scene-graph nodes needs to be able to check.
    pdf::PDFRevisionIdentity currentRevision() const;

signals:
    void snapshotChanged(quint64 requestGeneration);
    void surfaceTerminal(pdfinteraction::PageSurfaceKey key, pdfinteraction::SurfaceTerminalState state);

private:
    /// Keyed by a request id of this class's own, not by the scheduler's job id.
    /// A synchronous submitter can run the work before submit() returns, and the
    /// job id is only known afterwards; the request id exists before the work
    /// does, so the completion path never depends on submission having finished.
    struct InFlight
    {
        QString jobId;
        PageSurfaceKey key;
        RevisionFencedToken token;
        pdf::PDFJobPriority priority = pdf::PDFJobPriority::VisiblePage;
        std::shared_ptr<std::atomic_bool> workStarted;
        std::shared_ptr<pdf::PDFResourceReservation> resourceReservation;
    };

    struct CacheEntry
    {
        SurfaceBufferPointer pixels;
        std::shared_ptr<pdf::PDFResourceReservation> resourceReservation;
        pdf::PDFRenderDiagnostics diagnostics{};
        qint64 cost = 0;
        quint64 accessSequence = 0;
        std::list<PageSurfaceKey>::iterator lru{};
    };

    struct PageSurfaceKeyHash
    {
        size_t operator()(const PageSurfaceKey& key) const noexcept
        {
            size_t hash = qHash(key.revision.toString());
            hash = hash * 31U + qHash(key.pageIndex);
            hash = hash * 31U + qHash(static_cast<int>(key.rotation));
            hash = hash * 31U + qHash(key.featureBits);
            hash = hash * 31U + qHash(key.colorOutputIdentity);
            hash = hash * 31U + qHash(key.zoomBucket);
            hash = hash * 31U + qHash(key.targetPixelSize.width());
            hash = hash * 31U + qHash(key.targetPixelSize.height());
            hash = hash * 31U + qHash(key.devicePixelRatio1000);
            return hash;
        }
    };

    void onDemandChanged();
    void submit(const PageSurfaceRequest& request);
    void admit(quint64 requestId,
               PageSurfaceResult result,
               std::shared_ptr<pdf::PDFResourceReservation> resourceReservation);
    void requestCancellation(quint64 requestId);
    void cancelAndDrop(quint64 requestId);
    void resolveCancellation(quint64 requestId, std::shared_ptr<std::atomic_bool> workStarted);
    bool removeInFlight(quint64 requestId);
    void finishInFlight(quint64 requestId, SurfaceTerminalState state);

    std::optional<PageSurfaceKey> keyForPage(int pageIndex) const;
    bool insertIntoCache(const PageSurfaceKey& key,
                         SurfaceBufferPointer pixels,
                         std::shared_ptr<pdf::PDFResourceReservation> resourceReservation,
                         pdf::PDFRenderDiagnostics diagnostics);
    bool evictOldestCacheEntry();
    void trimCacheForIncoming(qsizetype bytes);
    bool trimCacheToBudget();
    void clearCache();
    qint64 inFlightBytes() const;
    int inFlightCount(pdf::PDFJobPriority priority) const;
    void rebuildSnapshot();
    void countTerminal(SurfaceTerminalState state);
    void scheduleSurfaceRetry();
    void resetAuthoritativePageAfterFailure(const PageSurfaceKey& key, SurfaceTerminalState state);

    IJobSubmitter* m_submitter = nullptr;
    IPageSurfaceRenderer* m_renderer = nullptr;
    IDocumentRevisionSource* m_revisions = nullptr;
    ViewportController* m_viewport = nullptr;

    PageSurfaceBounds m_bounds;
    qsizetype m_cacheLimit = 0;   // normalized total received from the production authority
    std::shared_ptr<pdf::PDFPageCacheBudget> m_pageCacheBudget;
    PageSurfaceRenderSettings m_settings;
    QString m_documentKey;
    std::shared_ptr<pdf::PDFResourceBudget> m_resourceBudget;
    QSet<int> m_authoritativePages;

    std::shared_ptr<JobRelay> m_relay;

    quint64 m_generation = 1;
    quint64 m_accessSequence = 0;
    quint64 m_requestSequence = 0;

    QHash<quint64, InFlight> m_inFlight;
    std::unordered_map<PageSurfaceKey, CacheEntry, PageSurfaceKeyHash> m_cache;
    std::list<PageSurfaceKey> m_lru;

    CanvasSnapshot m_snapshot;
    PageSurfaceCounters m_counters;
    bool m_retrySurfaceRequest = false;
};

}   // namespace pdfinteraction

#endif   // PAGESURFACECOORDINATOR_H
