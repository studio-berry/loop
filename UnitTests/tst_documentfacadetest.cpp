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

// Architecture invariant I22: one document lifecycle facade and one command
// catalog; no second action registry.
//
// As in tst_interactionboundarytest.cpp, the strongest assertion here is the
// link line in UnitTests/CMakeLists.txt. This target links LoupeLibInteraction,
// LoupeLibCore, Qt6::Core, Qt6::Gui and Qt6::Test, and deliberately not
// Qt6::Widgets. QTEST_GUILESS_MAIN then proves the P4-S2 exit condition: the
// full document lifecycle and the command path are drivable without a QWidget
// and without a QML engine.

#include <QtTest>

#include <QFileInfo>
#include <QTemporaryDir>

#include <atomic>
#include <memory>

#include "commandcatalog.h"
#include "documentfacade.h"
#include "documentloader.h"
#include "jobsubmitter.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentwriter.h"
#include "pdfjobscheduler.h"

namespace
{

pdf::PDFDocumentPointer buildDocument(int pageCount = 1)
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 100, 100));
    }

    return pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build()));
}

/// A submitter that keeps the scheduler's contract but lets a test decide when
/// work runs. It is deliberately not a second scheduler: it queues nothing, has
/// no worker threads, and no priority scheme of its own.
class FakeJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        const QString jobId =
            spec.jobId.isEmpty() ? QStringLiteral("job-%1").arg(++m_sequence) : spec.jobId;

        submittedSpecs.append(spec);
        m_status.insert(jobId, pdf::PDFJobStatus::Queued);

        if (runInline)
        {
            runJob(jobId, std::move(work));
        }
        else
        {
            m_deferred.insert(jobId, std::move(work));
        }

        return jobId;
    }

    bool cancel(const QString& jobId) override
    {
        cancelledJobs.append(jobId);

        if (m_status.value(jobId, pdf::PDFJobStatus::Succeeded) != pdf::PDFJobStatus::Queued)
        {
            return false;
        }

        if (!cancelStopsQueuedWork)
        {
            // Stands in for a job the scheduler had already started: cancellation
            // is requested, but the work still runs to its own terminal state.
            return false;
        }

        // Matches pdf::PDFJobScheduler: a job cancelled while still queued never
        // runs its work.
        m_status.insert(jobId, pdf::PDFJobStatus::Cancelled);
        m_deferred.remove(jobId);
        return true;
    }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = m_status.value(jobId, pdf::PDFJobStatus::Succeeded);
        return result;
    }

    void publishCurrentRevision(const QString& documentKey,
                                const pdf::PDFRevisionIdentity& revision) override
    {
        publishedRevisions.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override
    {
        publishedRevisions.remove(documentKey);
        clearedKeys.append(documentKey);
    }

    /// Runs a job that was held back, as a worker thread eventually would.
    bool runDeferred(const QString& jobId, bool cancelBeforeRun = false)
    {
        const auto it = m_deferred.find(jobId);
        if (it == m_deferred.end())
        {
            return false;
        }

        pdf::PDFJobWork work = *it;
        m_deferred.erase(it);
        runJob(jobId, std::move(work), cancelBeforeRun);
        return true;
    }

    int deferredJobCount() const { return int(m_deferred.size()); }

    bool runInline = true;
    bool cancelStopsQueuedWork = true;
    QList<pdf::PDFJobSpec> submittedSpecs;
    QStringList cancelledJobs;
    QStringList clearedKeys;
    QHash<QString, pdf::PDFRevisionIdentity> publishedRevisions;

private:
    void runJob(const QString& jobId, pdf::PDFJobWork work, bool cancelBeforeRun = false)
    {
        m_status.insert(jobId, pdf::PDFJobStatus::Running);

        auto token = std::make_shared<pdf::PDFJobCancellationToken>();
        if (cancelBeforeRun)
        {
            token->cancel();
        }

        pdf::PDFJobContext context(token,
                                   pdf::PDFProcessingLimits::conservativeDefaults(),
                                   [](int) {});
        work(context);
        m_status.insert(jobId, pdf::PDFJobStatus::Succeeded);
    }

    quint64 m_sequence = 0;
    QHash<QString, pdf::PDFJobStatus> m_status;
    QHash<QString, pdf::PDFJobWork> m_deferred;
};

