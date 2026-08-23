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

#include "pdfinteractiontrace_p.h"

#include <QJsonDocument>
#include <QLoggingCategory>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pdf
{

thread_local PDFInteractionTraceRecorder* PDFInteractionTraceRecorder::s_current = nullptr;

namespace
{

constexpr qsizetype MinimumSamples = 1;
constexpr qsizetype MaximumSamples = 16384;
constexpr int MinimumSampleEvery = 1;
constexpr int MaximumSampleEvery = 64;
constexpr double Reference60HzBudgetMs = 1000.0 / 60.0;
constexpr double Reference120HzBudgetMs = 1000.0 / 120.0;
constexpr qint64 SummaryRefreshPeriodNs = 250000000;

template <typename T>
T clampValue(T value, T minimum, T maximum)
{
    return std::max(minimum, std::min(value, maximum));
}

bool readIntegerEnvironment(const char* name, qint64* value)
{
    bool ok = false;
    const qint64 parsed = QString::fromLocal8Bit(qgetenv(name)).toLongLong(&ok);
    if (ok)
    {
        *value = parsed;
    }
    return ok;
}

bool readDoubleEnvironment(const char* name, double* value)
{
    bool ok = false;
    const double parsed = QString::fromLocal8Bit(qgetenv(name)).toDouble(&ok);
    if (ok)
    {
        *value = parsed;
    }
    return ok;
}

}   // namespace

PDFInteractionTraceRecorder::StageScope::StageScope(PDFInteractionTraceRecorder* recorder, Stage stage) :
    m_recorder(recorder),
    m_stage(stage)
{
    if (m_recorder && m_recorder->m_enabled)
    {
        m_active = true;
        m_previousCurrent = PDFInteractionTraceRecorder::s_current;
        PDFInteractionTraceRecorder::s_current = m_recorder;
        m_recorder->m_activeStages.push_back({ m_stage, m_recorder->nowNs(), 0 });
    }
}

PDFInteractionTraceRecorder::StageScope::~StageScope()
{
    if (m_active)
    {
        m_recorder->finishStage(m_stage);
        PDFInteractionTraceRecorder::s_current = m_previousCurrent;
    }
}

PDFInteractionTraceRecorder::StageScope::StageScope(StageScope&& other) noexcept :
    m_recorder(other.m_recorder),
    m_stage(other.m_stage),
    m_active(other.m_active),
    m_previousCurrent(other.m_previousCurrent)
{
    other.m_recorder = nullptr;
    other.m_active = false;
}

PDFInteractionTraceRecorder::StageScope& PDFInteractionTraceRecorder::StageScope::operator=(StageScope&& other) noexcept
{
    if (this != &other)
    {
        if (m_active)
        {
            m_recorder->finishStage(m_stage);
            PDFInteractionTraceRecorder::s_current = m_previousCurrent;
        }

        m_recorder = other.m_recorder;
        m_stage = other.m_stage;
        m_active = other.m_active;
        m_previousCurrent = other.m_previousCurrent;
        other.m_recorder = nullptr;
        other.m_active = false;
    }
    return *this;
}

PDFInteractionTraceRecorder::InputScope::InputScope(PDFInteractionTraceRecorder* recorder, quint64 inputId) :
    m_recorder(recorder),
    m_inputId(inputId)
{
    if (m_recorder && m_recorder->m_enabled && m_inputId != 0)
    {
        m_active = true;
        m_previousCurrent = PDFInteractionTraceRecorder::s_current;
        PDFInteractionTraceRecorder::s_current = m_recorder;
    }
}

PDFInteractionTraceRecorder::InputScope::~InputScope()
{
    if (m_active)
    {
        m_recorder->finishInput(m_inputId);
        PDFInteractionTraceRecorder::s_current = m_previousCurrent;
    }
}

PDFInteractionTraceRecorder::InputScope::InputScope(InputScope&& other) noexcept :
    m_recorder(other.m_recorder),
    m_inputId(other.m_inputId),
    m_active(other.m_active),
    m_previousCurrent(other.m_previousCurrent)
{
    other.m_recorder = nullptr;
    other.m_active = false;
}

PDFInteractionTraceRecorder::InputScope& PDFInteractionTraceRecorder::InputScope::operator=(InputScope&& other) noexcept
{
    if (this != &other)
    {
        if (m_active)
        {
            m_recorder->finishInput(m_inputId);
            PDFInteractionTraceRecorder::s_current = m_previousCurrent;
        }

        m_recorder = other.m_recorder;
        m_inputId = other.m_inputId;
        m_active = other.m_active;
        m_previousCurrent = other.m_previousCurrent;
        other.m_recorder = nullptr;
        other.m_active = false;
    }
    return *this;
}

PDFInteractionTraceRecorder::FrameScope::FrameScope(PDFInteractionTraceRecorder* recorder, quint64 frameId) :
    m_recorder(recorder),
    m_frameId(frameId)
{
    if (m_recorder && m_recorder->m_enabled && m_frameId != 0)
    {
        m_active = true;
        m_previousCurrent = PDFInteractionTraceRecorder::s_current;
        PDFInteractionTraceRecorder::s_current = m_recorder;
    }
}

PDFInteractionTraceRecorder::FrameScope::~FrameScope()
{
    if (m_active)
    {
        m_recorder->finishFrame(m_frameId);
        PDFInteractionTraceRecorder::s_current = m_previousCurrent;
    }
}

PDFInteractionTraceRecorder::FrameScope::FrameScope(FrameScope&& other) noexcept :
    m_recorder(other.m_recorder),
    m_frameId(other.m_frameId),
    m_active(other.m_active),
    m_previousCurrent(other.m_previousCurrent)
{
    other.m_recorder = nullptr;
    other.m_active = false;
}

PDFInteractionTraceRecorder::FrameScope& PDFInteractionTraceRecorder::FrameScope::operator=(FrameScope&& other) noexcept
{
    if (this != &other)
    {
        if (m_active)
        {
            m_recorder->finishFrame(m_frameId);
            PDFInteractionTraceRecorder::s_current = m_previousCurrent;
        }

        m_recorder = other.m_recorder;
        m_frameId = other.m_frameId;
        m_active = other.m_active;
        m_previousCurrent = other.m_previousCurrent;
        other.m_recorder = nullptr;
        other.m_active = false;
    }
    return *this;
}

PDFInteractionTraceRecorder::PDFInteractionTraceRecorder(Clock clock, QObject* parent) :
    QObject(parent),
    m_clock(std::move(clock)),
    m_config(configFromEnvironment()),
    m_traceId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    m_elapsedTimer.start();
}

