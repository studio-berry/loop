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

#include "interactiontrace.h"

#include "pdfjobscheduler.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace pdfinteraction
{

namespace
{

constexpr int stageIndex(TraceStage stage)
{
    return int(stage);
}

QJsonValue durationMs(qint64 nanoseconds)
{
    return QJsonValue(double(nanoseconds) / 1000000.0);
}

QJsonObject stampToJson(const InputStamp& stamp)
{
    QJsonObject json;
    json.insert(QStringLiteral("monotonic_ns"), qint64(stamp.monotonicNs));
    json.insert(QStringLiteral("sequence"), qint64(stamp.sequence));
    return json;
}

InputStamp stampFromJson(const QJsonObject& json)
{
    InputStamp stamp;
    stamp.monotonicNs = qint64(json.value(QStringLiteral("monotonic_ns")).toDouble());
    stamp.sequence = quint64(json.value(QStringLiteral("sequence")).toDouble());
    return stamp;
}

QJsonObject pointToJson(QPoint point)
{
    QJsonObject json;
    json.insert(QStringLiteral("x"), point.x());
    json.insert(QStringLiteral("y"), point.y());
    return json;
}

QPoint pointFromJson(const QJsonObject& json)
{
    return QPoint(json.value(QStringLiteral("x")).toInt(), json.value(QStringLiteral("y")).toInt());
}

}   // namespace

// TraceJobKind mirrors pdf::PDFJobKind so the trace header does not have to
// include the scheduler. A mirror that drifts is worse than a dependency: a
// preflight job would be reported as an export and nobody would see a compile
// error. These pin every enumerator to its original.
static_assert(int(TraceJobKind::Rendering) == int(pdf::PDFJobKind::Rendering));
static_assert(int(TraceJobKind::Preflight) == int(pdf::PDFJobKind::Preflight));
static_assert(int(TraceJobKind::OCR) == int(pdf::PDFJobKind::OCR));
static_assert(int(TraceJobKind::Export) == int(pdf::PDFJobKind::Export));
static_assert(int(TraceJobKind::Thumbnail) == int(pdf::PDFJobKind::Thumbnail));
static_assert(int(TraceJobKind::Batch) == int(pdf::PDFJobKind::Batch));
static_assert(int(TraceJobKind::Agent) == int(pdf::PDFJobKind::Agent));
static_assert(int(TraceJobKind::Other) == int(pdf::PDFJobKind::Other));
static_assert(int(TraceJobKind::Other) + 1 == TraceJobKindCount,
              "TraceJobKindCount must cover every TraceJobKind");

const char* getTraceJobKindName(TraceJobKind kind)
{
    switch (kind)
    {
        case TraceJobKind::Rendering:
            return "rendering";
        case TraceJobKind::Preflight:
            return "preflight";
        case TraceJobKind::OCR:
            return "ocr";
        case TraceJobKind::Export:
            return "export";
        case TraceJobKind::Thumbnail:
            return "thumbnail";
        case TraceJobKind::Batch:
            return "batch";
        case TraceJobKind::Agent:
            return "agent";
        case TraceJobKind::Other:
            break;
    }

    return "other";
}

const char* getTraceStageName(TraceStage stage)
{
    switch (stage)
    {
        case TraceStage::Interaction:
            return "interaction";
        case TraceStage::HitTest:
            return "hit-test";
        case TraceStage::Overlay:
            return "overlay";
        case TraceStage::PageSurface:
            return "page-surface";
        case TraceStage::External:
            return "external";
        case TraceStage::Unknown:
            return "unknown";
    }

    return "unknown";
}

QJsonObject InteractionTrace::toJson() const
{
    QJsonArray records;

    for (const TraceInputRecord& input : inputs)
    {
        QJsonObject record;

        if (input.pointer.has_value())
        {
            const PointerIntent& intent = *input.pointer;
            QJsonObject pointer;
            pointer.insert(QStringLiteral("stamp"), stampToJson(intent.stamp));
            pointer.insert(QStringLiteral("action"), QString::fromLatin1(getPointerActionName(intent.action)));
            pointer.insert(QStringLiteral("position_px"), pointToJson(intent.positionPx));
            pointer.insert(QStringLiteral("button"), int(intent.button));
            pointer.insert(QStringLiteral("buttons"), int(intent.buttons.toInt()));
            pointer.insert(QStringLiteral("modifiers"), int(intent.modifiers.toInt()));
            record.insert(QStringLiteral("pointer"), pointer);
        }
        else if (input.wheel.has_value())
        {
            const WheelIntent& intent = *input.wheel;
            QJsonObject wheel;
            wheel.insert(QStringLiteral("stamp"), stampToJson(intent.stamp));
            wheel.insert(QStringLiteral("position_px"), pointToJson(intent.positionPx));
            wheel.insert(QStringLiteral("angle_delta"), pointToJson(intent.angleDelta));
            wheel.insert(QStringLiteral("pixel_delta"), pointToJson(intent.pixelDelta));
            wheel.insert(QStringLiteral("modifiers"), int(intent.modifiers.toInt()));
            record.insert(QStringLiteral("wheel"), wheel);
        }
        else if (input.key.has_value())
        {
            const KeyIntent& intent = *input.key;
            QJsonObject key;
            key.insert(QStringLiteral("stamp"), stampToJson(intent.stamp));
            key.insert(QStringLiteral("action"), intent.action == KeyAction::Press ? QStringLiteral("press") : QStringLiteral("release"));
            key.insert(QStringLiteral("key"), intent.key);
            key.insert(QStringLiteral("modifiers"), int(intent.modifiers.toInt()));
            key.insert(QStringLiteral("auto_repeat"), intent.autoRepeat);
            record.insert(QStringLiteral("key"), key);
        }
        else if (input.notification.has_value())
        {
            record.insert(QStringLiteral("notification"), QString::fromLatin1(getHostNotificationName(*input.notification)));
        }
        else
        {
            continue;
        }

        records.append(record);
    }

    QJsonObject json;
    json.insert(QStringLiteral("schema_version"), 1);
    json.insert(QStringLiteral("trace_id"), traceId);
    json.insert(QStringLiteral("inputs"), records);
    return json;
}

InteractionTrace InteractionTrace::fromJson(const QJsonObject& json)
{
    InteractionTrace trace;
    trace.traceId = json.value(QStringLiteral("trace_id")).toString();

    const QJsonArray records = json.value(QStringLiteral("inputs")).toArray();
    trace.inputs.reserve(records.size());

    for (const QJsonValue& value : records)
    {
        const QJsonObject record = value.toObject();
        TraceInputRecord input;

        if (record.contains(QStringLiteral("pointer")))
        {
            const QJsonObject pointer = record.value(QStringLiteral("pointer")).toObject();
            PointerIntent intent;
            intent.stamp = stampFromJson(pointer.value(QStringLiteral("stamp")).toObject());
            const QString action = pointer.value(QStringLiteral("action")).toString();
            intent.action = action == QStringLiteral("press")     ? PointerAction::Press
                            : action == QStringLiteral("release") ? PointerAction::Release
                            : action == QStringLiteral("cancel")  ? PointerAction::Cancel
                            : action == QStringLiteral("leave")   ? PointerAction::Leave
                                                                  : PointerAction::Move;
            intent.positionPx = pointFromJson(pointer.value(QStringLiteral("position_px")).toObject());
            intent.button = Qt::MouseButton(pointer.value(QStringLiteral("button")).toInt());
            intent.buttons = Qt::MouseButtons(pointer.value(QStringLiteral("buttons")).toInt());
            intent.modifiers = Qt::KeyboardModifiers(pointer.value(QStringLiteral("modifiers")).toInt());
            input.pointer = intent;
        }
        else if (record.contains(QStringLiteral("wheel")))
        {
            const QJsonObject wheel = record.value(QStringLiteral("wheel")).toObject();
            WheelIntent intent;
            intent.stamp = stampFromJson(wheel.value(QStringLiteral("stamp")).toObject());
            intent.positionPx = pointFromJson(wheel.value(QStringLiteral("position_px")).toObject());
            intent.angleDelta = pointFromJson(wheel.value(QStringLiteral("angle_delta")).toObject());
            intent.pixelDelta = pointFromJson(wheel.value(QStringLiteral("pixel_delta")).toObject());
            intent.modifiers = Qt::KeyboardModifiers(wheel.value(QStringLiteral("modifiers")).toInt());
            input.wheel = intent;
        }
        else if (record.contains(QStringLiteral("key")))
        {
            const QJsonObject key = record.value(QStringLiteral("key")).toObject();
            KeyIntent intent;
            intent.stamp = stampFromJson(key.value(QStringLiteral("stamp")).toObject());
            intent.action = key.value(QStringLiteral("action")).toString() == QStringLiteral("release") ? KeyAction::Release : KeyAction::Press;
            intent.key = key.value(QStringLiteral("key")).toInt();
            intent.modifiers = Qt::KeyboardModifiers(key.value(QStringLiteral("modifiers")).toInt());
            intent.autoRepeat = key.value(QStringLiteral("auto_repeat")).toBool();
            input.key = intent;
        }
        else if (record.contains(QStringLiteral("notification")))
        {
            const QString name = record.value(QStringLiteral("notification")).toString();
            input.notification = name == QStringLiteral("capture-lost")         ? HostNotification::CaptureLost
                                 : name == QStringLiteral("window-deactivated") ? HostNotification::WindowDeactivated
                                                                                : HostNotification::FocusLost;
        }

        if (input.isValid())
        {
            trace.inputs.push_back(input);
        }
    }

    return trace;
}

InteractionTraceRecorder::InteractionTraceRecorder(const IMonotonicClock& clock) :
    InteractionTraceRecorder(clock, Config())
{
}

InteractionTraceRecorder::InteractionTraceRecorder(const IMonotonicClock& clock, Config config) :
    m_clock(&clock),
    m_config(config)
{
    setConfig(config);
}

void InteractionTraceRecorder::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

void InteractionTraceRecorder::setConfig(Config config)
{
    config.maxSamples = qMax(1, config.maxSamples);
    config.maxInputs = qMax(0, config.maxInputs);
    if (!std::isfinite(config.refreshRateHz) || config.refreshRateHz <= 0.0)
    {
        config.refreshRateHz = 0.0;
    }

    m_config = config;
}

void InteractionTraceRecorder::setRefreshRateHz(qreal refreshRateHz)
{
    Config config = m_config;
    config.refreshRateHz = refreshRateHz;
    setConfig(config);
}

void InteractionTraceRecorder::setTraceId(QString traceId)
{
    m_trace.traceId = std::move(traceId);
}

qint64 InteractionTraceRecorder::nowNs() const
{
    return m_clock ? m_clock->nowNs() : 0;
}

void InteractionTraceRecorder::appendSample(QList<qint64>& samples, qint64 value)
{
    samples.push_back(value);
    while (samples.size() > m_config.maxSamples)
    {
        samples.removeFirst();
    }
}

void InteractionTraceRecorder::recordInput(const TraceInputRecord& record)
{
    if (!m_enabled || !record.isValid())
    {
        return;
    }

    ++m_inputCount;

    quint64 sequence = 0;
    qint64 startNs = nowNs();

    if (record.pointer.has_value())
    {
        sequence = record.pointer->stamp.sequence;
        startNs = record.pointer->stamp.monotonicNs;
    }
    else if (record.wheel.has_value())
    {
        sequence = record.wheel->stamp.sequence;
        startNs = record.wheel->stamp.monotonicNs;
    }
    else if (record.key.has_value())
    {
        sequence = record.key->stamp.sequence;
        startNs = record.key->stamp.monotonicNs;
    }

    m_pendingInputs.push_back({ sequence, startNs });

    if (m_config.maxInputs == 0)
    {
        return;
    }

    m_trace.inputs.push_back(record);
    while (m_trace.inputs.size() > m_config.maxInputs)
    {
        m_trace.inputs.removeFirst();
        ++m_droppedInputRecords;
    }
}

void InteractionTraceRecorder::recordPointer(const PointerIntent& intent)
{
    TraceInputRecord record;
    record.pointer = intent;
    recordInput(record);
}

void InteractionTraceRecorder::recordWheel(const WheelIntent& intent)
{
    TraceInputRecord record;
    record.wheel = intent;
    recordInput(record);
}

void InteractionTraceRecorder::recordKey(const KeyIntent& intent)
{
    TraceInputRecord record;
    record.key = intent;
    recordInput(record);
}

void InteractionTraceRecorder::recordNotification(HostNotification notification)
{
    TraceInputRecord record;
    record.notification = notification;
    recordInput(record);
}

void InteractionTraceRecorder::beginFrame()
{
    if (!m_enabled)
    {
        return;
    }

    if (m_frame.has_value())
    {
        // A caller that opens a frame inside a frame has a bug. Counting it is
        // more useful than asserting, because the trace is a diagnostic and an
        // abort here would take out the thing being diagnosed.
        ++m_unbalancedFrames;
    }

    OpenFrame frame;
    frame.startNs = nowNs();
    m_frame = frame;
}

void InteractionTraceRecorder::recordStage(TraceStage stage, qint64 durationNs)
{
    if (!m_enabled || !m_frame.has_value() || durationNs < 0)
    {
        return;
    }

    m_frame->stageNs[size_t(stageIndex(stage))] += durationNs;
}

void InteractionTraceRecorder::recordHitTest(int indexCandidates, int preciseHits, qint64 durationNs)
{
    if (!m_enabled)
    {
        return;
    }

    appendSample(m_hitTestCandidates, qMax(0, indexCandidates));
    appendSample(m_hitTestPreciseHits, qMax(0, preciseHits));
    appendSample(m_hitTestDurations, qMax<qint64>(0, durationNs));
}

void InteractionTraceRecorder::recordJobStateChange(TraceJobKind kind, bool running)
{
    if (!m_enabled)
    {
        return;
    }

    int& active = m_activeJobs[size_t(kind)];

    if (running)
    {
        ++active;
        return;
    }

    // Clamped at zero rather than allowed to go negative. An unbalanced
    // finish is a caller bug, but a negative count would silently turn later
    // real overlaps invisible, which is worse than absorbing the one error.
    active = qMax(0, active - 1);
}

void InteractionTraceRecorder::recordJobOverlap(bool slowFrame)
{
    for (int index = 0; index < TraceJobKindCount; ++index)
    {
        if (m_activeJobs[size_t(index)] <= 0)
        {
            continue;
        }

        ++m_framesOverlapped[size_t(index)];

        if (slowFrame)
        {
            ++m_slowFramesOverlapped[size_t(index)];
        }
    }
}

void InteractionTraceRecorder::attributeSlowFrame(const OpenFrame& frame, qint64 durationNs)
{
    const QJsonObject budget = budgetObject(m_config.refreshRateHz);
    const double budgetMs = budget.value(QStringLiteral("frame_budget_ms")).toDouble(-1.0);
    if (budgetMs <= 0.0 || double(durationNs) / 1000000.0 <= budgetMs)
    {
        return;
    }

    qint64 accounted = 0;
    int slowest = stageIndex(TraceStage::Unknown);
    for (int index = 0; index < TraceStageCount; ++index)
    {
        accounted += frame.stageNs[size_t(index)];
        if (index != stageIndex(TraceStage::Unknown) && frame.stageNs[size_t(index)] > frame.stageNs[size_t(slowest)])
        {
            slowest = index;
        }
    }

    // Unattributed time wins when it dominates. A frame whose stages account for
    // a fraction of its duration was slowed by something none of them measured,
    // and charging it to the largest measured stage would invent a cause.
    if (durationNs - accounted > frame.stageNs[size_t(slowest)])
    {
        slowest = stageIndex(TraceStage::Unknown);
    }

    ++m_slowCauseCounts[size_t(slowest)];
}

void InteractionTraceRecorder::endFrame()
{
    if (!m_enabled || !m_frame.has_value())
    {
        if (m_enabled)
        {
            ++m_unbalancedFrames;
        }

        return;
    }

    const OpenFrame frame = *m_frame;
    m_frame.reset();

    const qint64 endNs = nowNs();
    const qint64 durationNs = qMax<qint64>(0, endNs - frame.startNs);

    ++m_frameCount;
    appendSample(m_frameDurations, durationNs);

    for (int index = 0; index < TraceStageCount; ++index)
    {
        if (frame.stageNs[size_t(index)] > 0)
        {
            appendSample(m_stageDurations[size_t(index)], frame.stageNs[size_t(index)]);
        }
    }

    const QJsonObject frameBudget = budgetObject(m_config.refreshRateHz);
    const double frameBudgetMs = frameBudget.value(QStringLiteral("frame_budget_ms")).toDouble(-1.0);
    const bool slowFrame = frameBudgetMs > 0.0 && double(durationNs) / 1000000.0 > frameBudgetMs;

    attributeSlowFrame(frame, durationNs);
    recordJobOverlap(slowFrame);

    for (auto it = m_pendingInputs.begin(); it != m_pendingInputs.end();)
    {
        if (it->startNs <= endNs)
        {
            appendSample(m_inputLatencies, qMax<qint64>(0, endNs - it->startNs));
            it = m_pendingInputs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void InteractionTraceRecorder::recordCacheLookup(bool hit)
{
    if (!m_enabled)
    {
        return;
    }

    if (hit)
    {
        ++m_cacheHits;
    }
    else
    {
        ++m_cacheMisses;
    }
}

QJsonObject InteractionTraceRecorder::percentileObject(const QList<qint64>& samples)
{
    return percentileObject(samples, PercentileUnit::Duration);
}

QJsonObject InteractionTraceRecorder::countPercentileObject(const QList<qint64>& samples)
{
    return percentileObject(samples, PercentileUnit::Count);
}

QJsonObject InteractionTraceRecorder::percentileObject(const QList<qint64>& samples, PercentileUnit unit)
{
    // A candidate count is not a nanosecond count. Running one through the
    // duration formatter divides it by a million, so twelve candidates report
    // as a p50 of 0.000012 -- a number that looks healthy on every dashboard
    // and means nothing. Counts keep their own unsuffixed keys.
    const bool isDuration = unit == PercentileUnit::Duration;
    const QString suffix = isDuration ? QStringLiteral("_ms") : QString();

    const auto key = [&suffix](const char* percentile)
    {
        return QString::fromLatin1(percentile) + suffix;
    };

    QJsonObject result;
    result.insert(QStringLiteral("available"), !samples.isEmpty());
    result.insert(QStringLiteral("sample_count"), qint64(samples.size()));

    if (samples.isEmpty())
    {
        // Null, not zero. An absent measurement and a measurement of zero are
        // different facts, and rounding the first into the second is how a
        // dashboard reports a healthy p99 for a path that never ran.
        result.insert(key("p50"), QJsonValue(QJsonValue::Null));
        result.insert(key("p95"), QJsonValue(QJsonValue::Null));
        result.insert(key("p99"), QJsonValue(QJsonValue::Null));
        return result;
    }

    QList<qint64> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    const auto nearestRank = [&sorted](double percentile)
    {
        const qsizetype rank = qMax<qsizetype>(1, qsizetype(std::ceil(percentile * double(sorted.size()))));
        return sorted.at(qMin(rank, sorted.size()) - 1);
    };

    const auto value = [isDuration](qint64 sample)
    {
        return isDuration ? durationMs(sample) : QJsonValue(sample);
    };

    result.insert(key("p50"), value(nearestRank(0.50)));
    result.insert(key("p95"), value(nearestRank(0.95)));
    result.insert(key("p99"), value(nearestRank(0.99)));
    return result;
}

QJsonObject InteractionTraceRecorder::budgetObject(qreal refreshRateHz)
{
    QJsonObject budget;
    budget.insert(QStringLiteral("reference_60_hz_ms"), double(Reference60HzBudgetMs));
    budget.insert(QStringLiteral("reference_120_hz_ms"), double(Reference120HzBudgetMs));

    if (std::isfinite(refreshRateHz) && refreshRateHz > 0.0)
    {
        budget.insert(QStringLiteral("status"), QStringLiteral("known"));
        budget.insert(QStringLiteral("refresh_rate_hz"), double(refreshRateHz));
        budget.insert(QStringLiteral("frame_budget_ms"), 1000.0 / double(refreshRateHz));
        return budget;
    }

    budget.insert(QStringLiteral("status"), QStringLiteral("unavailable"));
    budget.insert(QStringLiteral("refresh_rate_hz"), QJsonValue(QJsonValue::Null));
    budget.insert(QStringLiteral("frame_budget_ms"), QJsonValue(QJsonValue::Null));
    budget.insert(QStringLiteral("reason"), QStringLiteral("interaction-trace/refresh-rate-unknown"));
    return budget;
}

QJsonObject InteractionTraceRecorder::summary() const
{
    QJsonObject stages;
    QJsonObject slowCauses;
    for (int index = 0; index < TraceStageCount; ++index)
    {
        const QString name = QString::fromLatin1(getTraceStageName(TraceStage(index)));
        stages.insert(name, percentileObject(m_stageDurations[size_t(index)]));
        slowCauses.insert(name, qint64(m_slowCauseCounts[size_t(index)]));
    }

    QJsonObject cache;
    cache.insert(QStringLiteral("hits"), m_cacheHits);
    cache.insert(QStringLiteral("misses"), m_cacheMisses);

    QJsonObject counts;
    counts.insert(QStringLiteral("inputs"), qint64(m_inputCount));
    counts.insert(QStringLiteral("frames"), qint64(m_frameCount));
    counts.insert(QStringLiteral("pending_inputs"), qint64(m_pendingInputs.size()));
    counts.insert(QStringLiteral("dropped_input_records"), qint64(m_droppedInputRecords));
    counts.insert(QStringLiteral("unbalanced_frames"), qint64(m_unbalancedFrames));

    // Counts and timings only, exactly as everywhere else in this summary --
    // a candidate count says how much geometry was near the pointer, never
    // what any of it was.
    QJsonObject hitTest;
    hitTest.insert(QStringLiteral("index_candidates"), countPercentileObject(m_hitTestCandidates));
    hitTest.insert(QStringLiteral("precise_hits"), countPercentileObject(m_hitTestPreciseHits));
    hitTest.insert(QStringLiteral("duration_ms"), percentileObject(m_hitTestDurations));

    QJsonObject asyncOverlap;
    for (int index = 0; index < TraceJobKindCount; ++index)
    {
        QJsonObject kindOverlap;
        kindOverlap.insert(QStringLiteral("frames_overlapped"), qint64(m_framesOverlapped[size_t(index)]));
        kindOverlap.insert(QStringLiteral("slow_frames_overlapped"),
                           qint64(m_slowFramesOverlapped[size_t(index)]));
        kindOverlap.insert(QStringLiteral("active"), m_activeJobs[size_t(index)]);
        asyncOverlap.insert(QString::fromLatin1(getTraceJobKindName(TraceJobKind(index))), kindOverlap);
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), InteractionTraceSummaryVersion);
    root.insert(QStringLiteral("trace_id"), m_trace.traceId);
    root.insert(QStringLiteral("enabled"), m_enabled);
    root.insert(QStringLiteral("budgets"), budgetObject(m_config.refreshRateHz));
    root.insert(QStringLiteral("input_to_frame_ms"), percentileObject(m_inputLatencies));
    root.insert(QStringLiteral("frame_time_ms"), percentileObject(m_frameDurations));
    root.insert(QStringLiteral("stage_ms"), stages);
    root.insert(QStringLiteral("slow_frame_causes"), slowCauses);
    root.insert(QStringLiteral("hit_test"), hitTest);
    root.insert(QStringLiteral("async_overlap"), asyncOverlap);
    root.insert(QStringLiteral("page_surface_cache"), cache);
    root.insert(QStringLiteral("counts"), counts);
    return root;
}

void InteractionTraceRecorder::reset()
{
    m_trace.inputs.clear();
    m_frame.reset();
    m_pendingInputs.clear();
    m_frameDurations.clear();
    m_inputLatencies.clear();

    for (QList<qint64>& samples : m_stageDurations)
    {
        samples.clear();
    }

    m_slowCauseCounts.fill(0);
    m_inputCount = 0;
    m_frameCount = 0;
    m_droppedInputRecords = 0;
    m_unbalancedFrames = 0;
    m_cacheHits = 0;
    m_cacheMisses = 0;

    m_hitTestCandidates.clear();
    m_hitTestPreciseHits.clear();
    m_hitTestDurations.clear();

    // Active job counts are deliberately not cleared: a reset drops recorded
    // history, but a job that was running before it is still running after,
    // and zeroing the count here would lose its next completion.
    m_framesOverlapped.fill(0);
    m_slowFramesOverlapped.fill(0);
}

}   // namespace pdfinteraction