class FakeDocumentLoader final : public pdfinteraction::IDocumentLoader
{
public:
    pdfinteraction::DocumentLoadResult load(const pdfinteraction::DocumentSource& source,
                                            pdf::PDFJobContext& context) override
    {
        ++loadCount;
        requestedPaths.append(source.path);

        if (context.isCancellationRequested())
        {
            pdfinteraction::DocumentLoadResult cancelled;
            cancelled.outcome = pdfinteraction::DocumentLoadOutcome::Cancelled;
            cancelled.typedError = QStringLiteral("document/cancelled");
            return cancelled;
        }

        pdfinteraction::DocumentLoadResult result = nextResult;
        if (result.outcome == pdfinteraction::DocumentLoadOutcome::Loaded && !result.document)
        {
            result.document = buildDocument();
        }
        return result;
    }

    pdfinteraction::DocumentLoadResult nextResult;
    int loadCount = 0;
    QStringList requestedPaths;
};

class FakeDocumentWriter final : public pdfinteraction::IDocumentWriter
{
public:
    pdfinteraction::DocumentWriteResult write(const pdfinteraction::DocumentSource& target,
                                              const pdf::PDFDocument* document,
                                              pdf::PDFJobContext& context) override
    {
        ++writeCount;
        writtenPaths.append(target.path);
        sawDocument = document != nullptr;

        if (context.isCancellationRequested())
        {
            pdfinteraction::DocumentWriteResult cancelled;
            cancelled.outcome = pdfinteraction::DocumentWriteOutcome::Cancelled;
            cancelled.typedError = QStringLiteral("document/cancelled");
            return cancelled;
        }

        return nextResult;
    }

    pdfinteraction::DocumentWriteResult nextResult{ pdfinteraction::DocumentWriteOutcome::Written,
                                                    QString() };
    int writeCount = 0;
    bool sawDocument = false;
    QStringList writtenPaths;
};

/// Everything one lifecycle test needs, wired the way a host would wire it.
struct Harness
{
    Harness()
    {
        loader.nextResult.outcome = pdfinteraction::DocumentLoadOutcome::Loaded;
        facade = std::make_unique<pdfinteraction::DocumentFacade>(context, submitter, loader,
                                                                  writer, catalog);
    }

    pdf::PDFDocumentContext context{ nullptr };
    FakeJobSubmitter submitter;
    FakeDocumentLoader loader;
    FakeDocumentWriter writer;
    pdfinteraction::CommandCatalog catalog;
    std::unique_ptr<pdfinteraction::DocumentFacade> facade;
};

}   // namespace

class DocumentFacadeTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void catalogLoadsTheWholeEditorActionSet();
    void catalogRejectsAnUnknownCommand();
    void declaredCommandIsNotImplementedAndRunsNothing();
    void catalogRefusesAHandlerForADeclaredCommand();
    void missingRequiredParameterIsRejected();
    void unknownParameterIsRejected();
    void disabledCommandIsUnavailable();

    void openAdmitsDocumentAndPublishesRevision();
    void openFailureReportsTypedErrorAndBindsNoDocument();
    void openCancellationIsTerminalAndNotSuccess();
    void cancellingAQueuedOpenIsTerminal();

    void replacementEstablishesNewIdentityBeforeNewWork();
    void supersededCompletionIsRejectedNotAdmitted();
    void replacementInvalidatesTheAvailabilitySnapshot();

    void closeWithJobInFlightCancelsAndClearsRevision();
    void closeAfterOpenClearsThePublishedRevision();
    void destroyingTheFacadeTerminatesAPendingInvocation();

    void saveWritesAndReportsOutputSaved();
    void saveFailureKeepsTheDocumentReady();
    void markModifiedFencesTheCapturedRevision();

    void shellStatusProjectionIsPinned();

    void realDocumentRoundTripsThroughCoreReaderAndWriter();
};

void DocumentFacadeTest::initTestCase()
{
    qRegisterMetaType<pdf::PDFRevisionIdentity>("pdf::PDFRevisionIdentity");
    qRegisterMetaType<pdfinteraction::CommandResult>("pdfinteraction::CommandResult");
    qRegisterMetaType<pdfinteraction::DocumentState>("pdfinteraction::DocumentState");
}