PDFInteractionTraceRecorder::~PDFInteractionTraceRecorder()
{
    if (m_enabled && m_hasData)
    {
        emitSummary();
    }
}

PDFInteractionTraceRecorder::Config PDFInteractionTraceRecorder::configFromEnvironment()
{
    Config config;

    qint64 integerValue = 0;
    if (readIntegerEnvironment("LOUPE_INTERACTION_TRACE_MAX_SAMPLES", &integerValue))
    {
        config.maxSamples = clampValue<qsizetype>(static_cast<qsizetype>(integerValue), MinimumSamples, MaximumSamples);
    }

    if (readIntegerEnvironment("LOUPE_INTERACTION_TRACE_SAMPLE_EVERY", &integerValue))
    {
        config.sampleEvery = clampValue<int>(static_cast<int>(integerValue), MinimumSampleEvery, MaximumSampleEvery);
    }

    double refreshRate = 0.0;
    if (readDoubleEnvironment("LOUPE_INTERACTION_TRACE_REFRESH_HZ", &refreshRate) && std::isfinite(refreshRate) && refreshRate > 0.0)
    {
        config.refreshRateHz = refreshRate;
    }

    return config;
}

void PDFInteractionTraceRecorder::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
    {
        return;
    }

    if (!enabled)
    {
        if (m_hasData)
        {
            emitSummary();
        }
        m_enabled = false;
        m_frame.reset();
        m_pendingInputs.clear();
        m_activeStages.clear();
        m_frameDurations.clear();
        m_inputLatencies.clear();
        m_completedFrames.clear();
        m_acknowledgements.clear();
        for (QVector<qint64>& samples : m_stageDurations)
        {
            samples.clear();
        }
        m_cachedSummary = QJsonObject();
        m_lastSummaryNs = std::numeric_limits<qint64>::min();
        m_hasData = false;
        return;
    }

    resetTrace();
    m_enabled = true;
    m_frameDurations.reserve(m_config.maxSamples);
    m_inputLatencies.reserve(m_config.maxSamples);
    m_completedFrames.reserve(m_config.maxSamples);
    for (QVector<qint64>& samples : m_stageDurations)
    {
        samples.reserve(m_config.maxSamples);
    }
}

