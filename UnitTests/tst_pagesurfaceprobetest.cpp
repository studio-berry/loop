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

#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfdrawwidget.h"
#include "pdfdrawspacecontroller.h"
#include "pdfinteractiontrace_p.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QPainter>
#include <QSignalSpy>

#include <algorithm>
#include <optional>
#include <vector>

#include <QtTest>

namespace
{

enum class ProbePriority
{
    Interaction,
    VisiblePage,
    NearViewport
};

struct PageSurfaceKey
{
    pdf::PDFRevisionIdentity revision;
    pdf::PDFInteger pageIndex = -1;
    int rotation = 0;
    int zoomBucket = 0;
    QSize targetPixelSize;
    QString renderMode;
    QString colorOutput;
    int devicePixelRatio1000 = 1000;

    bool operator==(const PageSurfaceKey&) const = default;

    bool compatibleWith(const PageSurfaceKey& desired) const
    {
        return revision == desired.revision && pageIndex == desired.pageIndex && rotation == desired.rotation
            && renderMode == desired.renderMode && colorOutput == desired.colorOutput
            && devicePixelRatio1000 == desired.devicePixelRatio1000;
    }
};

struct RequestIdentity
{
    PageSurfaceKey key;
    quint64 generation = 0;

    bool operator==(const RequestIdentity&) const = default;
};

class SurfaceProbeCache final
{
public:
    explicit SurfaceProbeCache(qsizetype byteBudget) : m_byteBudget(byteBudget) { }

    void setVisiblePageCount(int count) { m_visiblePageCount = qMax(1, count); }

    std::optional<RequestIdentity> request(const PageSurfaceKey& key, ProbePriority priority)
    {
        const bool supersedesInteractive = m_current.has_value()
            && (priority == ProbePriority::Interaction || priority == ProbePriority::VisiblePage);
        if (!supersedesInteractive && !enqueue(priority))
        {
            return std::nullopt;
        }

        RequestIdentity request { key, ++m_generation };
        m_current = request;
        return request;
    }

    bool enqueue(ProbePriority priority)
    {
        switch (priority)
        {
            case ProbePriority::Interaction:
            case ProbePriority::VisiblePage:
            {
                if (m_interactivePending >= m_visiblePageCount)
                {
                    ++m_shedRequests;
                    return false;
                }
                ++m_interactivePending;
                return true;
            }

            case ProbePriority::NearViewport:
            {
                if (m_nearViewportPending >= 2)
                {
                    ++m_shedRequests;
                    return false;
                }
                ++m_nearViewportPending;
                return true;
            }
        }

        Q_UNREACHABLE_RETURN(false);
    }

    void finish(ProbePriority priority)
    {
        switch (priority)
        {
            case ProbePriority::Interaction:
            case ProbePriority::VisiblePage:
                m_interactivePending = qMax(0, m_interactivePending - 1);
                break;
            case ProbePriority::NearViewport:
                m_nearViewportPending = qMax(0, m_nearViewportPending - 1);
                break;
        }
    }

    bool accepts(const RequestIdentity& request) const
    {
        return m_current.has_value() && m_current.value() == request;
    }

    void cancel()
    {
        ++m_generation;
        m_current.reset();
        ++m_cancellations;
    }

