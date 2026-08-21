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

#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdfjobscheduler.h"
#include "pdfworkloadenvelope.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QtTest>

#include <atomic>
#include <thread>

class WorkloadEnvelopeTest : public QObject
{
    Q_OBJECT

private slots:
    void identityFieldsArePresent();
    void shedPrefetchAndQualityBeforeInteraction();
    void pageHeavyEnvelopeRecordsIdentity();
    void interactionSlotRunsWhenBackgroundIsSaturated();
};

void WorkloadEnvelopeTest::identityFieldsArePresent()
{
    qputenv("GIT_COMMIT", QByteArrayLiteral("0123456789abcdef0123456789abcdef01234567"));
    const pdf::PDFRunIdentity identity = pdf::PDFRunIdentity::capture();
    QVERIFY(!identity.compiler.isEmpty());
    QVERIFY(!identity.buildType.isEmpty());
    QVERIFY(!identity.os.isEmpty());
    QVERIFY(!identity.qtVersion.isEmpty());
    QVERIFY(!identity.cpuArchitecture.isEmpty());
    QVERIFY(!identity.gpu.isEmpty());
    QVERIFY(!identity.renderer.isEmpty());
    QCOMPARE(identity.commit, QStringLiteral("0123456789abcdef0123456789abcdef01234567"));
    QVERIFY(identity.toJson().contains(QStringLiteral("commit")));
    QVERIFY(identity.toJson().contains(QStringLiteral("qt")));
}

void WorkloadEnvelopeTest::shedPrefetchAndQualityBeforeInteraction()
{
    pdf::PDFDocumentBuilder builder;
    for (int i = 0; i < 6; ++i)
    {
        builder.appendPage(QRectF(0, 0, 100, 100));
    }
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);

    QCOMPARE(session.compileCacheLimit(), pdf::PDFDocumentSession::CompileCacheLimit);
    QVERIFY(session.prefetchEnabled());
    QCOMPARE(session.qualityPercent(), 100);
    QVERIFY(session.compilePage(0));
    QVERIFY(session.compilePage(1));
    QVERIFY(session.compilePage(2));

    session.shedPrefetchAndQuality();
    QVERIFY(!session.prefetchEnabled());
    QCOMPARE(session.qualityPercent(), pdf::PDFDocumentSession::ShedQualityPercent);
    QVERIFY(session.qualityPrefetchShed());
    QCOMPARE(session.compileCacheLimit(), pdf::PDFDocumentSession::ShedCompileCacheLimit);
    QVERIFY(session.compileCacheLimit() < pdf::PDFDocumentSession::CompileCacheLimit);
    QVERIFY(session.compilePage(3));
}

void WorkloadEnvelopeTest::pageHeavyEnvelopeRecordsIdentity()
{
    qputenv("GIT_COMMIT", QByteArrayLiteral("wave-d-envelope-commit"));
    pdf::PDFDocumentBuilder builder;
    const int pageCount = 24;
    for (int i = 0; i < pageCount; ++i)
    {
        builder.appendPage(QRectF(0, 0, 72, 72));
    }
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < pageCount; ++i)
    {
        QVERIFY(session.compilePage(static_cast<size_t>(i)));
    }
    session.shedPrefetchAndQuality();

    pdf::PDFWorkloadEnvelope envelope;
    envelope.identity = pdf::PDFRunIdentity::capture();
    envelope.identity.fixtureDigest = pdf::PDFRunIdentity::digestBytes(QByteArrayLiteral("page-heavy-24"));
    envelope.family = QStringLiteral("page-heavy");
    envelope.status = QStringLiteral("complete");
    envelope.pageCount = pageCount;
    envelope.openToFirstViewMs = 4;
    envelope.rssHighWaterBytes = pdf::PDFWorkloadEnvelope::currentRssHighWaterBytes();
    envelope.cacheHighWaterBytes = 2048;
    envelope.elapsedMs = timer.elapsed();
    envelope.pressureShedCount = 1;
    envelope.prefetchShed = session.qualityPrefetchShed();
    envelope.interactionSlotHeld = true;

    const QJsonObject json = envelope.toJson();
    QCOMPARE(json.value(QStringLiteral("family")).toString(), QStringLiteral("page-heavy"));
    QCOMPARE(json.value(QStringLiteral("status")).toString(), QStringLiteral("complete"));
    QCOMPARE(json.value(QStringLiteral("open_to_first_view_ms")).toInt(), 4);
    QCOMPARE(json.value(QStringLiteral("cache_high_water_bytes")).toInt(), 2048);
    QCOMPARE(json.value(QStringLiteral("pressure_shed_count")).toInt(), 1);
    QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("commit")));
    QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("os")));
    QVERIFY(json.value(QStringLiteral("identity")).toObject().contains(QStringLiteral("qt")));
    QVERIFY(!json.value(QStringLiteral("identity")).toObject().value(QStringLiteral("fixture_digest")).toString().isEmpty());
    QVERIFY(json.value(QStringLiteral("prefetch_shed")).toBool());
    QVERIFY(json.value(QStringLiteral("interaction_slot_held")).toBool());
    QVERIFY(json.value(QStringLiteral("page_count")).toInt() == pageCount);
}

void WorkloadEnvelopeTest::interactionSlotRunsWhenBackgroundIsSaturated()
{
    pdf::PDFJobScheduler scheduler(2);
    std::atomic_bool releaseBackground = false;
    std::atomic_int backgroundStarted = 0;
    std::atomic_bool interactionStarted = false;

    auto backgroundWork = [&releaseBackground, &backgroundStarted](pdf::PDFJobContext&)
    {
        ++backgroundStarted;
        while (!releaseBackground.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    };

    pdf::PDFJobSpec first;
    first.jobId = QStringLiteral("background-1");
    first.priority = pdf::PDFJobPriority::Background;
    const QString firstId = scheduler.submit(first, backgroundWork);
    pdf::PDFJobSpec second;
    second.jobId = QStringLiteral("background-2");
    second.priority = pdf::PDFJobPriority::Background;
    const QString secondId = scheduler.submit(second, backgroundWork);
    QTRY_VERIFY_WITH_TIMEOUT(backgroundStarted.load(std::memory_order_acquire) == 1, 1000);

    pdf::PDFJobSpec interaction;
    interaction.jobId = QStringLiteral("interaction-reserved");
    interaction.priority = pdf::PDFJobPriority::Interaction;
    const QString interactionId = scheduler.submit(interaction, [&interactionStarted](pdf::PDFJobContext&)
                                                   { interactionStarted = true; });

    QVERIFY(scheduler.waitForFinished(interactionId, 1000));
    QVERIFY(interactionStarted.load(std::memory_order_acquire));
    QCOMPARE(scheduler.snapshot(interactionId).status, pdf::PDFJobStatus::Succeeded);
    QCOMPARE(scheduler.snapshot(secondId).status, pdf::PDFJobStatus::Queued);

    releaseBackground = true;
    QVERIFY(scheduler.waitForFinished(firstId, 1000));
    QVERIFY(scheduler.waitForFinished(secondId, 1000));
}

QTEST_GUILESS_MAIN(WorkloadEnvelopeTest)
#include "tst_workloadenvelopetest.moc"
