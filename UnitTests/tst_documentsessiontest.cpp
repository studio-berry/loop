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

#include "pdfdocumentsession.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentbuilder.h"
#include "pdfjobscheduler.h"
#include "pdfobject.h"

#include <QtTest>

#include <atomic>
#include <chrono>
#include <thread>

class DocumentSessionTest : public QObject
{
    Q_OBJECT

private slots:
    void nullDocument_sessionIsInvalid();
    void compilePage_cachesResult();
    void getDecodedStream_cachesResult();
    void invalidate_clearsCaches();
    void setRendererFeatures_invalidatesCompileCache();
    void revisionFence_rejectsSupersededResults();
    void concurrentScheduledResults_rejectSupersededRevisions();
};

void DocumentSessionTest::nullDocument_sessionIsInvalid()
{
    pdf::PDFDocumentSession session(nullptr);
    QVERIFY(!session.isValid());
    QCOMPARE(session.getDocument(), nullptr);
    QCOMPARE(session.compilePage(0), nullptr);
    QVERIFY(session.getDecodedStream(pdf::PDFObjectReference()).isEmpty());
}

void DocumentSessionTest::compilePage_cachesResult()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    QVERIFY(session.isValid());

    const pdf::PDFPrecompiledPage* first = session.compilePage(0);
    QVERIFY(first != nullptr);

    const pdf::PDFPrecompiledPage* second = session.compilePage(0);
    QCOMPARE(second, first);

    QCOMPARE(session.compilePage(99), nullptr);
}

void DocumentSessionTest::getDecodedStream_cachesResult()
{
    pdf::PDFDocumentBuilder builder;

    pdf::PDFDictionary dictionary;
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(5));
    pdf::PDFObject streamObject = pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(dictionary), QByteArray("hello")));
    pdf::PDFObjectReference streamReference = builder.addObject(std::move(streamObject));

    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    QByteArray first = session.getDecodedStream(streamReference);
    QCOMPARE(first, QByteArray("hello"));

    QByteArray second = session.getDecodedStream(streamReference);
    QCOMPARE(second, first);
}

void DocumentSessionTest::invalidate_clearsCaches()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    const pdf::PDFPrecompiledPage* compiled = session.compilePage(0);
    QVERIFY(compiled != nullptr);
    QCOMPARE(session.compilePage(0), compiled);

    session.invalidate();

    const pdf::PDFPrecompiledPage* after = session.compilePage(0);
    QVERIFY(after != nullptr);
    QCOMPARE(session.compilePage(0), after);
}

void DocumentSessionTest::setRendererFeatures_invalidatesCompileCache()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    const pdf::PDFPrecompiledPage* compiled = session.compilePage(0);
    QVERIFY(compiled != nullptr);

    pdf::PDFRenderer::Features newFeatures = pdf::PDFRenderer::getDefaultFeatures();
    newFeatures.setFlag(pdf::PDFRenderer::ClipToCropBox, !newFeatures.testFlag(pdf::PDFRenderer::ClipToCropBox));
    session.setRendererFeatures(newFeatures);
    QCOMPARE(session.getRendererFeatures(), newFeatures);

    const pdf::PDFPrecompiledPage* after = session.compilePage(0);
    QVERIFY(after != nullptr);
    QCOMPARE(session.compilePage(0), after);
}

void DocumentSessionTest::revisionFence_rejectsSupersededResults()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentContext context(&document);
    QSignalSpy revisionSpy(&context, &pdf::PDFDocumentContext::revisionChanged);
    const pdf::PDFRevisionIdentity firstRevision = context.getRevision();

    for (int i = 0; i < 512; ++i)
    {
        const pdf::PDFRevisionIdentity jobRevision = context.getRevision();
        context.markModified(pdf::PDFModifiedDocument::PageContents);

        QVERIFY(!context.isCurrent(jobRevision));
        QVERIFY(context.isCurrent(context.getRevision()));
        QVERIFY(context.getRevision().documentRevision > firstRevision.documentRevision);
    }

    QCOMPARE(revisionSpy.count(), 512);
}

