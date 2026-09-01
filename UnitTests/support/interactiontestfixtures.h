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

#ifndef INTERACTIONTESTFIXTURES_H
#define INTERACTIONTESTFIXTURES_H

#include "interactiontrace.h"
#include "jobrelay.h"
#include "jobsubmitter.h"

#include "pdfjobscheduler.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <memory>
#include <utility>

/// Fixtures shared by the interaction-layer test suites.
///
/// These exist because issue #146 needs a trace that replays identically on
/// every runner. Six suites already grew their own submitter fake; those are
/// left alone deliberately -- rewriting them would bury the change this header
/// is part of. New suites use these.
namespace loupetest
{

/// A submitter that keeps pdf::PDFJobScheduler's contract but lets the test
/// decide when work runs.
///
/// It is not a second scheduler: no threads, no priority queue, no capacity
/// rule. Work sits in submission order until the test pumps it, and then runs
/// inline on the test thread. That is what makes "drag during preflight"
/// reproducible -- the preflight body executes at the same input index on every
/// run, on every machine.
class DeterministicJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        const QString jobId = spec.jobId.isEmpty()
                                  ? QStringLiteral("job-%1").arg(++m_sequence)
                                  : spec.jobId;

        spec.jobId = jobId;
        m_submitted.append(spec);
        m_status.insert(jobId, pdf::PDFJobStatus::Queued);
        m_pending.append({ jobId, std::move(work) });
        return jobId;
    }

    bool cancel(const QString& jobId) override
    {
        m_cancelled.append(jobId);

        if (m_status.value(jobId, pdf::PDFJobStatus::Succeeded) != pdf::PDFJobStatus::Queued)
        {
            // Matches the scheduler: cancelling a job that already left the
            // queue is a request, not a guarantee.
            return false;
        }

        m_status.insert(jobId, pdf::PDFJobStatus::Cancelled);
        removePending(jobId);
        return true;
    }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = m_status.value(jobId, pdf::PDFJobStatus::Succeeded);
        result.queueDepth = int(m_pending.size());
        return result;
    }

    void publishCurrentRevision(const QString& documentKey,
                                const pdf::PDFRevisionIdentity& revision) override
    {
        m_publishedRevisions.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override
    {
        m_publishedRevisions.remove(documentKey);
        m_clearedKeys.append(documentKey);
    }

    /// Jobs submitted and not yet run, cancelled or dropped.
    int pending() const noexcept { return int(m_pending.size()); }

    /// Runs the oldest pending job. False when there is none.
    bool pumpOne()
    {
        if (m_pending.isEmpty())
        {
            return false;
        }

        const PendingJob job = m_pending.takeFirst();
        runJob(job);
        return true;
    }

    /// Runs every pending job, including any a running job submits, and returns
    /// how many ran. Bounded so a job that resubmits itself fails the test
    /// rather than hanging it.
    int pumpAll(int maxJobs = 1024)
    {
        int ran = 0;

        while (ran < maxJobs && pumpOne())
        {
            ++ran;
        }

        return ran;
    }

    /// The next job to run reports this error instead of executing. Cleared
    /// once used.
    void failNext(QString errorMessage) { m_failNext = std::move(errorMessage); }

    const QList<pdf::PDFJobSpec>& submitted() const noexcept { return m_submitted; }
    const QList<QString>& cancelled() const noexcept { return m_cancelled; }
    const QList<QString>& clearedKeys() const noexcept { return m_clearedKeys; }

    pdf::PDFRevisionIdentity publishedRevision(const QString& documentKey) const
    {
        return m_publishedRevisions.value(documentKey);
    }

    pdf::PDFJobStatus status(const QString& jobId) const
    {
        return m_status.value(jobId, pdf::PDFJobStatus::Queued);
    }

    /// The cancellation token handed to a running job, so a test can cancel it
    /// mid-body the way the scheduler would.
    pdf::PDFJobCancellationTokenPtr token(const QString& jobId) const
    {
        return m_tokens.value(jobId);
    }

private:
    struct PendingJob
    {
        QString jobId;
        pdf::PDFJobWork work;
    };

    void removePending(const QString& jobId)
    {
        for (qsizetype index = 0; index < m_pending.size(); ++index)
        {
            if (m_pending.at(index).jobId == jobId)
            {
                m_pending.removeAt(index);
                return;
            }
        }
    }

    void runJob(const PendingJob& job)
    {
        if (!m_failNext.isEmpty())
        {
            m_failNext.clear();
            m_status.insert(job.jobId, pdf::PDFJobStatus::Failed);
            return;
        }

        auto cancellationToken = std::make_shared<pdf::PDFJobCancellationToken>();
        m_tokens.insert(job.jobId, cancellationToken);
        m_status.insert(job.jobId, pdf::PDFJobStatus::Running);

        pdf::PDFJobContext context(cancellationToken, pdf::PDFProcessingLimits(), [](int) {});

        if (job.work)
        {
            job.work(context);
        }

        m_status.insert(job.jobId,
                        cancellationToken->isCancellationRequested()
                            ? pdf::PDFJobStatus::Cancelled
                            : pdf::PDFJobStatus::Succeeded);
    }

    quint64 m_sequence = 0;
    QList<pdf::PDFJobSpec> m_submitted;
    QList<PendingJob> m_pending;
    QList<QString> m_cancelled;
    QList<QString> m_clearedKeys;
    QHash<QString, pdf::PDFJobStatus> m_status;
    QHash<QString, pdf::PDFRevisionIdentity> m_publishedRevisions;
    QHash<QString, pdf::PDFJobCancellationTokenPtr> m_tokens;
    QString m_failNext;
};