    void invalidate(const pdf::PDFRevisionIdentity& revision)
    {
        ++m_generation;
        m_current.reset();
        ++m_invalidations;
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&revision](const Entry& entry)
                                       { return entry.key.revision != revision; }),
                        m_entries.end());
        recalculateCost();
    }

    bool insert(const RequestIdentity& request, QImage image)
    {
        if (!accepts(request))
        {
            ++m_staleResults;
            return false;
        }

        const qsizetype cost = image.sizeInBytes();
        if (cost <= 0 || cost > m_byteBudget)
        {
            ++m_rejectedEntries;
            return false;
        }

        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        {
            if (it->key == request.key)
            {
                m_currentCost -= it->cost;
                m_entries.erase(it);
                break;
            }
        }

        while (m_currentCost + cost > m_byteBudget && !m_entries.empty())
        {
            const auto oldest = std::min_element(m_entries.begin(), m_entries.end(), [](const Entry& left, const Entry& right)
                                                 { return left.lastAccess < right.lastAccess; });
            m_currentCost -= oldest->cost;
            m_entries.erase(oldest);
            ++m_evictions;
        }

        m_entries.push_back(Entry { request.key, std::move(image), cost, ++m_accessCounter });
        m_currentCost += cost;
        return true;
    }

    const QImage* bestMatch(const PageSurfaceKey& desired)
    {
        Entry* best = nullptr;
        for (Entry& entry : m_entries)
        {
            if (!entry.key.compatibleWith(desired))
            {
                continue;
            }

            if (!best || qAbs(entry.key.zoomBucket - desired.zoomBucket) < qAbs(best->key.zoomBucket - desired.zoomBucket)
                || (qAbs(entry.key.zoomBucket - desired.zoomBucket) == qAbs(best->key.zoomBucket - desired.zoomBucket)
                    && entry.lastAccess > best->lastAccess))
            {
                best = &entry;
            }
        }

        if (!best)
        {
            ++m_cacheMisses;
            return nullptr;
        }

        best->lastAccess = ++m_accessCounter;
        ++m_cacheHits;
        return &best->image;
    }

    qsizetype currentCost() const { return m_currentCost; }
    qsizetype byteBudget() const { return m_byteBudget; }
    int evictions() const { return m_evictions; }
    int rejectedEntries() const { return m_rejectedEntries; }
    int staleResults() const { return m_staleResults; }
    int cacheHits() const { return m_cacheHits; }
    int cacheMisses() const { return m_cacheMisses; }
    int shedRequests() const { return m_shedRequests; }
    int cancellations() const { return m_cancellations; }
    int invalidations() const { return m_invalidations; }

private:
    struct Entry
    {
        PageSurfaceKey key;
        QImage image;
        qsizetype cost = 0;
        quint64 lastAccess = 0;
    };

    void recalculateCost()
    {
        m_currentCost = 0;
        for (const Entry& entry : m_entries)
        {
            m_currentCost += entry.cost;
        }
    }

    qsizetype m_byteBudget = 0;
    qsizetype m_currentCost = 0;
    int m_visiblePageCount = 1;
    int m_interactivePending = 0;
    int m_nearViewportPending = 0;
    quint64 m_generation = 0;
    quint64 m_accessCounter = 0;
    std::optional<RequestIdentity> m_current;
    std::vector<Entry> m_entries;
    int m_evictions = 0;
    int m_rejectedEntries = 0;
    int m_staleResults = 0;
    int m_cacheHits = 0;
    int m_cacheMisses = 0;
    int m_shedRequests = 0;
    int m_cancellations = 0;
    int m_invalidations = 0;
};

QImage renderWidget(pdf::PDFDrawWidgetProxy* proxy, QSize size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    proxy->draw(&painter, QRect(QPoint(0, 0), size));
    return image;
}

PageSurfaceKey makeKey(const pdf::PDFRevisionIdentity& revision, int zoomBucket = 100)
{
    PageSurfaceKey key;
    key.revision = revision;
    key.pageIndex = 0;
    key.rotation = 0;
    key.zoomBucket = zoomBucket;
    key.targetPixelSize = QSize(160, 120);
    key.renderMode = QStringLiteral("editor");
    key.colorOutput = QStringLiteral("document");
    key.devicePixelRatio1000 = 1000;
    return key;
}

class PageSurfaceProbeTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void reusesCompatibleSurfaceDuringZoom();
    void rejectsSupersededAndRevisionConflictingResults();
    void enforcesBudgetAndPriorityShedding();
    void rendersThroughExistingWidgetsPath();

private:
    std::unique_ptr<pdf::PDFCMSManager> m_cmsManager;
    std::unique_ptr<pdf::PDFWidget> m_widget;
    std::unique_ptr<pdf::PDFDocument> m_document;
};

void PageSurfaceProbeTest::init()
{
    m_cmsManager = std::make_unique<pdf::PDFCMSManager>(nullptr);
    // Match the editor's default renderer so the production-path regression
    // exercises the shipped configuration rather than only the sequential
    // Blend2D variant.
    m_widget = std::make_unique<pdf::PDFWidget>(m_cmsManager.get(),
                                                pdf::RendererEngine::Blend2D_MultiThread,
                                                nullptr);
    m_widget->resize(320, 240);
    m_widget->show();
    QCoreApplication::processEvents();
}

void PageSurfaceProbeTest::cleanup()
{
    m_widget.reset();
    m_document.reset();
    m_cmsManager.reset();
}

