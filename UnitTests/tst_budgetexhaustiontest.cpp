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
#include "pdfparser.h"
#include "pdfpreflightverdict.h"
#include "pdfprocessingbudget.h"
#include "pdfthinpartprobe.h"
#include "preflightengine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QtTest>

#include <functional>

class BudgetExhaustionTest : public QObject
{
    Q_OBJECT

private slots:
    void everyKindReportsExactKindAndPool();
    void generatedCorpusIsIncompleteNeverPass();
    void preflightEvidenceBudgetIsIncomplete();
    void rasterSizeBudgetIsIncomplete();
};

namespace
{

void expectKind(pdf::PDFBudgetKind kind, pdf::PDFBudgetPool pool, const std::function<void()>& charge)
{
    try
    {
        charge();
        QFAIL("expected budget exhaustion");
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        QCOMPARE(exception.getDetail().kind, kind);
        QCOMPARE(exception.getDetail().pool, pool);
        QVERIFY(!QString::fromLatin1(pdf::getPDFBudgetKindName(kind)).isEmpty());
    }
}

pdf::PDFDocument rgbPageDocument()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFPageContentStreamBuilder contentBuilder(&builder, pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    QPainter* painter = contentBuilder.begin(page);
    painter->fillRect(QRectF(10, 10, 80, 80), QColor(255, 0, 0));
    contentBuilder.end(painter);
    return builder.build();
}

} // namespace

void BudgetExhaustionTest::everyKindReportsExactKindAndPool()
{
    {
        pdf::PDFProcessingLimits limits;
        limits.maxInputBytes = 1;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::InputBytes, pdf::PDFBudgetPool::DocumentModel,
                   [&budget] { budget.chargeInputBytes(2, QStringLiteral("input")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxDecodedStreamBytes = 1;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::SingleDecodedStreamBytes, pdf::PDFBudgetPool::DecodedStreams,
                   [&budget] { budget.checkDecodedStreamSize(8, 8, QStringLiteral("stream")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxCumulativeDecodedBytes = 1;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::CumulativeDecodedBytes, pdf::PDFBudgetPool::DecodedStreams,
                   [&budget] { budget.chargeDecodedBytes(2, QStringLiteral("decode")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxDecompressionRatio = 2;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::DecompressionRatio, pdf::PDFBudgetPool::DecodedStreams,
                   [&budget] { budget.checkDecodedStreamSize(20, 2, QStringLiteral("ratio")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxObjectDepth = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::ObjectDepth, pdf::PDFBudgetPool::DocumentModel, [&budget]
                   {
                       pdf::PDFProcessingBudget::DepthScope scope(budget, pdf::PDFBudgetKind::ObjectDepth, QStringLiteral("obj"));
                   });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxRecursiveContentDepth = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::RecursiveContentDepth, pdf::PDFBudgetPool::DocumentModel, [&budget]
                   {
                       pdf::PDFProcessingBudget::DepthScope scope(budget, pdf::PDFBudgetKind::RecursiveContentDepth, QStringLiteral("form"));
                   });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxObjectsVisited = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::ObjectsVisited, pdf::PDFBudgetPool::DocumentModel,
                   [&budget] { budget.chargeObject(QStringLiteral("object")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxRenderOperations = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::RenderOperations, pdf::PDFBudgetPool::RasterTile,
                   [&budget] { budget.chargeRenderOperation(1, QStringLiteral("op")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxRenderPixels = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::RenderPixels, pdf::PDFBudgetPool::RasterTile,
                   [&budget] { budget.chargeRenderPixels(1, QStringLiteral("px")); });
    }
    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        pdf::PDFProcessingLimits limits;
        limits.maxElapsed = std::chrono::milliseconds(0);
        pdf::PDFProcessingBudget budget(limits, [&now] { return now; });
        now += std::chrono::milliseconds(1);
        expectKind(pdf::PDFBudgetKind::ElapsedTime, pdf::PDFBudgetPool::DocumentModel,
                   [&budget] { budget.checkElapsed(QStringLiteral("clock")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxEvidenceRecords = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::EvidenceRecords, pdf::PDFBudgetPool::EvidenceCache,
                   [&budget] { budget.chargeEvidenceRecords(1, QStringLiteral("graph")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxUndoSnapshots = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::UndoSnapshots, pdf::PDFBudgetPool::Undo,
                   [&budget] { budget.chargeUndoSnapshot(QStringLiteral("undo")); });
    }
    {
        pdf::PDFProcessingLimits limits;
        limits.maxRollbackArtifacts = 0;
        pdf::PDFProcessingBudget budget(limits);
        expectKind(pdf::PDFBudgetKind::RollbackArtifacts, pdf::PDFBudgetPool::Rollback,
                   [&budget] { budget.chargeRollbackArtifact(QStringLiteral("rollback")); });
    }
}

void BudgetExhaustionTest::generatedCorpusIsIncompleteNeverPass()
{
    pdf::PDFProcessingLimits limits;
    limits.maxObjectDepth = 1;
    pdf::PDFProcessingBudget budget(limits);
    pdf::PDFParser parser(QByteArray("[[[0]]]"), nullptr, pdf::PDFParser::None, &budget);
    bool exhausted = false;
    try
    {
        parser.getObject();
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        exhausted = true;
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::ObjectDepth);
        QCOMPARE(QString::fromLatin1(pdf::getPDFBudgetKindName(exception.getDetail().kind)), QStringLiteral("object-depth"));
    }
    QVERIFY(exhausted);
}

void BudgetExhaustionTest::preflightEvidenceBudgetIsIncomplete()
{
    pdf::PDFDocument document = rgbPageDocument();
    pdf::PDFDocumentSession session(&document);
    pdf::PDFProcessingLimits limits = session.getProcessingLimits();
    limits.maxEvidenceRecords = 0;
    session.setProcessingLimits(limits);

    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Exhaust evidence") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
              { QStringLiteral("id"), QStringLiteral("color-inventory") },
              { QStringLiteral("severity"), QStringLiteral("info") } } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.inspectionComplete);
    QCOMPARE(result.errorCode, QStringLiteral("budget-exceeded"));
    QVERIFY(!result.checkStatuses.isEmpty());
    QCOMPARE(result.checkStatuses.first().budgetKind, QStringLiteral("evidence-records"));
    QCOMPARE(result.checkStatuses.first().budgetPool, QStringLiteral("evidence-cache"));

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QVERIFY(!verdict.isPass());
}

void BudgetExhaustionTest::rasterSizeBudgetIsIncomplete()
{
    QPainterPath path;
    path.addRect(QRectF(0, 0, 200, 200));
    bool exhausted = false;
    try
    {
        pdf::measureThinPartPath(path, 72, 1, false, QStringLiteral("synthetic-raster"));
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        exhausted = true;
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::RenderPixels);
        QCOMPARE(exception.getDetail().pool, pdf::PDFBudgetPool::RasterTile);
    }
    QVERIFY(exhausted);
}

QTEST_MAIN(BudgetExhaustionTest)
#include "tst_budgetexhaustiontest.moc"