/// Holds a JobRelay the way a real owner does, and lets a test destroy the
/// owner at a chosen moment.
///
/// The interesting case is issue #144 AC6: a completion that arrives after the
/// owner is gone must do nothing. A test cannot express that with a bare relay,
/// because the ordering being verified is the owner's detach against the queued
/// action.
class RecordingRelayOwner
{
public:
    RecordingRelayOwner() :
        m_relay(std::make_shared<pdfinteraction::JobRelay>())
    {
    }

    ~RecordingRelayOwner() { detachNow(); }

    RecordingRelayOwner(const RecordingRelayOwner&) = delete;
    RecordingRelayOwner& operator=(const RecordingRelayOwner&) = delete;

    const std::shared_ptr<pdfinteraction::JobRelay>& relay() const noexcept { return m_relay; }

    /// What the owner's destructor does, on the owner's thread.
    void detachNow()
    {
        if (m_relay && !m_detached)
        {
            m_relay->detach();
            m_detached = true;
        }
    }

    bool isDetached() const noexcept { return m_detached; }

    /// Incremented every time an action actually reached the owner.
    int deliveries() const noexcept { return m_deliveries; }
    void recordDelivery() noexcept { ++m_deliveries; }

private:
    std::shared_ptr<pdfinteraction::JobRelay> m_relay;
    bool m_detached = false;
    int m_deliveries = 0;
};

/// Reads an InteractionTraceRecorder summary the way issue #146's contract
/// assertions do.
///
/// Free functions over the JSON rather than recorder methods, on purpose: the
/// CI artifact is the JSON, so a test that asserts against the object is
/// asserting against the thing that ships.
struct TraceAssert
{
    /// True when p95 input-to-frame latency is within `budgetMs`. A summary with
    /// no samples is not a pass: it reports `available: false`, and
    /// `firstViolation` says so rather than letting missing telemetry read as
    /// zero.
    static bool p95WithinBudget(const QJsonObject& summary,
                                qreal budgetMs,
                                QString* firstViolation = nullptr)
    {
        const QJsonObject latency = summary.value(QStringLiteral("input_to_frame_ms")).toObject();

        if (!latency.value(QStringLiteral("available")).toBool(false))
        {
            if (firstViolation)
            {
                *firstViolation = QStringLiteral("input latency unavailable");
            }

            return false;
        }

        const qreal p95 = latency.value(QStringLiteral("p95_ms")).toDouble();

        if (p95 > budgetMs)
        {
            if (firstViolation)
            {
                *firstViolation =
                    QStringLiteral("p95 input-to-frame %1 ms exceeds %2 ms").arg(p95).arg(budgetMs);
            }

            return false;
        }

        return true;
    }

    /// The stage most slow frames were charged to, or an empty string when no
    /// frame missed its budget. This is what issue #146 AC7 asks a failure to
    /// name.
    static QString failurePhase(const QJsonObject& summary)
    {
        const QJsonObject causes = summary.value(QStringLiteral("slow_frame_causes")).toObject();

        QString worstStage;
        double worstCount = 0.0;

        for (auto it = causes.constBegin(); it != causes.constEnd(); ++it)
        {
            const double count = it.value().toDouble();

            if (count > worstCount)
            {
                worstCount = count;
                worstStage = it.key();
            }
        }

        return worstStage;
    }
};

}   // namespace loupetest

#endif   // INTERACTIONTESTFIXTURES_H
