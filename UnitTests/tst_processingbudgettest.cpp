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

#include "pdfprocessingbudget.h"

#include <QTest>

class ProcessingBudgetTest : public QObject
{
    Q_OBJECT

private slots:
    void cumulativeDecodedBytesAreDocumentWide();
    void depthIsBoundedAndTyped();
    void elapsedTimeIsCooperativelyChecked();
};

void ProcessingBudgetTest::cumulativeDecodedBytesAreDocumentWide()
{
    pdf::PDFProcessingLimits limits;
    limits.maxDecodedStreamBytes = 10;
    limits.maxCumulativeDecodedBytes = 15;
    pdf::PDFProcessingBudget budget(limits);

    budget.checkDecodedStreamSize(10, 10, QStringLiteral("stream-a"));
    budget.chargeDecodedBytes(10, QStringLiteral("stream-a"));
    budget.checkDecodedStreamSize(5, 5, QStringLiteral("stream-b"));
    budget.chargeDecodedBytes(5, QStringLiteral("stream-b"));

    try
    {
        budget.chargeDecodedBytes(1, QStringLiteral("stream-c"));
        QFAIL("expected cumulative decoded-byte budget failure");
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::CumulativeDecodedBytes);
        QCOMPARE(exception.getDetail().limit, uint64_t(15));
        QCOMPARE(exception.getDetail().attempted, uint64_t(16));
    }
}

void ProcessingBudgetTest::depthIsBoundedAndTyped()
{
    pdf::PDFProcessingLimits limits;
    limits.maxRecursiveContentDepth = 1;
    pdf::PDFProcessingBudget budget(limits);
    pdf::PDFProcessingBudget::DepthScope outer(budget,
                                                pdf::PDFBudgetKind::RecursiveContentDepth,
                                                QStringLiteral("form"));

    try
    {
        pdf::PDFProcessingBudget::DepthScope inner(budget,
                                                    pdf::PDFBudgetKind::RecursiveContentDepth,
                                                    QStringLiteral("nested form"));
        QFAIL("expected recursive-depth budget failure");
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::RecursiveContentDepth);
        QCOMPARE(exception.getDetail().attempted, uint64_t(2));
    }
}

void ProcessingBudgetTest::elapsedTimeIsCooperativelyChecked()
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point now = Clock::time_point{};
    pdf::PDFProcessingLimits limits;
    limits.maxElapsed = std::chrono::milliseconds(10);
    pdf::PDFProcessingBudget budget(limits, [&now] { return now; });

    now += std::chrono::milliseconds(11);
    try
    {
        budget.checkElapsed(QStringLiteral("test clock"));
        QFAIL("expected elapsed-time budget failure");
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::ElapsedTime);
        QCOMPARE(exception.getDetail().limit, uint64_t(10));
    }
}

QTEST_MAIN(ProcessingBudgetTest)
#include "tst_processingbudgettest.moc"
