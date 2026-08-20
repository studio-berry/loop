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

#ifndef PDFINTERACTIONTRACE_P_H
#define PDFINTERACTIONTRACE_P_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QVector>

#include <array>
#include <functional>
#include <limits>
#include <optional>

namespace pdf
{

/// Private, opt-in recorder for the QWidget canvas interaction path.
///
/// The recorder deliberately lives in LoupeLibWidgets for Session 1. It has no
/// dependency on PDF data types and only exposes privacy-safe aggregate values.
class PDFInteractionTraceRecorder final : public QObject
{
public:
    using Clock = std::function<qint64()>;

    enum class InputKind
    {
        ShortcutOverride,
        KeyPress,
        KeyRelease,
        MousePress,
        MouseDoubleClick,
        MouseRelease,
        MouseMove,
        Wheel,
        DragEnter,
        DragMove,
        Drop
    };

    enum class Stage
    {
        Interaction,
        HitTest,
        PageRender,
        Overlay,
        Composition,
        ExternalUnknown
    };

    enum class EvidenceState
    {
        Verified,
        StaticOnly,
        InfrastructureBlocked
    };

    struct Config
    {
        qsizetype maxSamples = 2048;
        int sampleEvery = 1;
        double refreshRateHz = 0.0;
        EvidenceState evidenceState = EvidenceState::Verified;
    };

    class StageScope
    {
    public:
        StageScope() = default;
        StageScope(PDFInteractionTraceRecorder* recorder, Stage stage);
        ~StageScope();

        StageScope(const StageScope&) = delete;
        StageScope& operator=(const StageScope&) = delete;
        StageScope(StageScope&& other) noexcept;
        StageScope& operator=(StageScope&& other) noexcept;

    private:
        PDFInteractionTraceRecorder* m_recorder = nullptr;
        Stage m_stage = Stage::ExternalUnknown;
        bool m_active = false;
        PDFInteractionTraceRecorder* m_previousCurrent = nullptr;
    };

    class InputScope
    {
    public:
        InputScope() = default;
        InputScope(PDFInteractionTraceRecorder* recorder, quint64 inputId);
        ~InputScope();

        InputScope(const InputScope&) = delete;
        InputScope& operator=(const InputScope&) = delete;
        InputScope(InputScope&& other) noexcept;
        InputScope& operator=(InputScope&& other) noexcept;

        quint64 id() const { return m_inputId; }

    private:
        PDFInteractionTraceRecorder* m_recorder = nullptr;
        quint64 m_inputId = 0;
        bool m_active = false;
        PDFInteractionTraceRecorder* m_previousCurrent = nullptr;
    };

    class FrameScope
    {
    public:
        FrameScope() = default;
        FrameScope(PDFInteractionTraceRecorder* recorder, quint64 frameId);
        ~FrameScope();

        FrameScope(const FrameScope&) = delete;
        FrameScope& operator=(const FrameScope&) = delete;
        FrameScope(FrameScope&& other) noexcept;
        FrameScope& operator=(FrameScope&& other) noexcept;

        quint64 id() const { return m_frameId; }

    private:
        PDFInteractionTraceRecorder* m_recorder = nullptr;
        quint64 m_frameId = 0;
        bool m_active = false;
        PDFInteractionTraceRecorder* m_previousCurrent = nullptr;
    };

    explicit PDFInteractionTraceRecorder(Clock clock = {}, QObject* parent = nullptr);
    ~PDFInteractionTraceRecorder() override;

    PDFInteractionTraceRecorder(const PDFInteractionTraceRecorder&) = delete;
    PDFInteractionTraceRecorder& operator=(const PDFInteractionTraceRecorder&) = delete;

    static Config configFromEnvironment();

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    void setConfig(Config config);
    Config config() const { return m_config; }

    void setRefreshRateHz(double refreshRateHz);
    void setEvidenceState(EvidenceState state);
    void observeDocumentRevision(qint64 revision);

