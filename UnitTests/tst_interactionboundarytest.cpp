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

// Architecture invariant I21: the host-neutral interaction layer has no Widgets
// or QML dependency.
//
// The strongest assertion in this file is not written in C++ — it is the link
// line in UnitTests/CMakeLists.txt. This target links LoupeLibInteraction,
// LoupeLibCore, Qt6::Core, Qt6::Gui and Qt6::Test, and deliberately not
// Qt6::Widgets. Qt scopes its headers per module, so if a public interaction
// header ever pulls in QtWidgets or QtQuick this translation unit stops
// compiling. QTEST_GUILESS_MAIN then proves the layer also runs without a
// QApplication, which is the P4-S1 exit condition.

#include <QtTest>

#include <atomic>
#include <memory>
#include <thread>

#include "documentcontextsource.h"
#include "jobsubmitter.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"

class InteractionBoundaryTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void sourceReportsBoundContextRevision();
    void markModifiedFencesCapturedRevision();
    void profileIdentityChangeFencesCapturedRevision();
    void invalidateCachesFencesCapturedRevision();
    void revisionChangedIsForwarded();
    void destroyedContextDegradesToInvalidRevision();
    void unboundSourceReportsEmptyDocumentKey();

    void submitterRunsCurrentRevisionWork();
    void submitterDiscardsStaleRevisionBeforeWorkRuns();
    void submitterCancellationIsTerminalAndNotSuccess();
};

void InteractionBoundaryTest::initTestCase()
{
    qRegisterMetaType<pdf::PDFRevisionIdentity>("pdf::PDFRevisionIdentity");
}

void InteractionBoundaryTest::sourceReportsBoundContextRevision()
{
    pdf::PDFDocumentContext context(nullptr);
    pdfinteraction::PDFDocumentContextSource source(&context);

    QVERIFY(source.isBound());
    QCOMPARE(source.context(), &context);
    QCOMPARE(source.currentRevision(), context.getRevision());
    QVERIFY(source.isCurrent(context.getRevision()));
}

void InteractionBoundaryTest::markModifiedFencesCapturedRevision()
{
    pdf::PDFDocumentContext context(nullptr);
    pdfinteraction::PDFDocumentContextSource source(&context);

    const pdf::PDFRevisionIdentity captured = source.currentRevision();
    QVERIFY(source.isCurrent(captured));

    context.markModified();

    // A mutation advances the document revision, so work captured against the
    // previous fence can never be admitted into the current one.
    QVERIFY(!source.isCurrent(captured));
    QVERIFY(source.isCurrent(source.currentRevision()));
    QCOMPARE(source.currentRevision().documentRevision, captured.documentRevision + 1);
}

void InteractionBoundaryTest::profileIdentityChangeFencesCapturedRevision()
{
    pdf::PDFDocumentContext context(nullptr);
    pdfinteraction::PDFDocumentContextSource source(&context);

    const pdf::PDFRevisionIdentity captured = source.currentRevision();
    context.setEffectiveProfileIdentity(QStringLiteral("profile-under-test"));

    const pdf::PDFRevisionIdentity current = source.currentRevision();

    // The PDF bytes did not change, so documentRevision must not move; the
    // profile-dependent caches are still fenced by cacheGeneration.
    QCOMPARE(current.documentRevision, captured.documentRevision);
    QCOMPARE(current.cacheGeneration, captured.cacheGeneration + 1);
    QVERIFY(!source.isCurrent(captured));
}

void InteractionBoundaryTest::invalidateCachesFencesCapturedRevision()
{
    pdf::PDFDocumentContext context(nullptr);
    pdfinteraction::PDFDocumentContextSource source(&context);

    const pdf::PDFRevisionIdentity captured = source.currentRevision();
    context.invalidateCaches();

    const pdf::PDFRevisionIdentity current = source.currentRevision();
    QCOMPARE(current.documentRevision, captured.documentRevision);
    QCOMPARE(current.cacheGeneration, captured.cacheGeneration + 1);
    QVERIFY(!source.isCurrent(captured));
}

void InteractionBoundaryTest::revisionChangedIsForwarded()
{
    pdf::PDFDocumentContext context(nullptr);
    pdfinteraction::PDFDocumentContextSource source(&context);

    QSignalSpy spy(&source, &pdfinteraction::PDFDocumentContextSource::revisionChanged);
    QVERIFY(spy.isValid());

    context.markModified();

    QCOMPARE(spy.count(), 1);
    const QList<QVariant> arguments = spy.takeFirst();
    const auto previous = arguments.at(0).value<pdf::PDFRevisionIdentity>();
    const auto current = arguments.at(1).value<pdf::PDFRevisionIdentity>();
    QCOMPARE(current.documentRevision, previous.documentRevision + 1);
    QCOMPARE(current, source.currentRevision());
}

