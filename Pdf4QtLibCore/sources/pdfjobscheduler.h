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

#ifndef PDFJOBSCHEDULER_H
#define PDFJOBSCHEDULER_H

#include "pdfartifactidentity.h"
#include "pdfoperationcontrol.h"
#include "pdfprocessingbudget.h"

#include <QDateTime>
#include <QHashFunctions>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pdf
{

struct PDFJobStringHash
{
    size_t operator()(const QString& value) const noexcept
    {
        return static_cast<size_t>(qHash(value));
    }
};

enum class PDF4QTLIBCORESHARED_EXPORT PDFJobPriority
{
    Interaction = 0,
    VisiblePage = 1,
    NearViewport = 2,
    Operator = 3,
    Background = 4,
    Agent = 5
};

enum class PDF4QTLIBCORESHARED_EXPORT PDFJobKind
{
    Rendering,
    Preflight,
    OCR,
    Export,
    Thumbnail,
    Batch,
    Agent,
    Other
};

enum class PDF4QTLIBCORESHARED_EXPORT PDFJobStatus
{
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Stale
};

enum class PDF4QTLIBCORESHARED_EXPORT PDFJobStaleResultPolicy
{
    Discard,
    Deliver
};

PDF4QTLIBCORESHARED_EXPORT const char* getPDFJobPriorityName(PDFJobPriority priority);
PDF4QTLIBCORESHARED_EXPORT const char* getPDFJobKindName(PDFJobKind kind);
PDF4QTLIBCORESHARED_EXPORT const char* getPDFJobStatusName(PDFJobStatus status);

class PDF4QTLIBCORESHARED_EXPORT PDFJobCancellationToken final : public PDFOperationControl
{
public:
    void cancel() noexcept;
    bool isCancellationRequested() const noexcept;
    bool isOperationCancelled() const override { return isCancellationRequested(); }

private:
    std::atomic_bool m_cancelled = false;
};

using PDFJobCancellationTokenPtr = std::shared_ptr<PDFJobCancellationToken>;

struct PDF4QTLIBCORESHARED_EXPORT PDFJobSpec
{
    QString jobId;
    PDFJobKind kind = PDFJobKind::Other;
    PDFJobPriority priority = PDFJobPriority::Background;
    PDFArtifactIdentity artifact;
    QString documentKey;
    QString documentRevision;
    QString operationId;
    QString checkId;
    QString progressModel;
    PDFProcessingLimits processingLimits;
    PDFJobStaleResultPolicy staleResultPolicy = PDFJobStaleResultPolicy::Discard;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFJobSnapshot
{
    QString jobId;
    PDFJobKind kind = PDFJobKind::Other;
    PDFJobPriority priority = PDFJobPriority::Background;
    PDFJobStatus status = PDFJobStatus::Queued;
    PDFArtifactIdentity artifact;
    QString documentKey;
    QString documentRevision;
    QString operationId;
    QString checkId;
    QString progressModel;
    QString resultSummary;
    QString errorMessage;
    PDFArtifactIdentity outputArtifact;
    int progress = 0;
    int queueDepth = 0;
    qint64 queueWaitMs = 0;
    qint64 durationMs = 0;
    qint64 cancellationLatencyMs = -1;
    QDateTime queuedAtUtc;
    QDateTime startedAtUtc;
    QDateTime finishedAtUtc;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFJobTraceEvent
{
    QString jobId;
    PDFJobStatus status = PDFJobStatus::Queued;
    PDFJobPriority priority = PDFJobPriority::Background;
    int queueDepth = 0;
    int progress = 0;
    qint64 elapsedMs = 0;
    qint64 cancellationLatencyMs = -1;
    QDateTime timestampUtc;
};

class PDF4QTLIBCORESHARED_EXPORT PDFJobContext final
{
public:
    using ProgressReporter = std::function<void(int)>;

    PDFJobContext(PDFJobCancellationTokenPtr cancellationToken,
                  PDFProcessingLimits limits,
                  ProgressReporter progressReporter);

    const PDFOperationControl* operationControl() const noexcept { return m_cancellationToken.get(); }
    bool isCancellationRequested() const noexcept;
    PDFProcessingBudget& processingBudget() noexcept { return m_processingBudget; }
    void reportProgress(int percentage);
    int progress() const noexcept { return m_progress.load(std::memory_order_acquire); }
    void setResultSummary(QString summary);
    QString resultSummary() const;
    void setOutputArtifact(PDFArtifactIdentity artifact);
    const PDFArtifactIdentity& outputArtifact() const noexcept { return m_outputArtifact; }

private:
    PDFJobCancellationTokenPtr m_cancellationToken;
    PDFProcessingBudget m_processingBudget;
    ProgressReporter m_progressReporter;
    std::atomic_int m_progress = 0;
    mutable std::mutex m_mutex;
    QString m_resultSummary;
    PDFArtifactIdentity m_outputArtifact;
};

using PDFJobWork = std::function<void(PDFJobContext&)>;

class PDF4QTLIBCORESHARED_EXPORT PDFJobScheduler final : public QObject
{
    Q_OBJECT

public:
    explicit PDFJobScheduler(int workerCount = 0, QObject* parent = nullptr);
    ~PDFJobScheduler() override;

    PDFJobScheduler(const PDFJobScheduler&) = delete;
    PDFJobScheduler& operator=(const PDFJobScheduler&) = delete;

    static PDFJobScheduler& global();

    QString submit(PDFJobSpec spec, PDFJobWork work, PDFJobCancellationTokenPtr* cancellationToken = nullptr);
    bool cancel(const QString& jobId);
    bool waitForFinished(const QString& jobId, int timeoutMs = -1) const;
    PDFJobSnapshot snapshot(const QString& jobId) const;
    QList<PDFJobSnapshot> queuedJobs() const;
    QList<PDFJobSnapshot> runningJobs() const;
    QList<PDFJobTraceEvent> trace(const QString& jobId = {}) const;

    void setCurrentRevision(QString documentKey, QString documentRevision);
    void clearCurrentRevision(const QString& documentKey);

    int workerCount() const noexcept { return m_workerCount; }

signals:
    void jobQueued(pdf::PDFJobSnapshot snapshot);
    void jobStarted(pdf::PDFJobSnapshot snapshot);
    void jobProgress(pdf::PDFJobSnapshot snapshot);
    void jobFinished(pdf::PDFJobSnapshot snapshot);

private:
    struct JobEntry;
    struct JobCompare
    {
        bool operator()(const std::shared_ptr<JobEntry>& left,
                        const std::shared_ptr<JobEntry>& right) const;
    };

    void workerLoop();
    void finishJob(const std::shared_ptr<JobEntry>& job, PDFJobStatus status, QString errorMessage = {});
    void appendTrace(const std::shared_ptr<JobEntry>& job, PDFJobStatus status, qint64 elapsedMs = 0);
    bool isStale(const PDFJobSpec& spec) const;
    PDFJobSnapshot snapshotLocked(const JobEntry& job) const;
    static QString resolvedDocumentKey(const PDFJobSpec& spec);

    const int m_workerCount;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_condition;
    mutable std::condition_variable m_finishedCondition;
    bool m_stopping = false;
    quint64 m_sequence = 0;
    int m_activeBackgroundJobs = 0;
    std::priority_queue<std::shared_ptr<JobEntry>, std::vector<std::shared_ptr<JobEntry>>, JobCompare> m_queue;
    std::unordered_map<QString, std::shared_ptr<JobEntry>, PDFJobStringHash> m_jobs;
    std::unordered_map<QString, QString, PDFJobStringHash> m_currentRevisions;
    std::unordered_map<QString, QList<PDFJobTraceEvent>, PDFJobStringHash> m_traces;
    std::vector<std::thread> m_workers;
};

} // namespace pdf

Q_DECLARE_METATYPE(pdf::PDFJobPriority)
Q_DECLARE_METATYPE(pdf::PDFJobKind)
Q_DECLARE_METATYPE(pdf::PDFJobStatus)
Q_DECLARE_METATYPE(pdf::PDFJobSnapshot)
Q_DECLARE_METATYPE(pdf::PDFJobTraceEvent)

#endif // PDFJOBSCHEDULER_H
