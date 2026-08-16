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

#include "pdfartifactstore.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentsession.h"
#include "pdfjobscheduler.h"
#include "pdfoperationhistorystore.h"
#include "pdfpreflightverdict.h"
#include "pdfsavepolicy.h"
#include "preflightengine.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

class LifecycleTest : public QObject
{
    Q_OBJECT

private slots:
    void openEditPreflightCancelSaveRecoverRollback();
};

void LifecycleTest::openEditPreflightCancelSaveRecoverRollback()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);
    const pdf::PDFRevisionIdentity opened = context.getRevision();
    QVERIFY(opened.isValid());
    QCOMPARE(opened.documentRevision, quint64(0));

    context.markModified();
    const pdf::PDFRevisionIdentity edited = context.getRevision();
    QVERIFY(edited.documentRevision > opened.documentRevision);

    pdf::PDFDocumentSession* session = context.getSession();
    QVERIFY(session);
    pdf::PreflightEngine engine(session);
    pdf::PreflightProfileData profile;
    profile.name = QStringLiteral("lifecycle");
    pdf::PreflightCheckConfig check;
    check.id = QStringLiteral("bleed");
    profile.checks.append(check);
    const pdf::PreflightResult preflight = engine.run(profile);
    QVERIFY(context.isCurrent(session->getRevision()));
    QVERIFY(preflight.inspectionComplete);
    QCOMPARE(preflight.pass, pdf::reducePreflightVerdict(preflight).isPass());

    pdf::PDFJobScheduler scheduler(1);
    scheduler.setCurrentRevision(opened.document.documentId, edited.toString());
    pdf::PDFJobSpec spec;
    spec.jobId = QStringLiteral("preflight");
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.documentKey = opened.document.documentId;
    spec.documentRevision = opened.toString();
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;
    bool ran = false;
    const QString jobId = scheduler.submit(spec, [&ran](pdf::PDFJobContext&)
                                           { ran = true; });
    QVERIFY(scheduler.waitForFinished(jobId, 2000));
    QCOMPARE(scheduler.snapshot(jobId).status, pdf::PDFJobStatus::Stale);
    QVERIFY(!ran);

    pdf::PDFJobSpec cancelSpec;
    cancelSpec.jobId = QStringLiteral("cancel-me");
    cancelSpec.kind = pdf::PDFJobKind::Preflight;
    pdf::PDFJobCancellationTokenPtr token;
    const QString cancelId = scheduler.submit(cancelSpec, [](pdf::PDFJobContext& ctx)
                                              {
        while (!ctx.isCancellationRequested())
        {
        } }, &token);
    QVERIFY(token);
    QVERIFY(scheduler.cancel(cancelId));
    QVERIFY(scheduler.waitForFinished(cancelId, 2000));
    QCOMPARE(scheduler.snapshot(cancelId).status, pdf::PDFJobStatus::Cancelled);

    const pdf::PDFOperationSavePolicy policy = pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("ordinary edit"));
    QCOMPARE(policy.mode, pdf::PDFSaveMode::IncrementalAppend);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());
    pdf::PDFArtifactStore store(temporary.path());
    const pdf::PDFArtifactStoreResult imported = store.importBytes(QByteArrayLiteral("%PDF-1.4 test"), { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    QVERIFY(imported.success);
    QVERIFY(history.registerOriginalInput(imported.artifact));
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("lifecycle");
    execution.input = imported.artifact;
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));
    pdf::PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.kind = pdf::PDFOperationHistoryEventKind::PreflightRun;
    running.status = pdf::PDFOperationHistoryStatus::Cancelled;
    QVERIFY(history.appendEvent(running));
    const pdf::PDFOperationHistoryVerification verified = history.verify();
    QVERIFY(verified.verified);
    QCOMPARE(history.events().last().kind, pdf::PDFOperationHistoryEventKind::PreflightRun);
    QCOMPARE(history.events().last().status, pdf::PDFOperationHistoryStatus::Cancelled);
}

QTEST_GUILESS_MAIN(LifecycleTest)
#include "tst_lifecycletest.moc"
