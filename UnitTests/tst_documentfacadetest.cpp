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
// link line in UnitTests/CMakeLists.txt. This target links LoopLibInteraction,
// LoopLibCore, Qt6::Core, Qt6::Gui and Qt6::Test, and deliberately not
// Qt6::Widgets. QTEST_GUILESS_MAIN then proves the P4-S2 exit condition: the
// full document lifecycle and the command path are drivable without a QWidget
// and without a QML engine.

#include <QtTest>

#include <QFileInfo>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>

#include <atomic>
#include <memory>

#include "commandcatalog.h"
#include "documentfacade.h"
#include "documentloader.h"
#include "jobsubmitter.h"

#include "pdfauthoringedit.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"
#include "pdfjobscheduler.h"
#include "pdfsecurityhandler.h"

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
        m_specs.insert(jobId, spec);
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
        result.kind = m_specs.value(jobId).kind;
        result.documentKey = m_specs.value(jobId).documentKey;
        result.documentRevision = m_specs.value(jobId).documentRevision;
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
    pdf::PDFJobStatus terminalStatus = pdf::PDFJobStatus::Succeeded;
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
        m_status.insert(jobId, terminalStatus);
    }

    quint64 m_sequence = 0;
    QHash<QString, pdf::PDFJobStatus> m_status;
    QHash<QString, pdf::PDFJobSpec> m_specs;
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

class DelayedDocumentLoader final : public pdfinteraction::IDocumentLoader
{
public:
    pdfinteraction::DocumentLoadResult load(const pdfinteraction::DocumentSource&,
                                            pdf::PDFJobContext&) override
    {
        started.release();
        release.acquire();
        pdfinteraction::DocumentLoadResult result;
        result.outcome = pdfinteraction::DocumentLoadOutcome::Loaded;
        result.document = buildDocument();
        return result;
    }

    QSemaphore started;
    QSemaphore release;
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

struct InspectionFixture
{
    pdf::PreflightResult result;
    pdf::PreflightProfileData profile;
    pdf::PDFEvidenceGraph evidence;

    InspectionFixture()
    {
        result.documentRevisionDigest = QString(64, QLatin1Char('a'));
        result.effectiveProfileDigest = QString(64, QLatin1Char('b'));
        result.coverageScope = QJsonObject{
            { QStringLiteral("claim"), QStringLiteral("Enabled checks only.") },
            { QStringLiteral("enabled_checks"), QJsonArray{ QStringLiteral("bleed") } }
        };
        pdf::PreflightCheckStatus status;
        status.id = QStringLiteral("bleed");
        status.status = QStringLiteral("ok");
        result.checkStatuses.append(status);
        profile.effectiveDigest = result.effectiveProfileDigest;
        profile.coverageScope = result.coverageScope;
        pdf::PreflightCheckConfig check;
        check.id = status.id;
        check.enabled = true;
        check.required = true;
        profile.checks.append(check);
        pdf::PDFEvidenceRecord record;
        record.id = QStringLiteral("facade-inspection-evidence");
        record.fidelity = QStringLiteral("exact");
        record.artifact.sha256 = result.documentRevisionDigest;
        evidence.records.append(record);
    }

    QString addFinding()
    {
        pdf::PreflightFinding finding;
        finding.checkId = QStringLiteral("bleed");
        finding.type = QStringLiteral("bleed-missing");
        finding.scope = QStringLiteral("page");
        finding.page = 1;
        finding.severity = QStringLiteral("error");
        finding.message = QStringLiteral("Bleed is missing.");
        result.errors.append(finding);
        return finding.stableId();
    }
};

}   // namespace

class DocumentFacadeTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void catalogLoadsTheWholeEditorActionSet();
    void catalogPublishesAvailabilityAtomically();
    void catalogRejectsAnUnknownCommand();
    void declaredCommandIsNotImplementedAndRunsNothing();
    void catalogRefusesAHandlerForADeclaredCommand();
    void missingRequiredParameterIsRejected();
    void unknownParameterIsRejected();
    void disabledCommandIsUnavailable();

    void openAdmitsDocumentAndPublishesRevision();
    void workerLoadCannotPublishAfterSchedulerFailure();
    void openFailureReportsTypedErrorAndBindsNoDocument();
    void openCancellationIsTerminalAndNotSuccess();
    void cancellingAQueuedOpenIsTerminal();
    void cancellationRejectsAlreadyQueuedSuccess();
    void failedOpenRetryPreservesCauseAndAllocatesIdentity();
    void cancelledRetryRejectsReorderedCompletion_data();
    void cancelledRetryRejectsReorderedCompletion();
    void failedSaveRetryKeepsTargetAndRevision();
    void cancelledSaveRetryRejectsPriorSuccess();
    void retryRejectsStaleRevisionAndClosedDocument();
    void supersessionCannotRetryWhileReplacingDocument();
    void reopenRejectsPriorWriteCompletion();
    void realWorkerCancellationAndTeardown_data();
    void realWorkerCancellationAndTeardown();

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
    void operatorSnapshotTracksLifecycle();
    void inspectionVerdictContract_data();
    void inspectionVerdictContract();
    void inspectionProgressAndTerminalAdmission();
    void inspectionCancellationAndFailure();
    void inspectionSelectionAndPlanIntent();
    void inspectionRejectsInvalidEvidenceWithoutMutation();
    void inspectionInvalidation_data();
    void inspectionInvalidation();
    void inspectionStopsWhenContextIsDestroyed();

    void signatureCheckReducesVerificationResultsToTheOperatorVocabulary();
    void signatureCheckAdmitsOnlyTheCurrentRevisionAndClearsWithTheDocument();

    // The typed direct-edit gateway (ADR-014, issue #233).
    void authoringEditAdvancesTheRevisionAndWritesTheOutline();
    void authoringEditRefusesAStaleRevisionWithoutMutating();
    void authoringEditRefusesWithoutAReadyDocumentOrPermission();

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

    // The embedded contract is docs/loop-shell-actions.json itself. A static
    // library that lost its resource would present an empty command set as a
    // working one, so this also pins the resource wiring.
    QVERIFY2(catalog.isLoaded(), qPrintable(catalog.loadError()));
    QCOMPARE(catalog.descriptors().size(), 113);

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
    // The display colour modes (#235), the select/copy/capture commands (#232),
    // the measure commands (#238), the shell settings/diagnostics surfaces
    // (#239), the bookmark/hyperlink commands (#233), the annotation authoring
    // family (#234) and the form/attachment/metadata commands (#242) joined the
    // implemented set in epic #231.
    QCOMPARE(implemented, 94);