void DocumentFacadeTest::catalogLoadsTheWholeEditorActionSet()
{
    pdfinteraction::CommandCatalog catalog;

    // The embedded contract is docs/loupe-shell-actions.json itself. A static
    // library that lost its resource would present an empty command set as a
    // working one, so this also pins the resource wiring.
    QVERIFY2(catalog.isLoaded(), qPrintable(catalog.loadError()));
    QCOMPARE(catalog.descriptors().size(), 107);

    const pdfinteraction::CommandDescriptor* open =
        catalog.descriptor(pdfinteraction::DocumentFacade::OpenCommandId);
    QVERIFY(open != nullptr);
    QCOMPARE(open->labelKey, QStringLiteral("command.actionOpen.label"));
    QVERIFY(open->isImplemented());
    QVERIFY(open->cancellable);
    QCOMPARE(open->parameters.size(), 1);
    QCOMPARE(open->parameters.first().name, QStringLiteral("path"));
    QVERIFY(open->parameters.first().required);
    QCOMPARE(open->capability, pdfinteraction::CommandCapability::DocumentRead);
    QCOMPARE(open->target, QStringLiteral("Document"));

    // Routing metadata rides along, so a workspace rail needs no second table.
    const pdfinteraction::CommandDescriptor* bleedFixup =
        catalog.descriptor(QStringLiteral("actionBleedFixup"));
    QVERIFY(bleedFixup != nullptr);
    QCOMPARE(bleedFixup->disposition, QStringLiteral("ABSORB"));
    QCOMPARE(bleedFixup->target, QStringLiteral("Production"));
    QCOMPARE(bleedFixup->shortcut.sequence, QStringLiteral("Ctrl+Shift+B"));

    int implemented = 0;
    for (const pdfinteraction::CommandDescriptor& descriptor : catalog.descriptors())
    {
        if (descriptor.isImplemented())
        {
            ++implemented;
            // An implemented command must have been classified.
            QVERIFY(descriptor.capability != pdfinteraction::CommandCapability::Unclassified);
        }
    }
    QCOMPARE(implemented, 4);
}

void DocumentFacadeTest::catalogRejectsAnUnknownCommand()
{
    Harness harness;
    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    QVERIFY(finished.isValid());

    const pdfinteraction::CommandInvocationId invocation =
        harness.catalog.invoke(QStringLiteral("actionDefinitelyNotAThing"));

    // Not a silent no-op: the contract is the whole ID space, so an unknown ID
    // is reported as a routing bug.
    QCOMPARE(invocation, pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(finished.count(), 1);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Unavailable);
    QCOMPARE(result.typedError, QStringLiteral("command/unknown"));
    QVERIFY(!result.isSuccess());
    QVERIFY(harness.catalog.descriptor(QStringLiteral("actionDefinitelyNotAThing")) == nullptr);
}

void DocumentFacadeTest::declaredCommandIsNotImplementedAndRunsNothing()
{
    Harness harness;
    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);

    const pdfinteraction::CommandInvocationId invocation =
        harness.catalog.invoke(QStringLiteral("actionSanitize"));

    QCOMPARE(invocation, pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(finished.count(), 1);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::NotImplemented);
    QCOMPARE(result.typedError, QStringLiteral("command/not-implemented"));

    // Declared means "has a descriptor", never "quietly did something".
    QCOMPARE(harness.submitter.submittedSpecs.size(), 0);
    QCOMPARE(harness.loader.loadCount, 0);
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Empty);
}

void DocumentFacadeTest::catalogRefusesAHandlerForADeclaredCommand()
{
    pdfinteraction::CommandCatalog catalog;

    pdfinteraction::CommandCatalog::Handler handler;
    handler.invoke = [](pdfinteraction::CommandInvocationId, const QVariantMap&) {};

    QVERIFY(!catalog.setHandler(QStringLiteral("actionSanitize"), handler));
    QVERIFY(!catalog.setHandler(QStringLiteral("actionNotInTheContract"), handler));
    QVERIFY(catalog.setHandler(pdfinteraction::DocumentFacade::OpenCommandId, handler));
}

void DocumentFacadeTest::missingRequiredParameterIsRejected()
{
    Harness harness;
    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);

    const pdfinteraction::CommandInvocationId invocation =
        harness.catalog.invoke(pdfinteraction::DocumentFacade::OpenCommandId);

    QCOMPARE(invocation, pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(finished.count(), 1);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Failed);
    QCOMPARE(result.typedError, QStringLiteral("command/missing-parameter"));
    QCOMPARE(harness.loader.loadCount, 0);
}

