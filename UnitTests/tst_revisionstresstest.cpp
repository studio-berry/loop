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

// Concurrent revision-authority stress scenario.
//
// tst_documentsessiontest.cpp proves the fence in isolation: one orchestrated
// round, all producers released after the mutation. This file runs the
// acceptance scenario instead - render, preflight, thumbnail, and repair-plan
// jobs in flight simultaneously while the document is mutated at points the
// producers do not observe - and asserts the four correctness properties:
//
//   1. zero stale findings applied;
//   2. zero stale tiles presented as current past an invalidation boundary;
//   3. deterministic cancellation (cancelled work is terminal, never success,
//      and never publishes a result);
//   4. no cache ever returns a result for the wrong revision.
//
// "Zero stale" is a correctness requirement here, not a percentile: a single
// admitted stale result fails the test.

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentsession.h"
#include "pdfjobscheduler.h"

#include <QtTest>

#include <atomic>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace
{

constexpr int RoundCount = 64;
constexpr int PageCount = 4;
constexpr int WorkerCount = 4;
constexpr int JobWaitTimeoutMs = 15000;

/// The single place where an asynchronous result becomes visible state.
///
/// Product consumers - the findings model, the tile presenter, the evidence
/// cache - all follow the same rule, so the stress test models them with one
/// type: publish() moves the fence, apply() admits a result only when the
/// result's complete revision still equals the fence, and lookup() refuses to
/// hand back an entry the fence has since superseded.
///
/// One mutex covers the fence and the retained entries together. Without that,
/// a mutation on the owning thread and a result arriving on a worker could
/// interleave into an accepted stale entry - exactly the defect the revision
/// authority exists to make impossible.
class RevisionGate
{
public:
    explicit RevisionGate(pdf::PDFRevisionIdentity revision) :
        m_current(std::move(revision))
    {
    }

    /// Moves the fence. Everything computed against the previous revision is
    /// dropped rather than reconciled, so nothing survives the boundary.
    void publish(pdf::PDFRevisionIdentity revision)
    {
        std::lock_guard lock(m_mutex);
        m_current = std::move(revision);
        m_entries.clear();
    }

    /// Admits a result computed against `revision`. Returns whether it became
    /// visible state.
    bool apply(const QString& kind, int page, const pdf::PDFRevisionIdentity& revision)
    {
        std::lock_guard lock(m_mutex);
        if (!(revision == m_current))
        {
            ++m_rejected;
            return false;
        }

        m_entries.insert(entryKey(kind, page), revision);
        ++m_applied;
        return true;
    }

    /// Cache read. A hit that does not carry the current revision is a defect,
    /// not a miss to be reconciled - it is recorded and the entry is dropped.
    std::optional<pdf::PDFRevisionIdentity> lookup(const QString& kind, int page)
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_entries.constFind(entryKey(kind, page));
        if (it == m_entries.constEnd())
        {
            return std::nullopt;
        }

        if (!(it.value() == m_current))
        {
            recordViolationLocked(QStringLiteral("cache returned %1 for %2, current is %3")
                                      .arg(it.value().toString(), entryKey(kind, page), m_current.toString()));
            m_entries.remove(entryKey(kind, page));
            return std::nullopt;
        }

        return it.value();
    }

    /// Audits every retained entry against the fence.
    void auditRetainedEntries()
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        {
            if (!(it.value() == m_current))
            {
                recordViolationLocked(QStringLiteral("stale entry %1 retained at revision %2, current is %3")
                                          .arg(it.key(), it.value().toString(), m_current.toString()));
            }
        }
    }

    void recordViolation(QString description)
    {
        std::lock_guard lock(m_mutex);
        recordViolationLocked(std::move(description));
    }

    QStringList violations() const
    {
        std::lock_guard lock(m_mutex);
        return m_violations;
    }

    int appliedCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_applied;
    }

private:
    static QString entryKey(const QString& kind, int page)
    {
        return QStringLiteral("%1/%2").arg(kind).arg(page);
    }

    void recordViolationLocked(QString description)
    {
        // Bounded: a broken fence would otherwise produce thousands of lines
        // and bury the first, most diagnosable failure.
        if (m_violations.size() < 16)
        {
            m_violations.append(std::move(description));
        }
    }

    mutable std::mutex m_mutex;
    pdf::PDFRevisionIdentity m_current;
    QHash<QString, pdf::PDFRevisionIdentity> m_entries;
    QStringList m_violations;
    int m_applied = 0;
    int m_rejected = 0;
};