void PDFInteractionTraceRecorder::setConfig(Config config)
{
    config.maxSamples = clampValue<qsizetype>(config.maxSamples, MinimumSamples, MaximumSamples);
    config.sampleEvery = clampValue<int>(config.sampleEvery, MinimumSampleEvery, MaximumSampleEvery);
    if (!std::isfinite(config.refreshRateHz) || config.refreshRateHz <= 0.0)
    {
        config.refreshRateHz = 0.0;
    }
    m_config = config;

    if (m_enabled)
    {
        m_frameDurations.reserve(m_config.maxSamples);
        m_inputLatencies.reserve(m_config.maxSamples);
        m_completedFrames.reserve(m_config.maxSamples);
        for (QVector<qint64>& samples : m_stageDurations)
        {
            samples.reserve(m_config.maxSamples);
        }
    }
}

void PDFInteractionTraceRecorder::setRefreshRateHz(double refreshRateHz)
{
    const double normalized = std::isfinite(refreshRateHz) && refreshRateHz > 0.0 ? refreshRateHz : 0.0;
    if (qFuzzyCompare(m_config.refreshRateHz, normalized))
    {
        return;
    }
    m_config.refreshRateHz = normalized;
    m_lastSummaryNs = std::numeric_limits<qint64>::min();
}

void PDFInteractionTraceRecorder::setEvidenceState(EvidenceState state)
{
    m_config.evidenceState = state;
    m_lastSummaryNs = std::numeric_limits<qint64>::min();
}

void PDFInteractionTraceRecorder::observeDocumentRevision(qint64 revision)
{
    if (!m_enabled || revision == std::numeric_limits<qint64>::min())
    {
        return;
    }

    if (m_lastDocumentRevision == std::numeric_limits<qint64>::min())
    {
        m_documentRevisionOrdinal = 1;
    }
    else if (m_lastDocumentRevision != revision)
    {
        ++m_documentRevisionOrdinal;
    }

    m_lastDocumentRevision = revision;
}

PDFInteractionTraceRecorder::InputScope PDFInteractionTraceRecorder::beginInput(InputKind kind)
{
    if (!m_enabled)
    {
        return InputScope();
    }

    ++m_inputCounter;
    m_hasData = true;
    if ((m_inputCounter - 1) % static_cast<quint64>(m_config.sampleEvery) != 0)
    {
        return InputScope();
    }

    const quint64 inputId = m_nextInputId++;
    m_pendingInputs.push_back({ inputId, nowNs(), kind });
    while (m_pendingInputs.size() > m_config.maxSamples)
    {
        m_pendingInputs.remove(0);
    }
    return InputScope(this, inputId);
}

PDFInteractionTraceRecorder::FrameScope PDFInteractionTraceRecorder::beginFrame(int visiblePages, int queueDepth)
{
    if (!m_enabled)
    {
        return FrameScope();
    }

    ++m_frameCounter;
    const quint64 frameId = m_nextFrameId++;
    Frame frame;
    frame.id = frameId;
    frame.startNs = nowNs();
    frame.visiblePages = visiblePages;
    frame.queueDepth = queueDepth;
    frame.sampled = (m_frameCounter - 1) % static_cast<quint64>(m_config.sampleEvery) == 0;
    m_frame = frame;
    m_lastVisiblePages = visiblePages;
    if (queueDepth >= 0)
    {
        m_lastQueueDepth = queueDepth;
    }
    m_hasData = true;
    return FrameScope(this, frameId);
}

PDFInteractionTraceRecorder::StageScope PDFInteractionTraceRecorder::beginStage(Stage stage)
{
    return StageScope(this, stage);
}

void PDFInteractionTraceRecorder::recordCacheLookup(bool hit)
{
    if (!m_enabled)
    {
        return;
    }

    m_hasData = true;
    if (m_frame)
    {
        if (hit)
        {
            ++m_frame->cacheHits;
        }
        else
        {
            ++m_frame->cacheMisses;
        }
    }

    if (hit)
    {
        ++m_totalCacheHits;
    }
    else
    {
        ++m_totalCacheMisses;
    }
}

void PDFInteractionTraceRecorder::recordQueueDepth(int queueDepth)
{
    if (!m_enabled || queueDepth < 0)
    {
        return;
    }

    m_lastQueueDepth = queueDepth;
    if (m_frame)
    {
        m_frame->queueDepth = queueDepth;
    }
}