void PageSurfaceProbeTest::reusesCompatibleSurfaceDuringZoom()
{
    const pdf::PDFRevisionIdentity revision = []
    {
        pdf::PDFDocumentBuilder builder;
        builder.appendPage(QRectF(0, 0, 200, 200));
        pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentContext context(&document);
        return context.getRevision();
    }();

    SurfaceProbeCache cache(1024 * 1024);
    const PageSurfaceKey baseKey = makeKey(revision, 100);
    const auto request = cache.request(baseKey, ProbePriority::Interaction);
    QVERIFY(request.has_value());
    QVERIFY(cache.insert(request.value(), QImage(QSize(160, 120), QImage::Format_ARGB32_Premultiplied)));
    cache.finish(ProbePriority::Interaction);

    const PageSurfaceKey zoomedKey = makeKey(revision, 125);
    const QImage* reused = cache.bestMatch(zoomedKey);
    QVERIFY(reused != nullptr);
    QCOMPARE(cache.cacheHits(), 1);

    const QImage transformed = reused->scaled(QSize(200, 150), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QCOMPARE(transformed.size(), QSize(200, 150));

    pdf::PDFInteractionTraceRecorder recorder;
    recorder.setEnabled(true);
    {
        auto frame = recorder.beginFrame(1, 0);
        recorder.recordCacheLookup(false);
        recorder.recordCacheLookup(true);
    }
    const QJsonObject cacheSummary = recorder.summary().value(QStringLiteral("cache")).toObject();
    QCOMPARE(cacheSummary.value(QStringLiteral("hits")).toInt(), 1);
    QCOMPARE(cacheSummary.value(QStringLiteral("misses")).toInt(), 1);
}

void PageSurfaceProbeTest::rejectsSupersededAndRevisionConflictingResults()
{
    SurfaceProbeCache cache(1024 * 1024);
    const pdf::PDFRevisionIdentity revision = m_widget->getDrawWidgetProxy()->getDocumentRevision();
    const PageSurfaceKey firstKey = makeKey(revision, 100);
    const auto first = cache.request(firstKey, ProbePriority::Interaction);
    QVERIFY(first.has_value());
    const auto second = cache.request(makeKey(revision, 125), ProbePriority::Interaction);
    QVERIFY(second.has_value());

    QVERIFY(!cache.insert(first.value(), QImage(QSize(160, 120), QImage::Format_ARGB32_Premultiplied)));
    QCOMPARE(cache.staleResults(), 1);

    cache.finish(ProbePriority::Interaction);
    cache.cancel();
    QVERIFY(!cache.insert(second.value(), QImage(QSize(160, 120), QImage::Format_ARGB32_Premultiplied)));
    QCOMPARE(cache.staleResults(), 2);

    const auto third = cache.request(firstKey, ProbePriority::Interaction);
    QVERIFY(third.has_value());
    cache.finish(ProbePriority::Interaction);
    const pdf::PDFRevisionIdentity changedRevision = [&revision]
    {
        pdf::PDFRevisionIdentity changed = revision;
        ++changed.documentRevision;
        ++changed.cacheGeneration;
        return changed;
    }();
    cache.invalidate(changedRevision);
    QVERIFY(!cache.insert(third.value(), QImage(QSize(160, 120), QImage::Format_ARGB32_Premultiplied)));
    QCOMPARE(cache.invalidations(), 1);
}

void PageSurfaceProbeTest::enforcesBudgetAndPriorityShedding()
{
    SurfaceProbeCache cache(64 * 64 * 4 * 2);
    cache.setVisiblePageCount(1);
    const pdf::PDFRevisionIdentity revision = m_widget->getDrawWidgetProxy()->getDocumentRevision();

    const auto first = cache.request(makeKey(revision, 100), ProbePriority::VisiblePage);
    QVERIFY(first.has_value());
    QVERIFY(cache.insert(first.value(), QImage(QSize(64, 64), QImage::Format_ARGB32_Premultiplied)));
    cache.finish(ProbePriority::VisiblePage);

    const auto second = cache.request(makeKey(revision, 125), ProbePriority::VisiblePage);
    QVERIFY(second.has_value());
    QVERIFY(cache.insert(second.value(), QImage(QSize(64, 64), QImage::Format_ARGB32_Premultiplied)));
    cache.finish(ProbePriority::VisiblePage);
    QCOMPARE(cache.currentCost(), cache.byteBudget());

    const auto third = cache.request(makeKey(revision, 135), ProbePriority::VisiblePage);
    QVERIFY(third.has_value());
    QVERIFY(cache.insert(third.value(), QImage(QSize(64, 64), QImage::Format_ARGB32_Premultiplied)));
    cache.finish(ProbePriority::VisiblePage);
    QCOMPARE(cache.evictions(), 1);
    QCOMPARE(cache.currentCost(), cache.byteBudget());

    const auto oversized = cache.request(makeKey(revision, 150), ProbePriority::VisiblePage);
    QVERIFY(oversized.has_value());
    QVERIFY(!cache.insert(oversized.value(), QImage(QSize(128, 128), QImage::Format_ARGB32_Premultiplied)));
    QCOMPARE(cache.rejectedEntries(), 1);

    QVERIFY(cache.enqueue(ProbePriority::NearViewport));
    QVERIFY(cache.enqueue(ProbePriority::NearViewport));
    QVERIFY(!cache.enqueue(ProbePriority::NearViewport));
    QCOMPARE(cache.shedRequests(), 1);
}

void PageSurfaceProbeTest::rendersThroughExistingWidgetsPath()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    m_document = std::make_unique<pdf::PDFDocument>(builder.build());
    m_widget->setDocument(pdf::PDFModifiedDocument(m_document.get(), nullptr), {});

    pdf::PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    QVERIFY(proxy != nullptr);
    m_widget->updateCacheLimits(4 * 1024 * 1024, 16384, 128, 128);
    proxy->setFeatures(proxy->getFeatures() | pdf::PDFRenderer::DisplayTimes);
    QSignalSpy pageSpy(proxy, &pdf::PDFDrawWidgetProxy::pageImageChanged);
    QWidget* canvas = m_widget->getDrawWidget()->getWidget();
    QVERIFY(canvas != nullptr);
    const QImage first = canvas->grab().toImage();
    QVERIFY(!first.isNull());

    QTRY_VERIFY_WITH_TIMEOUT(pageSpy.count() > 0, 10000);
    const QImage refined = canvas->grab().toImage();
    QVERIFY(!refined.isNull());
    QCOMPARE(refined.size(), first.size());

    const QImage repeated = canvas->grab().toImage();
    QCOMPARE(repeated.size(), refined.size());

    QObject* recorderObject = canvas->findChild<QObject*>(QStringLiteral("LoupeInteractionTraceRecorder"));
    QVERIFY(recorderObject != nullptr);
    const auto* recorder = static_cast<const pdf::PDFInteractionTraceRecorder*>(recorderObject);
    const QJsonObject surfaceCache = recorder->summary().value(QStringLiteral("cache")).toObject();
    QVERIFY(surfaceCache.value(QStringLiteral("surface_misses")).toInt() > 0);
    QVERIFY(surfaceCache.value(QStringLiteral("surface_hits")).toInt() > 0);

    const int missesBeforeRevision = surfaceCache.value(QStringLiteral("surface_misses")).toInt();
    proxy->getDocumentContext()->markModified(pdf::PDFModifiedDocument::PageContents);
    const QImage revised = canvas->grab().toImage();
    QVERIFY(!revised.isNull());
    const QJsonObject revisedCache = recorder->summary().value(QStringLiteral("cache")).toObject();
    QVERIFY(revisedCache.value(QStringLiteral("surface_misses")).toInt() > missesBeforeRevision);

    proxy->zoom(proxy->getZoom() * 1.2, QPointF(canvas->rect().center()));
    const QImage zoomed = canvas->grab().toImage();
    QVERIFY(!zoomed.isNull());

    const pdf::PDFRevisionIdentity revision = proxy->getDocumentRevision();
    SurfaceProbeCache cache(4 * 1024 * 1024);
    const auto request = cache.request(makeKey(revision), ProbePriority::VisiblePage);
    QVERIFY(request.has_value());
    QVERIFY(cache.insert(request.value(), refined));
    cache.finish(ProbePriority::VisiblePage);
    QVERIFY(cache.bestMatch(makeKey(revision, 110)) != nullptr);
}

}   // namespace

QTEST_MAIN(PageSurfaceProbeTest)

#include "tst_pagesurfaceprobetest.moc"