pdf::PDFDocument buildDocument()
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < PageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 100, 100));
    }
    return builder.build();
}

pdf::PDFArtifactIdentity buildArtifact(const QString& storageToken)
{
    pdf::PDFArtifactIdentity artifact;
    artifact.sha256 = QString(64, QLatin1Char('b'));
    artifact.size = 4096;
    artifact.logicalName = QStringLiteral("revision-stress.pdf");
    artifact.storageToken = storageToken;
    return artifact;
}

struct JobKindSpec
{
    pdf::PDFJobKind kind;
    pdf::PDFJobPriority priority;
    const char* consumer;
    const char* operationId;
};

constexpr JobKindSpec JobKinds[] = {
    { pdf::PDFJobKind::Rendering, pdf::PDFJobPriority::VisiblePage, "tile", "render" },
    { pdf::PDFJobKind::Preflight, pdf::PDFJobPriority::Operator, "finding", "preflight" },
    { pdf::PDFJobKind::Thumbnail, pdf::PDFJobPriority::NearViewport, "tile", "thumbnail" },
    { pdf::PDFJobKind::Other, pdf::PDFJobPriority::Operator, "finding", "repair-plan" }
};

constexpr int JobKindCount = int(std::size(JobKinds));

}   // namespace

class RevisionStressTest : public QObject
{
    Q_OBJECT

private slots:
    void concurrentJobsNeverPublishStaleResults();
    void cancellationIsDeterministicAndPublishesNothing();
    void sessionCachesNeverServeSupersededRevisions();
};