PDFInteractionTraceRecorder* PDFInteractionTraceRecorder::current()
{
    return s_current;
}

qint64 PDFInteractionTraceRecorder::nowNs() const
{
    return m_clock ? m_clock() : m_elapsedTimer.nsecsElapsed();
}

void PDFInteractionTraceRecorder::resetTrace()
{
    m_traceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_hasData = false;
    m_nextInputId = 1;
    m_nextFrameId = 1;
    m_inputCounter = 0;
    m_frameCounter = 0;
    m_documentRevisionOrdinal = 0;
    m_lastDocumentRevision = std::numeric_limits<qint64>::min();
    m_lastSummaryNs = std::numeric_limits<qint64>::min();
    m_cachedSummary = QJsonObject();
    m_frame.reset();
    m_pendingInputs.clear();
    m_activeStages.clear();
    m_frameDurations.clear();
    m_inputLatencies.clear();
    m_completedFrames.clear();
    m_acknowledgements.clear();
    for (QVector<qint64>& samples : m_stageDurations)
    {
        samples.clear();
    }
    m_totalStageNs.fill(0);
    m_slowCauseCounts.fill(0);
    m_lastVisiblePages = -1;
    m_lastQueueDepth = -1;
    m_totalCacheHits = 0;
    m_totalCacheMisses = 0;
    m_lastAcknowledgedInputId = 0;
    m_lastAcknowledgedFrameId = 0;
}

void PDFInteractionTraceRecorder::finishInput(quint64 inputId)
{
    Q_UNUSED(inputId);
}

void PDFInteractionTraceRecorder::finishStage(Stage stage)
{
    if (!m_enabled || m_activeStages.empty())
    {
        return;
    }

    ActiveStage active = m_activeStages.back();
    m_activeStages.pop_back();
    if (active.stage != stage)
    {
        // Scope lifetimes are expected to be LIFO. Preserve a bounded trace even
        // if a caller violates that contract rather than corrupting the frame.
        stage = active.stage;
    }

    const qint64 elapsedNs = qMax<qint64>(0, nowNs() - active.startNs);
    const qint64 exclusiveNs = qMax<qint64>(0, elapsedNs - active.childNs);
    const int index = stageIndex(stage);
    m_totalStageNs[static_cast<size_t>(index)] += exclusiveNs;
    appendSample(m_stageDurations[static_cast<size_t>(index)], exclusiveNs);

    if (m_frame)
    {
        m_frame->stageNs[static_cast<size_t>(index)] += exclusiveNs;
    }
    if (!m_activeStages.empty())
    {
        m_activeStages.back().childNs += elapsedNs;
    }
    m_hasData = true;
}

