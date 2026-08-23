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


#include "canvaspresentmetrics.h"

#include <QElapsedTimer>
#include <QQuickWindow>
#include <QScreen>

namespace pdfquick
{

using pdfinteraction::IMonotonicClock;
using pdfinteraction::InteractionTraceRecorder;
using pdfinteraction::TraceStage;

class SteadyMonotonicClock::Impl
{
public:
    Impl() { timer.start(); }

    QElapsedTimer timer;
};

SteadyMonotonicClock::SteadyMonotonicClock() :
    m_impl(std::make_shared<Impl>())
{
}

qint64 SteadyMonotonicClock::nowNs() const
{
    return m_impl->timer.nsecsElapsed();
}

CanvasPresentMetrics::CanvasPresentMetrics(QObject* parent) :
    QObject(parent)
{
}

CanvasPresentMetrics::~CanvasPresentMetrics()
{
    attach(nullptr);
}

void CanvasPresentMetrics::setMaxSamples(int maxSamples)
{
    m_maxSamples = qMax(1, maxSamples);
}

void CanvasPresentMetrics::setRecorder(InteractionTraceRecorder* recorder)
{
    m_recorder = recorder;
}

void CanvasPresentMetrics::setClock(const IMonotonicClock* clock)
{
    m_clock = clock;
}

qint64 CanvasPresentMetrics::clockNow() const
{
    return m_clock ? m_clock->nowNs() : m_defaultClock.nowNs();
}

void CanvasPresentMetrics::attach(QQuickWindow* window)
{
    for (const QMetaObject::Connection& connection : m_connections)
    {
        QObject::disconnect(connection);
    }
    m_connections.clear();

    m_window = window;
    m_renderStartNs.store(0);
    m_renderEndNs.store(0);

    if (!window)
    {
        return;
    }

    // The host can see a screen; the neutral layer cannot. Reporting the real
    // rate is what turns the recorder's budget fields from "unavailable" into a
    // number, and it must stay a report rather than a default -- a guessed 60Hz
    // would make a 120Hz display look permanently within budget.
    if (const QScreen* screen = window->screen())
    {
        if (m_recorder && screen->refreshRate() > 0.0)
        {
            m_recorder->setRefreshRateHz(screen->refreshRate());
        }
    }

    // DirectConnection on purpose: these run on the render thread and must do
    // nothing but read the clock. See the thread rule on the class.
    m_connections.append(QObject::connect(
        window, &QQuickWindow::beforeRendering, this, [this]()
        { m_renderStartNs.store(clockNow()); }, Qt::DirectConnection));

    m_connections.append(QObject::connect(
        window, &QQuickWindow::afterRendering, this, [this]()
        { m_renderEndNs.store(clockNow()); }, Qt::DirectConnection));

    m_connections.append(QObject::connect(
        window,
        &QQuickWindow::frameSwapped,
        this,
        [this]()
        {
            const qint64 swapNs = clockNow();
            const qint64 renderStartNs = m_renderStartNs.exchange(0);
            const qint64 renderEndNs = m_renderEndNs.exchange(0);

            const bool stamped = renderStartNs > 0 && renderEndNs >= renderStartNs;
            const qint64 gpuNs = stamped ? renderEndNs - renderStartNs : -1;
            const qint64 presentNs = stamped ? qMax(qint64(0), swapNs - renderEndNs) : -1;

            // Only the numbers cross the thread boundary. The recorder is
            // touched exclusively in the queued slot below, on the GUI thread.
            QMetaObject::invokeMethod(
                this, [this, gpuNs, presentNs, swapNs]()
                { onFramePresented(gpuNs, presentNs, swapNs); }, Qt::QueuedConnection);
        },
        Qt::DirectConnection));
}

void CanvasPresentMetrics::frameRequested()
{
    if (m_recorder)
    {
        m_recorder->beginFrame();
    }
}

void CanvasPresentMetrics::appendSample(QList<qint64>& samples, qint64 value)
{
    samples.append(value);
    while (samples.size() > m_maxSamples)
    {
        samples.removeFirst();
    }
}

void CanvasPresentMetrics::onFramePresented(qint64 gpuNs, qint64 presentNs, qint64 swapNs)
{
    ++m_presentedFrames;

    if (m_lastSwapNs > 0 && swapNs > m_lastSwapNs)
    {
        appendSample(m_frameIntervalNs, swapNs - m_lastSwapNs);
    }
    m_lastSwapNs = swapNs;

    if (gpuNs < 0 || presentNs < 0)
    {
        // A swap with no matching render pass: the scene graph presented a frame
        // this item did not cause. Counted, never charged to the GPU stage as
        // zero -- a bucket that silently absorbs unmeasured frames reads as fast.
        ++m_framesWithoutRenderStamp;

        if (m_recorder)
        {
            m_recorder->endFrame();
        }
        return;
    }

    appendSample(m_gpuNs, gpuNs);
    appendSample(m_presentNs, presentNs);

    if (m_recorder)
    {
        // TraceStage::External is the bucket for time spent outside the
        // interaction layer, which is exactly what the render pass and the swap
        // are. Charging it to Unknown instead would make the recorder's
        // slow-frame attribution answer "unknown" for every GPU-bound frame.
        m_recorder->recordStage(TraceStage::External, gpuNs + presentNs);
        m_recorder->endFrame();
    }

    emit framePresented(gpuNs, presentNs);
}

QJsonObject CanvasPresentMetrics::summary() const
{
    QJsonObject present;
    present[QStringLiteral("presented_frames")] = qint64(m_presentedFrames);
    present[QStringLiteral("frames_without_render_stamp")] = qint64(m_framesWithoutRenderStamp);
    present[QStringLiteral("retained_samples")] = m_gpuNs.size();

    // The recorder's own helper, not a second percentile implementation, so a
    // present percentile and an interaction percentile mean the same thing.
    // It converts nanosecond samples to milliseconds, hence the _ms names on
    // fields fed from nanosecond durations.
    present[QStringLiteral("gpu_ms")] = InteractionTraceRecorder::percentileObject(m_gpuNs);
    present[QStringLiteral("present_ms")] = InteractionTraceRecorder::percentileObject(m_presentNs);
    present[QStringLiteral("frame_interval_ms")] = InteractionTraceRecorder::percentileObject(m_frameIntervalNs);

    QJsonObject root;
    root[QStringLiteral("present")] = present;
    return root;
}

void CanvasPresentMetrics::reset()
{
    m_gpuNs.clear();
    m_presentNs.clear();
    m_frameIntervalNs.clear();
    m_lastSwapNs = 0;
    m_presentedFrames = 0;
    m_framesWithoutRenderStamp = 0;
    m_renderStartNs.store(0);
    m_renderEndNs.store(0);
}

}   // namespace pdfquick