void RevisionStressTest::concurrentJobsNeverPublishStaleResults()
{
    pdf::PDFDocument document = buildDocument();
    pdf::PDFDocumentContext context(&document);
    pdf::PDFJobScheduler scheduler(WorkerCount);

    const QString documentKey = context.getDocumentIdentity().documentId;
    const pdf::PDFArtifactIdentity artifact = buildArtifact(documentKey);

    RevisionGate gate(context.getRevision());
    scheduler.setCurrentRevision(documentKey, context.getRevision().toString());

    QStringList jobIds;

    for (int round = 0; round < RoundCount; ++round)
    {
        for (int index = 0; index < JobKindCount; ++index)
        {
            const JobKindSpec& kindSpec = JobKinds[index];
            const int page = (round + index) % PageCount;

            // The revision is captured with the submission, exactly as a product
            // producer captures it, and travels with the result.
            const pdf::PDFRevisionIdentity submittedRevision = context.getRevision();

            pdf::PDFJobSpec spec;
            spec.kind = kindSpec.kind;
            spec.priority = kindSpec.priority;
            spec.artifact = artifact;
            spec.documentKey = documentKey;
            spec.documentRevision = submittedRevision.toString();
            spec.operationId = QString::fromLatin1(kindSpec.operationId);
            spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

            const QString consumer = QString::fromLatin1(kindSpec.consumer);
            jobIds.append(scheduler.submit(spec,
                                           [&gate, submittedRevision, consumer, page, index](pdf::PDFJobContext& jobContext)
                                           {
                                               // Simulated work of varying length, so producers finish on
                                               // both sides of the mutations happening on the owning thread.
                                               for (int step = 0; step < (index % 3) + 1; ++step)
                                               {
                                                   if (jobContext.isCancellationRequested())
                                                   {
                                                       return;
                                                   }
                                                   std::this_thread::yield();
                                               }

                                               gate.apply(consumer, page, submittedRevision);
                                           }));
        }

        // Mutate while earlier rounds are still running. The mutation and the
        // fence publication happen on the owning thread; producers never touch
        // the context.
        if (round % 2 == 0)
        {
            context.markModified(pdf::PDFModifiedDocument::PageContents);
            const pdf::PDFRevisionIdentity currentRevision = context.getRevision();
            scheduler.setCurrentRevision(documentKey, currentRevision.toString());
            gate.publish(currentRevision);
        }
        else if (round % 5 == 0)
        {
            // A profile change fences profile-dependent entries without
            // pretending the PDF bytes changed.
            context.setEffectiveProfileIdentity(QStringLiteral("profile-%1").arg(round));
            const pdf::PDFRevisionIdentity currentRevision = context.getRevision();
            scheduler.setCurrentRevision(documentKey, currentRevision.toString());
            gate.publish(currentRevision);
        }

        // Read back through the cache while producers are still active.
        for (int page = 0; page < PageCount; ++page)
        {
            const std::optional<pdf::PDFRevisionIdentity> tile = gate.lookup(QStringLiteral("tile"), page);
            if (tile.has_value() && !context.isCurrent(tile.value()))
            {
                gate.recordViolation(QStringLiteral("tile cache served %1 outside the current revision")
                                         .arg(tile.value().toString()));
            }
        }
    }

    for (const QString& jobId : std::as_const(jobIds))
    {
        QVERIFY2(scheduler.waitForFinished(jobId, JobWaitTimeoutMs),
                 qPrintable(QStringLiteral("job %1 did not reach a terminal state").arg(jobId)));

        const pdf::PDFJobSnapshot snapshot = scheduler.snapshot(jobId);
        QVERIFY2(snapshot.status == pdf::PDFJobStatus::Succeeded ||
                     snapshot.status == pdf::PDFJobStatus::Stale,
                 qPrintable(QStringLiteral("job %1 finished as %2")
                                .arg(jobId, QString::fromLatin1(pdf::getPDFJobStatusName(snapshot.status)))));
        QCOMPARE(snapshot.documentKey, documentKey);
    }

    gate.auditRetainedEntries();
    QVERIFY2(gate.violations().isEmpty(), qPrintable(gate.violations().join(QStringLiteral("; "))));

    // A fence that rejected everything would satisfy every assertion above, so
    // first pin down the other half of the rule: a result carrying the current
    // revision is admitted, and reads it back as current.
    const int appliedBeforeCurrentPhase = gate.appliedCount();
    QStringList currentJobIds;

    for (int index = 0; index < JobKindCount; ++index)
    {
        const JobKindSpec& kindSpec = JobKinds[index];
        const pdf::PDFRevisionIdentity submittedRevision = context.getRevision();

        pdf::PDFJobSpec spec;
        spec.jobId = QStringLiteral("current-%1").arg(index);
        spec.kind = kindSpec.kind;
        spec.priority = kindSpec.priority;
        spec.artifact = artifact;
        spec.documentKey = documentKey;
        spec.documentRevision = submittedRevision.toString();
        spec.operationId = QString::fromLatin1(kindSpec.operationId);
        spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

        const QString consumer = QString::fromLatin1(kindSpec.consumer);
        currentJobIds.append(scheduler.submit(spec,
                                              [&gate, submittedRevision, consumer, index](pdf::PDFJobContext&)
                                              {
                                                  if (!gate.apply(consumer, index, submittedRevision))
                                                  {
                                                      gate.recordViolation(QStringLiteral("current result for %1/%2 was rejected")
                                                                               .arg(consumer)
                                                                               .arg(index));
                                                  }
                                              }));
    }

    for (const QString& jobId : std::as_const(currentJobIds))
    {
        QVERIFY(scheduler.waitForFinished(jobId, JobWaitTimeoutMs));
        QCOMPARE(scheduler.snapshot(jobId).status, pdf::PDFJobStatus::Succeeded);
    }

    QCOMPARE(gate.appliedCount(), appliedBeforeCurrentPhase + JobKindCount);
    for (int index = 0; index < JobKindCount; ++index)
    {
        const std::optional<pdf::PDFRevisionIdentity> entry =
            gate.lookup(QString::fromLatin1(JobKinds[index].consumer), index);
        QVERIFY(entry.has_value());
        QVERIFY(context.isCurrent(entry.value()));
    }

    QVERIFY2(gate.violations().isEmpty(), qPrintable(gate.violations().join(QStringLiteral("; "))));

    // The churn above cannot guarantee, by timing alone, that a result was ever
    // actually superseded mid-flight, and a stress test that silently stops
    // exercising the fence is worse than no test. This phase forces it: one
    // producer per job kind is held inside its work function, the document is
    // mutated underneath all of them, and only then are they released.
    const int appliedBeforeSupersession = gate.appliedCount();
    std::atomic_bool releaseProducers = false;
    std::atomic_int heldProducers = 0;
    std::atomic_int rejectedResults = 0;
    QStringList heldJobIds;

    for (int index = 0; index < JobKindCount; ++index)
    {
        const JobKindSpec& kindSpec = JobKinds[index];
        const pdf::PDFRevisionIdentity submittedRevision = context.getRevision();

        pdf::PDFJobSpec spec;
        spec.jobId = QStringLiteral("superseded-%1").arg(index);
        spec.kind = kindSpec.kind;
        spec.priority = kindSpec.priority;
        spec.artifact = artifact;
        spec.documentKey = documentKey;
        spec.documentRevision = submittedRevision.toString();
        spec.operationId = QString::fromLatin1(kindSpec.operationId);
        spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

        const QString consumer = QString::fromLatin1(kindSpec.consumer);
        heldJobIds.append(scheduler.submit(spec,
                                           [&gate, &releaseProducers, &heldProducers, &rejectedResults, submittedRevision, consumer, index](pdf::PDFJobContext&)
                                           {
                                               ++heldProducers;
                                               while (!releaseProducers.load(std::memory_order_acquire))
                                               {
                                                   std::this_thread::yield();
                                               }

                                               if (!gate.apply(consumer, index, submittedRevision))
                                               {
                                                   ++rejectedResults;
                                               }
                                           }));
    }

    QTRY_VERIFY_WITH_TIMEOUT(heldProducers.load(std::memory_order_acquire) == JobKindCount, 5000);

    const pdf::PDFRevisionIdentity supersededRevision = context.getRevision();
    context.markModified(pdf::PDFModifiedDocument::PageContents);
    const pdf::PDFRevisionIdentity currentRevision = context.getRevision();
    QVERIFY(currentRevision.documentRevision > supersededRevision.documentRevision);
    QVERIFY(currentRevision.cacheGeneration > supersededRevision.cacheGeneration);
    scheduler.setCurrentRevision(documentKey, currentRevision.toString());
    gate.publish(currentRevision);
    releaseProducers.store(true, std::memory_order_release);

    for (const QString& jobId : std::as_const(heldJobIds))
    {
        QVERIFY(scheduler.waitForFinished(jobId, JobWaitTimeoutMs));

        // Rejected twice over: by the consumer's fence check, and by the
        // scheduler before the result is reported as a success.
        const pdf::PDFJobSnapshot snapshot = scheduler.snapshot(jobId);
        QCOMPARE(snapshot.status, pdf::PDFJobStatus::Stale);
        QCOMPARE(snapshot.documentRevision, supersededRevision.toString());
    }

    QCOMPARE(rejectedResults.load(std::memory_order_acquire), JobKindCount);
    QCOMPARE(gate.appliedCount(), appliedBeforeSupersession);

    gate.auditRetainedEntries();
    QVERIFY2(gate.violations().isEmpty(), qPrintable(gate.violations().join(QStringLiteral("; "))));
}