void PDFInteractionTraceRecorder::finishFrame(quint64 frameId)
{
    if (!m_enabled || !m_frame || m_frame->id != frameId)
    {
        return;
    }

    const qint64 durationNs = qMax<qint64>(0, nowNs() - m_frame->startNs);
    Frame frame = *m_frame;
    m_frame.reset();

    const qint64 stageSumNs = std::accumulate(frame.stageNs.cbegin(), frame.stageNs.cend(), qint64(0));
    const qint64 externalNs = qMax<qint64>(0, durationNs - stageSumNs);
    frame.stageNs[static_cast<size_t>(stageIndex(Stage::ExternalUnknown))] += externalNs;
    m_totalStageNs[static_cast<size_t>(stageIndex(Stage::ExternalUnknown))] += externalNs;
    appendSample(m_stageDurations[static_cast<size_t>(stageIndex(Stage::ExternalUnknown))], externalNs);

    CompletedFrame completed;
    completed.durationNs = durationNs;
    completed.visiblePages = frame.visiblePages;
    completed.queueDepth = frame.queueDepth;
    completed.cacheHits = frame.cacheHits;
    completed.cacheMisses = frame.cacheMisses;
    completed.stageNs = frame.stageNs;

    if (frame.sampled)
    {
        appendSample(m_frameDurations, durationNs);
        m_completedFrames.push_back(completed);
        while (m_completedFrames.size() > m_config.maxSamples)
        {
            m_completedFrames.remove(0);
        }

        const QJsonObject budget = budgetObject(m_config.refreshRateHz);
        const double budgetMs = budget.value(QStringLiteral("frame_budget_ms")).toDouble(-1.0);
        if (budgetMs > 0.0 && durationNs / 1000000.0 > budgetMs)
        {
            int slowIndex = stageIndex(Stage::ExternalUnknown);
            for (int index = 0; index < StageCount; ++index)
            {
                if (frame.stageNs[static_cast<size_t>(index)] > frame.stageNs[static_cast<size_t>(slowIndex)])
                {
                    slowIndex = index;
                }
            }
            ++m_slowCauseCounts[static_cast<size_t>(slowIndex)];
        }
    }

    const qint64 frameEndNs = frame.startNs + durationNs;
    int acknowledgedInputs = 0;
    for (int index = 0; index < m_pendingInputs.size();)
    {
        const PendingInput& input = m_pendingInputs.at(index);
        if (input.startNs <= frameEndNs)
        {
            appendSample(m_inputLatencies, qMax<qint64>(0, frameEndNs - input.startNs));
            m_acknowledgements.push_back({ input.id, frame.id });
            while (m_acknowledgements.size() > m_config.maxSamples)
            {
                m_acknowledgements.remove(0);
            }
            m_lastAcknowledgedInputId = input.id;
            m_lastAcknowledgedFrameId = frame.id;
            m_pendingInputs.remove(index);
            ++acknowledgedInputs;
        }
        else
        {
            ++index;
        }
    }
    if (acknowledgedInputs > 0)
    {
        completed.hasInput = true;
        completed.inputToFrameNs = m_inputLatencies.isEmpty() ? 0 : m_inputLatencies.back();
    }

    m_lastVisiblePages = frame.visiblePages;
    if (frame.queueDepth >= 0)
    {
        m_lastQueueDepth = frame.queueDepth;
    }
    m_hasData = true;
    m_lastSummaryNs = std::numeric_limits<qint64>::min();
}

void PDFInteractionTraceRecorder::appendSample(QVector<qint64>& samples, qint64 value)
{
    if (m_config.maxSamples <= 0)
    {
        return;
    }
    if (samples.size() >= m_config.maxSamples)
    {
        samples.remove(0);
    }
    samples.push_back(value);
}

int PDFInteractionTraceRecorder::stageIndex(Stage stage)
{
    return clampValue(static_cast<int>(stage), 0, StageCount - 1);
}

QString PDFInteractionTraceRecorder::stageName(Stage stage)
{
    switch (stage)
    {
        case Stage::Interaction:
            return QStringLiteral("interaction_logic");
        case Stage::HitTest:
            return QStringLiteral("hit_testing");
        case Stage::PageRender:
            return QStringLiteral("page_rendering");
        case Stage::Overlay:
            return QStringLiteral("overlays");
        case Stage::Composition:
            return QStringLiteral("composition");
        case Stage::ExternalUnknown:
            return QStringLiteral("external_unknown");
    }
    return QStringLiteral("external_unknown");
}

QString PDFInteractionTraceRecorder::evidenceName(EvidenceState state)
{
    switch (state)
    {
        case EvidenceState::Verified:
            return QStringLiteral("verified");
        case EvidenceState::StaticOnly:
            return QStringLiteral("static-only");
        case EvidenceState::InfrastructureBlocked:
            return QStringLiteral("infrastructure-blocked");
    }
    return QStringLiteral("static-only");
}

QJsonValue PDFInteractionTraceRecorder::durationMs(qint64 nanoseconds)
{
    return QJsonValue(static_cast<double>(nanoseconds) / 1000000.0);
}

QJsonObject PDFInteractionTraceRecorder::percentileObject(const QVector<qint64>& samples)
{
    QJsonObject result;
    result.insert(QStringLiteral("available"), !samples.isEmpty());
    result.insert(QStringLiteral("sample_count"), samples.size());
    if (samples.isEmpty())
    {
        result.insert(QStringLiteral("p50_ms"), QJsonValue(QJsonValue::Null));
        result.insert(QStringLiteral("p95_ms"), QJsonValue(QJsonValue::Null));
        result.insert(QStringLiteral("p99_ms"), QJsonValue(QJsonValue::Null));
        return result;
    }

    QVector<qint64> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto nearestRank = [&sorted](double percentile)
    {
        const qsizetype rank = qMax<qsizetype>(1, static_cast<qsizetype>(std::ceil(percentile * sorted.size())));
        return sorted.at(qMin(rank, sorted.size()) - 1);
    };
    result.insert(QStringLiteral("p50_ms"), durationMs(nearestRank(0.50)));
    result.insert(QStringLiteral("p95_ms"), durationMs(nearestRank(0.95)));
    result.insert(QStringLiteral("p99_ms"), durationMs(nearestRank(0.99)));
    return result;
}