void DocumentFacadeTest::unknownParameterIsRejected()
{
    Harness harness;
    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);

    QVariantMap parameters;
    parameters.insert(QStringLiteral("path"), QStringLiteral("/tmp/a.pdf"));
    parameters.insert(QStringLiteral("pathh"), QStringLiteral("/tmp/b.pdf"));

    // A misspelled parameter must not run the command with a default the caller
    // never asked for.
    QCOMPARE(harness.catalog.invoke(pdfinteraction::DocumentFacade::OpenCommandId, parameters),
             pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(finished.count(), 1);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.typedError, QStringLiteral("command/unknown-parameter"));
    QCOMPARE(harness.loader.loadCount, 0);
}

void DocumentFacadeTest::disabledCommandIsUnavailable()
{
    Harness harness;

    // Nothing is open, so closing and saving are not offered.
    QVERIFY(harness.catalog.isEnabled(pdfinteraction::DocumentFacade::OpenCommandId));
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::DocumentFacade::CloseCommandId));
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::DocumentFacade::SaveCommandId));

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    QCOMPARE(harness.facade->close(), pdfinteraction::InvalidCommandInvocation);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Unavailable);
    QCOMPARE(result.typedError, QStringLiteral("command/unavailable"));
}

void DocumentFacadeTest::openAdmitsDocumentAndPublishesRevision()
{
    Harness harness;
    QSignalSpy stateChanged(harness.facade.get(), &pdfinteraction::DocumentFacade::stateChanged);
    QSignalSpy replaced(harness.facade.get(), &pdfinteraction::DocumentFacade::documentReplaced);
    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);

    const pdfinteraction::CommandInvocationId invocation =
        harness.facade->open(QStringLiteral("/corpus/report.pdf"));
    QVERIFY(invocation != pdfinteraction::InvalidCommandInvocation);

    // Admission is queued, never delivered while the submitting call is still on
    // the stack.
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Opening);
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    QCOMPARE(harness.loader.loadCount, 1);
    QCOMPARE(harness.loader.requestedPaths.first(), QStringLiteral("/corpus/report.pdf"));
    QCOMPARE(harness.facade->source().displayLabel(), QStringLiteral("report.pdf"));
    QVERIFY(harness.context.getDocument() != nullptr);
    QCOMPARE(harness.facade->shellDocumentStatus(), pdfinteraction::ShellDocumentStatus::Open);
    QCOMPARE(harness.facade->rejectedCompletionCount(), 0);

    // The one revision fence is published under the one document key.
    const QString documentKey = harness.context.getDocumentIdentity().documentId;
    QVERIFY(!documentKey.isEmpty());
    QCOMPARE(harness.submitter.publishedRevisions.value(documentKey), harness.context.getRevision());

    // The load job carries no document key: there is no revision to fence it
    // against before the document exists.
    QCOMPARE(harness.submitter.submittedSpecs.size(), 1);
    QVERIFY(harness.submitter.submittedSpecs.first().documentKey.isEmpty());
    QCOMPARE(harness.submitter.submittedSpecs.first().priority, pdf::PDFJobPriority::Operator);
    QCOMPARE(harness.submitter.submittedSpecs.first().staleResultPolicy,
             pdf::PDFJobStaleResultPolicy::Discard);

    QCOMPARE(replaced.count(), 1);
    QVERIFY(stateChanged.count() >= 2);
    QCOMPARE(finished.count(), 1);
    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.invocation, invocation);
    QVERIFY(result.isSuccess());

    QVERIFY(harness.catalog.isEnabled(pdfinteraction::DocumentFacade::CloseCommandId));
    QVERIFY(harness.catalog.isEnabled(pdfinteraction::DocumentFacade::SaveCommandId));
}

void DocumentFacadeTest::openFailureReportsTypedErrorAndBindsNoDocument()
{
    Harness harness;
    harness.loader.nextResult.outcome = pdfinteraction::DocumentLoadOutcome::Failed;
    harness.loader.nextResult.typedError = QStringLiteral("document/read-failed");

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    harness.facade->open(QStringLiteral("/corpus/broken.pdf"));

    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Error);
    QCOMPARE(harness.facade->typedError(), QStringLiteral("document/read-failed"));
    QCOMPARE(harness.context.getDocument(), nullptr);
    QCOMPARE(harness.facade->shellDocumentStatus(), pdfinteraction::ShellDocumentStatus::NoDocument);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Failed);
    QVERIFY(!result.isSuccess());

    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::DocumentFacade::CloseCommandId));
    QVERIFY(harness.catalog.isEnabled(pdfinteraction::DocumentFacade::OpenCommandId));
}

