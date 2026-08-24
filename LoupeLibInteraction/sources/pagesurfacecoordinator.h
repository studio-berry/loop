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
#include "viewportcontroller.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <optional>

namespace pdfinteraction
{

/// What the render path is configured to produce. Set by the owner, never
/// derived here: the coordinator must not reconfigure the session (see
/// PDFSessionPageSurfaceRenderer::render for why a worker changing renderer
/// features invalidates every in-flight key).
struct PageSurfaceRenderSettings
{
    pdf::PDFRenderer::Features features = pdf::PDFRenderer::getDefaultFeatures();

    /// Identity of the colour-managed output path in force. Opaque to the
    /// coordinator; it only has to change whenever the pixels would.
    QString colorOutputIdentity;
};

/// Hard limits, pre-registered rather than discovered under load.
struct PageSurfaceBounds
{
    /// Renders in flight at once, all priorities.
    int maxInFlightRequests = 8;

    /// Of those, how many may be prefetch. Interactive work must never be
    /// crowded out by look-ahead.
    int maxNearViewportRequests = 2;

    /// Estimated bytes of the renders in flight.
    qint64 maxInFlightBytes = 64ll * 1024 * 1024;

    /// Bytes of admitted surfaces held for reuse.
    qint64 maxAdmittedBytes = 128ll * 1024 * 1024;

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

    const PageSurfaceBounds& bounds() const noexcept { return m_bounds; }

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
    };

    struct CacheEntry
    {
        SurfaceBufferPointer pixels;
        qint64 cost = 0;
        quint64 accessSequence = 0;
    };

    void onDemandChanged();
    void submit(const PageSurfaceRequest& request);
    void admit(quint64 requestId, PageSurfaceResult result);
    void requestCancellation(quint64 requestId);
    void cancelAndDrop(quint64 requestId);
    void resolveCancellation(quint64 requestId, std::shared_ptr<std::atomic_bool> workStarted);
    bool removeInFlight(quint64 requestId);
    void finishInFlight(quint64 requestId, SurfaceTerminalState state);

    std::optional<PageSurfaceKey> keyForPage(int pageIndex) const;
    bool insertIntoCache(const PageSurfaceKey& key, SurfaceBufferPointer pixels);
    void trimCacheToBudget();
    qint64 inFlightBytes() const;
    int inFlightCount(pdf::PDFJobPriority priority) const;
    void rebuildSnapshot();
    void countTerminal(SurfaceTerminalState state);

    IJobSubmitter* m_submitter = nullptr;
    IPageSurfaceRenderer* m_renderer = nullptr;
    IDocumentRevisionSource* m_revisions = nullptr;
    ViewportController* m_viewport = nullptr;

    PageSurfaceBounds m_bounds;
    PageSurfaceRenderSettings m_settings;
    QString m_documentKey;

    std::shared_ptr<JobRelay> m_relay;

    quint64 m_generation = 1;
    quint64 m_accessSequence = 0;
    quint64 m_requestSequence = 0;

    QHash<quint64, InFlight> m_inFlight;
    std::map<PageSurfaceKey, CacheEntry> m_cache;

    CanvasSnapshot m_snapshot;
    PageSurfaceCounters m_counters;
};

}   // namespace pdfinteraction

#endif   // PAGESURFACECOORDINATOR_H