    InputScope beginInput(InputKind kind);
    FrameScope beginFrame(int visiblePages, int queueDepth);
    StageScope beginStage(Stage stage);

    void recordCacheLookup(bool hit);
    void recordSurfaceCacheLookup(bool hit);
    void recordQueueDepth(int queueDepth);

    /// Returns a compact aggregate summary. Percentiles are nearest-rank.
    QJsonObject summary() const;

    /// Returns the current recorder for the calling thread, if any scope is active.
    static PDFInteractionTraceRecorder* current();

    QString traceId() const { return m_traceId; }

private:
    struct PendingInput
    {
        quint64 id = 0;
        qint64 startNs = 0;
        InputKind kind = InputKind::MouseMove;
    };

    struct ActiveStage
    {
        Stage stage = Stage::ExternalUnknown;
        qint64 startNs = 0;
        qint64 childNs = 0;
    };

    struct Frame
    {
        quint64 id = 0;
        qint64 startNs = 0;
        int visiblePages = -1;
        int queueDepth = -1;
        int cacheHits = 0;
        int cacheMisses = 0;
        bool sampled = true;
        std::array<qint64, 6> stageNs{};
    };

    struct CompletedFrame
    {
        qint64 durationNs = 0;
        qint64 inputToFrameNs = 0;
        bool hasInput = false;
        int visiblePages = -1;
        int queueDepth = -1;
        int cacheHits = 0;
        int cacheMisses = 0;
        std::array<qint64, 6> stageNs{};
    };

    struct Acknowledgement
    {
        quint64 inputId = 0;
        quint64 frameId = 0;
    };

    static constexpr int StageCount = 6;

    qint64 nowNs() const;
    void resetTrace();
    void finishInput(quint64 inputId);
    void finishStage(Stage stage);
    void finishFrame(quint64 frameId);
    void appendSample(QVector<qint64>& samples, qint64 value);
    void emitSummary() const;
    void refreshCachedSummary() const;

    static int stageIndex(Stage stage);
    static QString stageName(Stage stage);
    static QString evidenceName(EvidenceState state);
    static QJsonObject percentileObject(const QVector<qint64>& samples);
    static QJsonValue durationMs(qint64 nanoseconds);
    static QJsonObject budgetObject(double refreshRateHz);

    Clock m_clock;
    QElapsedTimer m_elapsedTimer;
    Config m_config;
    bool m_enabled = false;
    bool m_hasData = false;
    QString m_traceId;
    quint64 m_nextInputId = 1;
    quint64 m_nextFrameId = 1;
    quint64 m_inputCounter = 0;
    quint64 m_frameCounter = 0;
    quint64 m_documentRevisionOrdinal = 0;
    qint64 m_lastDocumentRevision = std::numeric_limits<qint64>::min();
    mutable qint64 m_lastSummaryNs = std::numeric_limits<qint64>::min();
    mutable QJsonObject m_cachedSummary;
    std::optional<Frame> m_frame;
    QVector<PendingInput> m_pendingInputs;
    QVector<ActiveStage> m_activeStages;
    QVector<qint64> m_frameDurations;
    QVector<qint64> m_inputLatencies;
    QVector<CompletedFrame> m_completedFrames;
    QVector<Acknowledgement> m_acknowledgements;
    std::array<QVector<qint64>, StageCount> m_stageDurations;
    std::array<qint64, StageCount> m_totalStageNs{};
    std::array<quint64, StageCount> m_slowCauseCounts{};
    int m_lastVisiblePages = -1;
    int m_lastQueueDepth = -1;
    int m_totalCacheHits = 0;
    int m_totalCacheMisses = 0;
    int m_totalSurfaceCacheHits = 0;
    int m_totalSurfaceCacheMisses = 0;
    quint64 m_lastAcknowledgedInputId = 0;
    quint64 m_lastAcknowledgedFrameId = 0;

    static thread_local PDFInteractionTraceRecorder* s_current;
};

}   // namespace pdf

#endif // PDFINTERACTIONTRACE_P_H
