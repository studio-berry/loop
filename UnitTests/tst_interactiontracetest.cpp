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

#include "pdfinteractiontrace_p.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QTest>


class InteractionTraceTest final : public QObject
{
    Q_OBJECT

private slots:
    void recordsInputToFrameAndPercentiles();
    void reportsKnownAndUnknownBudgets();
    void attributesSlowFramesAndExclusiveStages();
    void boundsSamplingAndKeepsPayloadPrivate();
};

void InteractionTraceTest::recordsInputToFrameAndPercentiles()
{
    qint64 nowNs = 0;
    pdf::PDFInteractionTraceRecorder recorder([&nowNs]() { return nowNs; });
    pdf::PDFInteractionTraceRecorder::Config config;
    config.maxSamples = 8;
    config.refreshRateHz = 60.0;
    recorder.setConfig(config);
    recorder.setEnabled(true);

    {
        auto input = recorder.beginInput(pdf::PDFInteractionTraceRecorder::InputKind::MouseMove);
        nowNs += 5000000;
        auto frame = recorder.beginFrame(2, 3);
        nowNs += 10000000;
        Q_UNUSED(input);
        Q_UNUSED(frame);
    }

    const QJsonObject summary = recorder.summary();
    const QJsonObject latency = summary.value(QStringLiteral("input_to_frame_ms")).toObject();
    QCOMPARE(latency.value(QStringLiteral("sample_count")).toInt(), 1);
    QCOMPARE(latency.value(QStringLiteral("p50_ms")).toDouble(), 15.0);

    const QJsonObject frameTime = summary.value(QStringLiteral("frame_time_ms")).toObject();
    QCOMPARE(frameTime.value(QStringLiteral("sample_count")).toInt(), 1);
    QCOMPARE(frameTime.value(QStringLiteral("p50_ms")).toDouble(), 10.0);
    QCOMPARE(summary.value(QStringLiteral("visible_page_count")).toInt(), 2);
    QCOMPARE(summary.value(QStringLiteral("pending_async_work")).toObject().value(QStringLiteral("queue_depth")).toInt(), 3);
}

void InteractionTraceTest::reportsKnownAndUnknownBudgets()
{
    qint64 nowNs = 0;
    pdf::PDFInteractionTraceRecorder recorder([&nowNs]() { return nowNs; });
    recorder.setEnabled(true);

    {
        auto frame = recorder.beginFrame(0, -1);
        nowNs += 1000000;
        Q_UNUSED(frame);
    }
    QJsonObject summary = recorder.summary();
    QJsonObject budget = summary.value(QStringLiteral("budgets")).toObject();
    QCOMPARE(budget.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QVERIFY(budget.value(QStringLiteral("frame_budget_ms")).isNull());
    QCOMPARE(budget.value(QStringLiteral("reference_60_hz_ms")).toDouble(), 1000.0 / 60.0);
    QCOMPARE(budget.value(QStringLiteral("reference_120_hz_ms")).toDouble(), 1000.0 / 120.0);

    recorder.setRefreshRateHz(120.0);
    summary = recorder.summary();
    budget = summary.value(QStringLiteral("budgets")).toObject();
    QCOMPARE(budget.value(QStringLiteral("status")).toString(), QStringLiteral("known"));
    QCOMPARE(budget.value(QStringLiteral("frame_budget_ms")).toDouble(), 1000.0 / 120.0);
}

void InteractionTraceTest::attributesSlowFramesAndExclusiveStages()
{
    qint64 nowNs = 0;
    pdf::PDFInteractionTraceRecorder recorder([&nowNs]() { return nowNs; });
    pdf::PDFInteractionTraceRecorder::Config config;
    config.refreshRateHz = 60.0;
    recorder.setConfig(config);
    recorder.setEnabled(true);

    {
        auto frame = recorder.beginFrame(1, 0);
        auto render = recorder.beginStage(pdf::PDFInteractionTraceRecorder::Stage::PageRender);
        nowNs += 20000000;
        {
            auto overlay = recorder.beginStage(pdf::PDFInteractionTraceRecorder::Stage::Overlay);
            nowNs += 10000000;
            Q_UNUSED(overlay);
        }
        nowNs += 5000000;
        Q_UNUSED(render);
        Q_UNUSED(frame);
    }

    const QJsonObject summary = recorder.summary();
    const QJsonObject causes = summary.value(QStringLiteral("slow_frame_cause_buckets")).toObject();
    QCOMPARE(causes.value(QStringLiteral("page_rendering")).toInt(), 1);
    const QJsonObject stages = summary.value(QStringLiteral("stage_time_ms")).toObject();
    QCOMPARE(stages.value(QStringLiteral("page_rendering")).toObject().value(QStringLiteral("p50_ms")).toDouble(), 25.0);
    QCOMPARE(stages.value(QStringLiteral("overlays")).toObject().value(QStringLiteral("p50_ms")).toDouble(), 10.0);
}

void InteractionTraceTest::boundsSamplingAndKeepsPayloadPrivate()
{
    qint64 nowNs = 0;
    pdf::PDFInteractionTraceRecorder recorder([&nowNs]() { return nowNs; });
    pdf::PDFInteractionTraceRecorder::Config config;
    config.maxSamples = 2;
    config.sampleEvery = 2;
    config.evidenceState = pdf::PDFInteractionTraceRecorder::EvidenceState::StaticOnly;
    recorder.setConfig(config);
    recorder.setEnabled(true);

    for (int index = 0; index < 5; ++index)
    {
        {
            auto frame = recorder.beginFrame(index + 1, index);
            nowNs += 1000000;
            Q_UNUSED(frame);
        }
    }

    const QJsonObject summary = recorder.summary();
    QCOMPARE(summary.value(QStringLiteral("evidence_state")).toString(), QStringLiteral("static-only"));
    QCOMPARE(summary.value(QStringLiteral("sampling")).toObject().value(QStringLiteral("frame_samples")).toInt(), 2);
    QVERIFY(!summary.value(QStringLiteral("privacy")).toObject().value(QStringLiteral("document_payload_recorded")).toBool());
    QVERIFY(!summary.contains(QStringLiteral("text")));
    QVERIFY(!summary.contains(QStringLiteral("pixels")));
    QVERIFY(!summary.contains(QStringLiteral("path")));
}

QTEST_APPLESS_MAIN(InteractionTraceTest)

#include "tst_interactiontracetest.moc"