void DocumentSessionTest::concurrentScheduledResults_rejectSupersededRevisions()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentContext context(&document);
    pdf::PDFJobScheduler scheduler(4);
    const QString documentKey = context.getDocumentIdentity().documentId;
    pdf::PDFArtifactIdentity artifact;
    artifact.sha256 = QString(64, QLatin1Char('a'));
    artifact.size = 100;
    artifact.logicalName = QStringLiteral("concurrent-session.pdf");
    artifact.storageToken = documentKey;

    const QList<pdf::PDFJobKind> jobKinds = {
        pdf::PDFJobKind::Rendering,
        pdf::PDFJobKind::Preflight,
        pdf::PDFJobKind::Thumbnail,
        pdf::PDFJobKind::Other
    };

    for (int round = 0; round < 32; ++round)
    {
        const pdf::PDFRevisionIdentity submittedRevision = context.getRevision();
        const QString submittedRevisionText = submittedRevision.toString();
        scheduler.setCurrentRevision(documentKey, submittedRevisionText);

        std::atomic_bool releaseJobs = false;
        std::atomic_int startedJobs = 0;
        QList<QString> jobIds;
        for (int index = 0; index < jobKinds.size(); ++index)
        {
            pdf::PDFJobSpec spec;
            spec.kind = jobKinds.at(index);
            spec.priority = index == 0 ? pdf::PDFJobPriority::VisiblePage : pdf::PDFJobPriority::Operator;
            spec.artifact = artifact;
            spec.documentKey = documentKey;
            spec.documentRevision = submittedRevisionText;
            spec.operationId = index == 3 ? QStringLiteral("repair-plan") : QStringLiteral("session-stress");
            spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;
            jobIds.append(scheduler.submit(spec, [&releaseJobs, &startedJobs, index](pdf::PDFJobContext&)
                                           {
                                                ++startedJobs;
                                                while (!releaseJobs.load(std::memory_order_acquire))
                                                {
                                                    std::this_thread::yield();
                                                }
                                                std::this_thread::sleep_for(std::chrono::milliseconds(index % 3)); }));
        }

        QTRY_VERIFY_WITH_TIMEOUT(startedJobs.load(std::memory_order_acquire) == jobKinds.size(), 1000);

        // Advance every part of the revision fence while all four result
        // producers are still active. They must be rejected at the scheduler
        // boundary, regardless of completion order.
        context.markModified(pdf::PDFModifiedDocument::PageContents);
        const pdf::PDFRevisionIdentity currentRevision = context.getRevision();
        QVERIFY(currentRevision.documentRevision > submittedRevision.documentRevision);
        QVERIFY(currentRevision.cacheGeneration > submittedRevision.cacheGeneration);
        scheduler.setCurrentRevision(documentKey, currentRevision.toString());
        releaseJobs.store(true, std::memory_order_release);

        for (const QString& jobId : jobIds)
        {
            QVERIFY(scheduler.waitForFinished(jobId, 5000));
            const pdf::PDFJobSnapshot snapshot = scheduler.snapshot(jobId);
            QCOMPARE(snapshot.status, pdf::PDFJobStatus::Stale);
            QCOMPARE(snapshot.artifact.storageToken, artifact.storageToken);
            QCOMPARE(snapshot.documentRevision, submittedRevisionText);
        }

        // A result carrying the new, complete revision is accepted after the
        // mutation; this prevents the stress test from passing merely because
        // the scheduler rejects every result.
        pdf::PDFJobSpec currentSpec;
        currentSpec.kind = pdf::PDFJobKind::Rendering;
        currentSpec.priority = pdf::PDFJobPriority::VisiblePage;
        currentSpec.artifact = artifact;
        currentSpec.documentKey = documentKey;
        currentSpec.documentRevision = currentRevision.toString();
        currentSpec.operationId = QStringLiteral("current-render");
        const QString currentJobId = scheduler.submit(currentSpec, [](pdf::PDFJobContext& context)
                                                      { context.reportProgress(100); });
        QVERIFY(scheduler.waitForFinished(currentJobId, 1000));
        const pdf::PDFJobSnapshot currentSnapshot = scheduler.snapshot(currentJobId);
        QCOMPARE(currentSnapshot.status, pdf::PDFJobStatus::Succeeded);
        QCOMPARE(currentSnapshot.documentRevision, currentRevision.toString());
    }
}

QTEST_GUILESS_MAIN(DocumentSessionTest)

#include "tst_documentsessiontest.moc"