void InteractionBoundaryTest::destroyedContextDegradesToInvalidRevision()
{
    auto context = std::make_unique<pdf::PDFDocumentContext>(nullptr);
    pdfinteraction::PDFDocumentContextSource source(context.get());

    const pdf::PDFRevisionIdentity captured = source.currentRevision();
    context.reset();

    // A late completion tested against a source whose context is gone must be
    // rejected, not admitted, and must not read through a dangling pointer.
    QVERIFY(!source.isBound());
    QCOMPARE(source.context(), nullptr);
    QVERIFY(!source.currentRevision().isValid());
    QVERIFY(!source.isCurrent(captured));
}

void InteractionBoundaryTest::unboundSourceReportsEmptyDocumentKey()
{
    pdfinteraction::PDFDocumentContextSource source(nullptr);

    // PDFJobScheduler treats an empty document key as "never stale", so an empty
    // key must be visible to callers rather than silently disabling the fence.
    QVERIFY(source.documentKey().isEmpty());
    QVERIFY(!source.isBound());
    QVERIFY(!source.isCurrent(pdf::PDFRevisionIdentity()));
}

void InteractionBoundaryTest::submitterRunsCurrentRevisionWork()
{
    pdf::PDFJobScheduler scheduler(1);
    pdfinteraction::PDFJobSchedulerSubmitter submitter(scheduler);

    pdf::PDFDocumentContext context(nullptr);
    const QString documentKey = QStringLiteral("doc-under-test");
    submitter.publishCurrentRevision(documentKey, context.getRevision());

    std::atomic_bool executed = false;
    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Rendering;
    spec.priority = pdf::PDFJobPriority::VisiblePage;
    spec.documentKey = documentKey;
    spec.documentRevision = context.getRevision().toString();
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    const QString jobId = submitter.submit(spec, [&executed](pdf::PDFJobContext&)
                                           { executed.store(true); });

    QVERIFY(scheduler.waitForFinished(jobId, 5000));
    QVERIFY(executed.load());
    QCOMPARE(submitter.snapshot(jobId).status, pdf::PDFJobStatus::Succeeded);
}

void InteractionBoundaryTest::submitterDiscardsStaleRevisionBeforeWorkRuns()
{
    pdf::PDFJobScheduler scheduler(1);
    pdfinteraction::PDFJobSchedulerSubmitter submitter(scheduler);

    pdf::PDFDocumentContext context(nullptr);
    const QString documentKey = QStringLiteral("doc-under-test");
    const pdf::PDFRevisionIdentity superseded = context.getRevision();

    context.markModified();
    submitter.publishCurrentRevision(documentKey, context.getRevision());

    std::atomic_bool executed = false;
    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Rendering;
    spec.priority = pdf::PDFJobPriority::VisiblePage;
    spec.documentKey = documentKey;
    spec.documentRevision = superseded.toString();
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    const QString jobId = submitter.submit(spec, [&executed](pdf::PDFJobContext&)
                                           { executed.store(true); });

    QVERIFY(scheduler.waitForFinished(jobId, 5000));

    // Stale is terminal and the work never runs: a superseded revision must not
    // burn worker capacity, and must not produce a result that could be admitted.
    QCOMPARE(submitter.snapshot(jobId).status, pdf::PDFJobStatus::Stale);
    QVERIFY(!executed.load());
}

void InteractionBoundaryTest::submitterCancellationIsTerminalAndNotSuccess()
{
    pdf::PDFJobScheduler scheduler(1);
    pdfinteraction::PDFJobSchedulerSubmitter submitter(scheduler);

    std::atomic_bool started = false;
    pdf::PDFJobSpec spec;
    spec.jobId = QStringLiteral("interaction-cancel-me");
    spec.kind = pdf::PDFJobKind::Rendering;
    spec.priority = pdf::PDFJobPriority::Interaction;
    spec.documentKey = QStringLiteral("doc-under-test");

    // Cancel a running job cooperatively, as UnitTestsJobScheduler does. The work
    // exits on the cancellation token rather than on a flag this slot must live
    // long enough to set, so an assertion failure here cannot strand a worker.
    const QString jobId = submitter.submit(spec, [&started](pdf::PDFJobContext& context)
                                           {
        started.store(true);
        while (!context.isCancellationRequested())
        {
            std::this_thread::yield();
        } });

    QTRY_VERIFY_WITH_TIMEOUT(started.load(), 2000);
    QVERIFY(submitter.cancel(jobId));
    QVERIFY(scheduler.waitForFinished(jobId, 5000));

    const pdf::PDFJobSnapshot snapshot = submitter.snapshot(jobId);
    QCOMPARE(snapshot.status, pdf::PDFJobStatus::Cancelled);
    QVERIFY(snapshot.status != pdf::PDFJobStatus::Succeeded);

    // Terminal means terminal: a second cancel is refused rather than re-running
    // the transition.
    QVERIFY(!submitter.cancel(jobId));
}

QTEST_GUILESS_MAIN(InteractionBoundaryTest)

#include "tst_interactionboundarytest.moc"