void DocumentFacadeTest::openCancellationIsTerminalAndNotSuccess()
{
    Harness harness;
    harness.loader.nextResult.outcome = pdfinteraction::DocumentLoadOutcome::Cancelled;
    harness.loader.nextResult.typedError = QStringLiteral("document/cancelled");

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    harness.facade->open(QStringLiteral("/corpus/report.pdf"));

    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Empty);
    QVERIFY(harness.facade->facets().testFlag(pdfinteraction::DocumentFacet::Cancelled));
    QCOMPARE(harness.context.getDocument(), nullptr);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Cancelled);

    // Cancellation is terminal and it is not success.
    QVERIFY(!result.isSuccess());
    QVERIFY(!harness.catalog.isPending(result.invocation));
}

void DocumentFacadeTest::cancellingAQueuedOpenIsTerminal()
{
    Harness harness;
    harness.submitter.runInline = false;

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    const pdfinteraction::CommandInvocationId invocation =
        harness.facade->open(QStringLiteral("/corpus/report.pdf"));

    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Opening);
    QCOMPARE(harness.submitter.deferredJobCount(), 1);

    QVERIFY(harness.facade->cancelPendingOperation());

    // Work cancelled out of the queue never runs, so nothing would report a
    // terminal state for it. The facade must resolve that itself rather than
    // leaving the invocation pending forever.
    QTRY_VERIFY(!harness.catalog.isPending(invocation));
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Empty);
    QCOMPARE(harness.loader.loadCount, 0);
    QVERIFY(harness.facade->facets().testFlag(pdfinteraction::DocumentFacet::Cancelled));

    QCOMPARE(finished.count(), 1);
    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Cancelled);
    QCOMPARE(harness.facade->rejectedCompletionCount(), 0);
}