void RevisionStressTest::cancellationIsDeterministicAndPublishesNothing()
{
    pdf::PDFDocument document = buildDocument();
    pdf::PDFDocumentContext context(&document);
    pdf::PDFJobScheduler scheduler(WorkerCount);

    const QString documentKey = context.getDocumentIdentity().documentId;
    const pdf::PDFArtifactIdentity artifact = buildArtifact(documentKey);

    RevisionGate gate(context.getRevision());
    scheduler.setCurrentRevision(documentKey, context.getRevision().toString());

    std::atomic_int startedJobs = 0;
    std::atomic_int cancellationsObserved = 0;
    QStringList jobIds;

    for (int index = 0; index < JobKindCount; ++index)
    {
        const JobKindSpec& kindSpec = JobKinds[index];
        const pdf::PDFRevisionIdentity submittedRevision = context.getRevision();

        pdf::PDFJobSpec spec;
        spec.jobId = QStringLiteral("cancelled-%1").arg(index);
        spec.kind = kindSpec.kind;
        spec.priority = kindSpec.priority;
        spec.artifact = artifact;
        spec.documentKey = documentKey;
        spec.documentRevision = submittedRevision.toString();
        spec.operationId = QString::fromLatin1(kindSpec.operationId);
        spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

        const QString consumer = QString::fromLatin1(kindSpec.consumer);
        jobIds.append(scheduler.submit(spec,
                                       [&gate, &startedJobs, &cancellationsObserved, submittedRevision, consumer, index](pdf::PDFJobContext& jobContext)
                                       {
                                           ++startedJobs;

                                           // Runs until cancellation is observed, so the outcome
                                           // does not depend on timing: this job never completes
                                           // its work on its own.
                                           while (!jobContext.isCancellationRequested())
                                           {
                                               std::this_thread::yield();
                                           }

                                           // Publication is guarded by the cancellation check, as
                                           // in a real producer. Cancelled work publishes nothing.
                                           if (jobContext.isCancellationRequested())
                                           {
                                               ++cancellationsObserved;
                                               return;
                                           }

                                           gate.apply(consumer, index, submittedRevision);
                                       }));
    }

    QTRY_VERIFY_WITH_TIMEOUT(startedJobs.load(std::memory_order_acquire) == JobKindCount, 5000);

    for (const QString& jobId : std::as_const(jobIds))
    {
        QVERIFY(scheduler.cancel(jobId));
    }

    for (const QString& jobId : std::as_const(jobIds))
    {
        QVERIFY(scheduler.waitForFinished(jobId, JobWaitTimeoutMs));

        const pdf::PDFJobSnapshot snapshot = scheduler.snapshot(jobId);
        QCOMPARE(snapshot.status, pdf::PDFJobStatus::Cancelled);
        QVERIFY(snapshot.cancellationLatencyMs >= 0);

        // Cancellation is terminal: a second request finds nothing to cancel and
        // the status does not drift afterwards.
        QVERIFY(!scheduler.cancel(jobId));
        QCOMPARE(scheduler.snapshot(jobId).status, pdf::PDFJobStatus::Cancelled);
    }

    // The jobs above return only after cancellation is observed and publish
    // nothing on that path, so no cancelled producer ever became visible state.
    QCOMPARE(cancellationsObserved.load(std::memory_order_acquire), JobKindCount);
    QCOMPARE(gate.appliedCount(), 0);
    gate.auditRetainedEntries();
    QVERIFY2(gate.violations().isEmpty(), qPrintable(gate.violations().join(QStringLiteral("; "))));
}