QJsonObject PDFInteractionTraceRecorder::budgetObject(double refreshRateHz)
{
    QJsonObject budget;
    budget.insert(QStringLiteral("reference_60_hz_ms"), Reference60HzBudgetMs);
    budget.insert(QStringLiteral("reference_120_hz_ms"), Reference120HzBudgetMs);

    if (std::isfinite(refreshRateHz) && refreshRateHz > 0.0)
    {
        budget.insert(QStringLiteral("status"), QStringLiteral("known"));
        budget.insert(QStringLiteral("refresh_rate_hz"), refreshRateHz);
        budget.insert(QStringLiteral("frame_budget_ms"), 1000.0 / refreshRateHz);
    }
    else
    {
        budget.insert(QStringLiteral("status"), QStringLiteral("unavailable"));
        budget.insert(QStringLiteral("refresh_rate_hz"), QJsonValue(QJsonValue::Null));
        budget.insert(QStringLiteral("frame_budget_ms"), QJsonValue(QJsonValue::Null));
        budget.insert(QStringLiteral("reason"), QStringLiteral("refresh rate is unknown"));
    }
    return budget;
}

void PDFInteractionTraceRecorder::refreshCachedSummary() const
{
    const qint64 now = nowNs();
    if (!m_cachedSummary.isEmpty() && m_lastSummaryNs != std::numeric_limits<qint64>::min() && now >= m_lastSummaryNs && now - m_lastSummaryNs < SummaryRefreshPeriodNs)
    {
        return;
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("LOUPE_INTERACTION_TRACE_V1"));
    root.insert(QStringLiteral("trace_id"), m_traceId);
    root.insert(QStringLiteral("evidence_state"), evidenceName(m_config.evidenceState));

    QJsonObject sampling;
    sampling.insert(QStringLiteral("max_samples"), m_config.maxSamples);
    sampling.insert(QStringLiteral("sample_every"), m_config.sampleEvery);
    sampling.insert(QStringLiteral("frame_samples"), m_frameDurations.size());
    sampling.insert(QStringLiteral("input_samples"), m_inputLatencies.size());
    root.insert(QStringLiteral("sampling"), sampling);

    root.insert(QStringLiteral("budgets"), budgetObject(m_config.refreshRateHz));
    root.insert(QStringLiteral("input_to_frame_ms"), percentileObject(m_inputLatencies));
    QJsonObject acknowledgements;
    acknowledgements.insert(QStringLiteral("available"), !m_acknowledgements.isEmpty());
    acknowledgements.insert(QStringLiteral("sample_count"), m_acknowledgements.size());
    acknowledgements.insert(QStringLiteral("last_input_sequence"), m_lastAcknowledgedInputId > 0 ? QJsonValue(static_cast<qint64>(m_lastAcknowledgedInputId)) : QJsonValue(QJsonValue::Null));
    acknowledgements.insert(QStringLiteral("last_frame_id"), m_lastAcknowledgedFrameId > 0 ? QJsonValue(static_cast<qint64>(m_lastAcknowledgedFrameId)) : QJsonValue(QJsonValue::Null));
    root.insert(QStringLiteral("input_acknowledgements"), acknowledgements);

    QJsonObject inputToPresent;
    inputToPresent.insert(QStringLiteral("available"), false);
    inputToPresent.insert(QStringLiteral("value_ms"), QJsonValue(QJsonValue::Null));
    inputToPresent.insert(QStringLiteral("reason"), QStringLiteral("QWidget present timing is unavailable"));
    root.insert(QStringLiteral("input_to_present"), inputToPresent);

    root.insert(QStringLiteral("frame_time_ms"), percentileObject(m_frameDurations));
    QJsonObject fps;
    if (!m_frameDurations.isEmpty())
    {
        const QJsonObject frameStats = percentileObject(m_frameDurations);
        const double p50 = frameStats.value(QStringLiteral("p50_ms")).toDouble();
        fps.insert(QStringLiteral("available"), p50 > 0.0);
        fps.insert(QStringLiteral("p50"), p50 > 0.0 ? 1000.0 / p50 : 0.0);
    }
    else
    {
        fps.insert(QStringLiteral("available"), false);
        fps.insert(QStringLiteral("p50"), QJsonValue(QJsonValue::Null));
    }
    root.insert(QStringLiteral("fps"), fps);

    QJsonObject droppedFrames;
    droppedFrames.insert(QStringLiteral("available"), false);
    droppedFrames.insert(QStringLiteral("count"), QJsonValue(QJsonValue::Null));
    droppedFrames.insert(QStringLiteral("reason"), QStringLiteral("QWidget paint path has no present callback"));
    root.insert(QStringLiteral("dropped_frames"), droppedFrames);

    QJsonObject lateFrames;
    lateFrames.insert(QStringLiteral("available"), budgetObject(m_config.refreshRateHz).value(QStringLiteral("status")).toString() == QStringLiteral("known"));
    quint64 lateCount = 0;
    if (m_config.refreshRateHz > 0.0)
    {
        const double budgetNs = 1000000000.0 / m_config.refreshRateHz;
        for (qint64 frameNs : m_frameDurations)
        {
            if (frameNs > budgetNs)
            {
                ++lateCount;
            }
        }
    }
    lateFrames.insert(QStringLiteral("count"), m_config.refreshRateHz > 0.0 ? QJsonValue(static_cast<qint64>(lateCount)) : QJsonValue(QJsonValue::Null));
    if (m_config.refreshRateHz <= 0.0)
    {
        lateFrames.insert(QStringLiteral("reason"), QStringLiteral("refresh rate is unknown"));
    }
    root.insert(QStringLiteral("late_frames"), lateFrames);

    QJsonObject causes;
    for (int index = 0; index < StageCount; ++index)
    {
        causes.insert(stageName(static_cast<Stage>(index)), static_cast<qint64>(m_slowCauseCounts[static_cast<size_t>(index)]));
    }
    root.insert(QStringLiteral("slow_frame_cause_buckets"), causes);

    QJsonObject stages;
    for (int index = 0; index < StageCount; ++index)
    {
        stages.insert(stageName(static_cast<Stage>(index)), percentileObject(m_stageDurations[static_cast<size_t>(index)]));
    }
    root.insert(QStringLiteral("stage_time_ms"), stages);

    QJsonObject cache;
    cache.insert(QStringLiteral("hits"), m_totalCacheHits);
    cache.insert(QStringLiteral("misses"), m_totalCacheMisses);
    const int cacheLookups = m_totalCacheHits + m_totalCacheMisses;
    cache.insert(QStringLiteral("hit_rate"), cacheLookups > 0 ? static_cast<double>(m_totalCacheHits) / cacheLookups : QJsonValue(QJsonValue::Null));
    root.insert(QStringLiteral("cache"), cache);

    root.insert(QStringLiteral("visible_page_count"), m_lastVisiblePages >= 0 ? QJsonValue(m_lastVisiblePages) : QJsonValue(QJsonValue::Null));
    QJsonObject pendingAsync;
    pendingAsync.insert(QStringLiteral("queue_depth"), m_lastQueueDepth >= 0 ? QJsonValue(m_lastQueueDepth) : QJsonValue(QJsonValue::Null));
    pendingAsync.insert(QStringLiteral("sampled"), m_lastQueueDepth >= 0);
    root.insert(QStringLiteral("pending_async_work"), pendingAsync);
    root.insert(QStringLiteral("document_revision_ordinal"), m_documentRevisionOrdinal > 0 ? QJsonValue(static_cast<qint64>(m_documentRevisionOrdinal)) : QJsonValue(QJsonValue::Null));

    QJsonObject privacy;
    privacy.insert(QStringLiteral("document_payload_recorded"), false);
    privacy.insert(QStringLiteral("pdf_text_recorded"), false);
    privacy.insert(QStringLiteral("image_pixels_recorded"), false);
    privacy.insert(QStringLiteral("paths_recorded"), false);
    root.insert(QStringLiteral("privacy"), privacy);

    m_cachedSummary = root;
    m_lastSummaryNs = now;
}

QJsonObject PDFInteractionTraceRecorder::summary() const
{
    refreshCachedSummary();
    return m_cachedSummary;
}

void PDFInteractionTraceRecorder::emitSummary() const
{
    const QJsonDocument document(summary());
    const QString line = QStringLiteral("LOUPE_INTERACTION_TRACE_V1 %1")
                             .arg(QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
    qInfo().noquote() << line;
}

}   // namespace pdf