void DocumentFacadeTest::replacementEstablishesNewIdentityBeforeNewWork()
{
    Harness harness;
    harness.facade->open(QStringLiteral("/corpus/first.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    const pdf::PDFRevisionIdentity firstRevision = harness.facade->currentRevision();
    const QString firstKey = harness.context.getDocumentIdentity().documentId;
    const quint64 firstGeneration = harness.facade->documentGeneration();
    QVERIFY(harness.facade->revisionSource().isCurrent(firstRevision));

    harness.facade->open(QStringLiteral("/corpus/second.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    // The prior identity is dropped before new work is admitted, so nothing
    // captured against it can be presented as current.
    QVERIFY(!harness.facade->revisionSource().isCurrent(firstRevision));
    QVERIFY(harness.facade->documentGeneration() > firstGeneration);
    QVERIFY(harness.submitter.clearedKeys.contains(firstKey));
    QCOMPARE(harness.loader.loadCount, 2);
    QCOMPARE(harness.facade->source().displayLabel(), QStringLiteral("second.pdf"));
    QCOMPARE(harness.facade->rejectedCompletionCount(), 0);
}

void DocumentFacadeTest::supersededCompletionIsRejectedNotAdmitted()
{
    Harness harness;
    harness.submitter.runInline = false;

    // The first read is already running when it is superseded, so cancelling it
    // does not stop it: its completion still arrives, and admission is the only
    // thing standing between it and the current document.
    harness.submitter.cancelStopsQueuedWork = false;

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);

    const pdfinteraction::CommandInvocationId first =
        harness.facade->open(QStringLiteral("/corpus/first.pdf"));
    const QString firstJob = QStringLiteral("job-1");
    QCOMPARE(harness.submitter.deferredJobCount(), 1);

    harness.submitter.runInline = true;
    harness.facade->open(QStringLiteral("/corpus/second.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);
    QCOMPARE(harness.facade->source().displayLabel(), QStringLiteral("second.pdf"));

    const pdf::PDFRevisionIdentity currentRevision = harness.facade->currentRevision();
    QVERIFY(!harness.catalog.isPending(first));
    QVERIFY(harness.submitter.cancelledJobs.contains(firstJob));

    // Now the superseded read finishes. It must be counted and dropped, never
    // patched into the document that replaced it.
    QVERIFY(harness.submitter.runDeferred(firstJob));
    QTRY_COMPARE(harness.facade->rejectedCompletionCount(), 1);

    QCOMPARE(harness.facade->currentRevision(), currentRevision);
    QCOMPARE(harness.facade->source().displayLabel(), QStringLiteral("second.pdf"));
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    bool sawSupersededTerminal = false;
    int terminalsForFirst = 0;
    for (const QList<QVariant>& emitted : finished)
    {
        const auto result = emitted.at(0).value<pdfinteraction::CommandResult>();
        if (result.invocation == first)
        {
            ++terminalsForFirst;
            sawSupersededTerminal = true;
            QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Cancelled);
            QCOMPARE(result.typedError, QStringLiteral("document/superseded"));
        }
    }
    QVERIFY(sawSupersededTerminal);

    // Terminal means terminal: the late completion does not emit a second one.
    QCOMPARE(terminalsForFirst, 1);
}

void DocumentFacadeTest::replacementInvalidatesTheAvailabilitySnapshot()
{
    Harness harness;
    harness.facade->open(QStringLiteral("/corpus/first.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);
    QVERIFY(harness.catalog.isEnabled(pdfinteraction::DocumentFacade::SaveCommandId));

    harness.submitter.runInline = false;
    harness.facade->open(QStringLiteral("/corpus/second.pdf"));

    // Availability computed against the previous document is not evidence about
    // the next one, so it does not survive the replacement.
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Opening);
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::DocumentFacade::SaveCommandId));
    QVERIFY(harness.catalog.isEnabled(pdfinteraction::DocumentFacade::CloseCommandId));
}

void DocumentFacadeTest::closeWithJobInFlightCancelsAndClearsRevision()
{
    Harness harness;
    harness.facade->open(QStringLiteral("/corpus/report.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    const QString documentKey = harness.context.getDocumentIdentity().documentId;
    const pdf::PDFRevisionIdentity captured = harness.facade->currentRevision();

    // A save is left in flight, then the document is closed under it.
    harness.submitter.runInline = false;
    const pdfinteraction::CommandInvocationId save =
        harness.facade->saveAs(QStringLiteral("/out/report.pdf"));
    QVERIFY(save != pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.facade->shellDocumentStatus(),
             pdfinteraction::ShellDocumentStatus::OutputPending);

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    harness.facade->close();

    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Empty);
    QVERIFY(!harness.catalog.isPending(save));
    QVERIFY(harness.submitter.cancelledJobs.contains(QStringLiteral("job-2")));

    // The fence entry this facade created is the one it clears, and a key with
    // no entry is never stale.
    QVERIFY(harness.submitter.clearedKeys.contains(documentKey));
    QVERIFY(!harness.submitter.publishedRevisions.contains(documentKey));
    QVERIFY(!harness.facade->revisionSource().isCurrent(captured));

    bool sawSaveCancelled = false;
    for (const QList<QVariant>& emitted : finished)
    {
        const auto result = emitted.at(0).value<pdfinteraction::CommandResult>();
        if (result.invocation == save)
        {
            sawSaveCancelled = true;
            QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Cancelled);
            QVERIFY(!result.isSuccess());
        }
    }
    QVERIFY(sawSaveCancelled);

    // A late completion after close must not reach a document that is gone.
    QCOMPARE(harness.writer.writeCount, 0);
    QCOMPARE(harness.facade->shellDocumentStatus(), pdfinteraction::ShellDocumentStatus::NoDocument);
}

void DocumentFacadeTest::closeAfterOpenClearsThePublishedRevision()
{
    Harness harness;
    harness.facade->open(QStringLiteral("/corpus/report.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    const QString documentKey = harness.context.getDocumentIdentity().documentId;
    QSignalSpy closed(harness.facade.get(), &pdfinteraction::DocumentFacade::documentClosed);

    const pdfinteraction::CommandInvocationId invocation = harness.facade->close();
    QVERIFY(invocation != pdfinteraction::InvalidCommandInvocation);

    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Empty);
    QCOMPARE(harness.context.getDocument(), nullptr);
    QVERIFY(!harness.facade->source().isValid());
    QCOMPARE(closed.count(), 1);
    QVERIFY(harness.submitter.clearedKeys.contains(documentKey));
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::DocumentFacade::CloseCommandId));
}

void DocumentFacadeTest::destroyingTheFacadeTerminatesAPendingInvocation()
{
    Harness harness;
    harness.submitter.runInline = false;

    // The read is already running, so the facade's destructor cannot stop it —
    // which is the case worth proving.
    harness.submitter.cancelStopsQueuedWork = false;

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    const pdfinteraction::CommandInvocationId invocation =
        harness.facade->open(QStringLiteral("/corpus/report.pdf"));
    QVERIFY(harness.catalog.isPending(invocation));

    harness.facade.reset();

    // No invocation is left dangling, and the relay makes a late completion
    // harmless rather than a use-after-free.
    QVERIFY(!harness.catalog.isPending(invocation));
    QCOMPARE(finished.count(), 1);
    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Cancelled);
    QCOMPARE(result.typedError, QStringLiteral("document/facade-destroyed"));

    QVERIFY(harness.submitter.runDeferred(QStringLiteral("job-1")));
    QTest::qWait(20);
}

void DocumentFacadeTest::saveWritesAndReportsOutputSaved()
{
    Harness harness;
    harness.facade->open(QStringLiteral("/corpus/report.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    const QString documentKey = harness.context.getDocumentIdentity().documentId;
    const pdf::PDFRevisionIdentity revision = harness.facade->currentRevision();

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    const pdfinteraction::CommandInvocationId invocation = harness.facade->save();
    QVERIFY(invocation != pdfinteraction::InvalidCommandInvocation);

    QTRY_COMPARE(harness.facade->shellDocumentStatus(),
                 pdfinteraction::ShellDocumentStatus::OutputSaved);
    QCOMPARE(harness.writer.writeCount, 1);
    QVERIFY(harness.writer.sawDocument);
    QCOMPARE(harness.writer.writtenPaths.first(), QStringLiteral("/corpus/report.pdf"));
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    // The write job is fenced by the scheduler's own revision contract.
    const pdf::PDFJobSpec saveSpec = harness.submitter.submittedSpecs.last();
    QCOMPARE(saveSpec.documentKey, documentKey);
    QCOMPARE(saveSpec.documentRevision, revision.toString());
    QCOMPARE(saveSpec.staleResultPolicy, pdf::PDFJobStaleResultPolicy::Discard);
    QCOMPARE(saveSpec.kind, pdf::PDFJobKind::Export);

    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QVERIFY(result.isSuccess());
}

void DocumentFacadeTest::saveFailureKeepsTheDocumentReady()
{
    Harness harness;
    harness.facade->open(QStringLiteral("/corpus/report.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    harness.writer.nextResult.outcome = pdfinteraction::DocumentWriteOutcome::Failed;
    harness.writer.nextResult.typedError = QStringLiteral("document/write-failed");

    QSignalSpy finished(&harness.catalog, &pdfinteraction::CommandCatalog::invocationFinished);
    harness.facade->save();

    QTRY_COMPARE(finished.count(), 1);
    const auto result = finished.takeFirst().at(0).value<pdfinteraction::CommandResult>();
    QCOMPARE(result.state, pdfinteraction::CommandTerminalState::Failed);

    // Only the output axis failed; the document is untouched and still open.
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);
    QCOMPARE(harness.facade->outputState(), pdfinteraction::DocumentOutputState::None);
    QCOMPARE(harness.facade->shellDocumentStatus(), pdfinteraction::ShellDocumentStatus::Open);
    QCOMPARE(harness.facade->typedError(), QStringLiteral("document/write-failed"));
}

void DocumentFacadeTest::markModifiedFencesTheCapturedRevision()
{
    Harness harness;
    harness.facade->open(QStringLiteral("/corpus/report.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);

    const pdf::PDFRevisionIdentity captured = harness.facade->currentRevision();
    const QString documentKey = harness.context.getDocumentIdentity().documentId;

    harness.facade->markModified();

    QVERIFY(!harness.facade->revisionSource().isCurrent(captured));
    QVERIFY(harness.facade->facets().testFlag(pdfinteraction::DocumentFacet::Dirty));
    QCOMPARE(harness.facade->shellDocumentStatus(), pdfinteraction::ShellDocumentStatus::Modified);

    // The published fence follows the document rather than going stale behind it.
    QCOMPARE(harness.submitter.publishedRevisions.value(documentKey), harness.context.getRevision());

    // A modification after a successful save makes the saved state no longer
    // what the operator is looking at.
    harness.facade->save();
    QTRY_COMPARE(harness.facade->shellDocumentStatus(),
                 pdfinteraction::ShellDocumentStatus::OutputSaved);
    harness.facade->markModified();
    QCOMPARE(harness.facade->shellDocumentStatus(), pdfinteraction::ShellDocumentStatus::Modified);
}

void DocumentFacadeTest::shellStatusProjectionIsPinned()
{
    using pdfinteraction::DocumentFacet;
    using pdfinteraction::DocumentFacets;
    using pdfinteraction::DocumentOutputState;
    using pdfinteraction::DocumentState;
    using pdfinteraction::ShellDocumentStatus;
    using pdfinteraction::DocumentFacade;

    // docs/loupe-shell.json names five document states. The facade's richer
    // model answers in those five terms; this table is the whole projection, so
    // no host has to re-derive it and drift.
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Empty, {}, DocumentOutputState::None),
             ShellDocumentStatus::NoDocument);
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Opening, {}, DocumentOutputState::None),
             ShellDocumentStatus::NoDocument);
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Closing, {}, DocumentOutputState::None),
             ShellDocumentStatus::NoDocument);
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Error, {}, DocumentOutputState::None),
             ShellDocumentStatus::NoDocument);

    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Ready, {}, DocumentOutputState::None),
             ShellDocumentStatus::Open);
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Ready,
                                                DocumentFacets(DocumentFacet::Dirty),
                                                DocumentOutputState::None),
             ShellDocumentStatus::Modified);
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Ready, {}, DocumentOutputState::Pending),
             ShellDocumentStatus::OutputPending);
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Ready, {}, DocumentOutputState::Saved),
             ShellDocumentStatus::OutputSaved);

    // A write in flight outranks unsaved changes; unsaved changes outrank a
    // previous successful write.
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Ready,
                                                DocumentFacets(DocumentFacet::Dirty),
                                                DocumentOutputState::Pending),
             ShellDocumentStatus::OutputPending);
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Ready,
                                                DocumentFacets(DocumentFacet::Dirty),
                                                DocumentOutputState::Saved),
             ShellDocumentStatus::Modified);

    // Non-Dirty facets describe the document, not the shell status.
    QCOMPARE(DocumentFacade::projectShellStatus(DocumentState::Ready,
                                                DocumentFacets(DocumentFacet::Incomplete) |
                                                    DocumentFacet::Stale,
                                                DocumentOutputState::None),
             ShellDocumentStatus::Open);

    QCOMPARE(QLatin1String(pdfinteraction::getShellDocumentStatusName(ShellDocumentStatus::OutputPending)),
             QLatin1String("OUTPUT_PENDING"));
}