void RevisionStressTest::sessionCachesNeverServeSupersededRevisions()
{
    pdf::PDFDocument document = buildDocument();
    pdf::PDFDocumentContext context(&document);

    pdf::PDFDocumentSession* session = context.getSession();
    QVERIFY(session != nullptr);
    QVERIFY(session->isValid());

    for (int round = 0; round < 16; ++round)
    {
        const pdf::PDFRevisionIdentity beforeRevision = context.getRevision();
        QVERIFY(session->getRevision() == beforeRevision);

        const pdf::PDFPrecompiledPage* compiled = session->compilePage(size_t(round % PageCount));
        QVERIFY(compiled != nullptr);
        QVERIFY(session->compiledCacheBytes() > 0);

        // A document mutation and a profile change are both invalidation
        // boundaries: nothing compiled before them may be served afterwards.
        if (round % 2 == 0)
        {
            context.markModified(pdf::PDFModifiedDocument::PageContents);
        }
        else
        {
            context.setEffectiveProfileIdentity(QStringLiteral("profile-%1").arg(round));
        }

        const pdf::PDFRevisionIdentity afterRevision = context.getRevision();
        QVERIFY(!(afterRevision == beforeRevision));
        QVERIFY(!context.isCurrent(beforeRevision));
        QVERIFY(context.isCurrent(afterRevision));

        // The session follows the context, and its caches were dropped rather
        // than reconciled against the new revision.
        QVERIFY(session->getRevision() == afterRevision);
        QVERIFY(!session->isCurrent(beforeRevision));
        QCOMPARE(session->compiledCacheBytes(), qsizetype(0));

        const pdf::PDFPrecompiledPage* recompiled = session->compilePage(size_t(round % PageCount));
        QVERIFY(recompiled != nullptr);
        QCOMPARE(session->compilePage(size_t(round % PageCount)), recompiled);
    }
}

QTEST_GUILESS_MAIN(RevisionStressTest)

#include "tst_revisionstresstest.moc"