void DocumentFacadeTest::realDocumentRoundTripsThroughCoreReaderAndWriter()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString sourcePath = directory.filePath(QStringLiteral("source.pdf"));
    const QString targetPath = directory.filePath(QStringLiteral("target.pdf"));

    {
        const pdf::PDFDocumentPointer document = buildDocument(3);
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(bool(writer.write(sourcePath, document.data(), true)));
    }

    pdf::PDFDocumentContext context(nullptr);
    pdf::PDFJobScheduler scheduler(1);
    pdfinteraction::PDFJobSchedulerSubmitter submitter(scheduler);
    pdfinteraction::PDFReaderDocumentLoader loader;
    pdfinteraction::PDFDocumentFileWriter writer;
    pdfinteraction::CommandCatalog catalog;
    pdfinteraction::DocumentFacade facade(context, submitter, loader, writer, catalog);

    // The whole lifecycle over the real Core reader, writer, and scheduler —
    // with no QApplication, no QWidget, and no QML engine anywhere.
    facade.open(sourcePath);
    QTRY_COMPARE_WITH_TIMEOUT(facade.state(), pdfinteraction::DocumentState::Ready, 10000);
    QVERIFY(context.getDocument() != nullptr);
    QCOMPARE(context.getDocument()->getCatalog()->getPageCount(), size_t(3));

    facade.saveAs(targetPath);
    QTRY_COMPARE_WITH_TIMEOUT(facade.shellDocumentStatus(),
                              pdfinteraction::ShellDocumentStatus::OutputSaved, 10000);
    QVERIFY(QFileInfo::exists(targetPath));

    facade.close();
    QCOMPARE(facade.state(), pdfinteraction::DocumentState::Empty);
    QCOMPARE(context.getDocument(), nullptr);
    QCOMPARE(facade.rejectedCompletionCount(), 0);
}

QTEST_GUILESS_MAIN(DocumentFacadeTest)

#include "tst_documentfacadetest.moc"
