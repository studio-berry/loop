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
#include "pdfdocumentreader.h"
#include "pdfparser.h"

#include <QTest>
#include <QIODevice>

#include <cstring>
#include <utility>

class SequentialByteDevice final : public QIODevice
{
public:
    explicit SequentialByteDevice(QByteArray data) : m_data(std::move(data)) { }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return (m_data.size() - m_offset) + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 remaining = m_data.size() - m_offset;
        const qint64 count = qMin(maxSize, remaining);
        if (count > 0)
        {
            std::memcpy(data, m_data.constData() + m_offset, static_cast<size_t>(count));
            m_offset += count;
        }
        return count;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QByteArray m_data;
    qint64 m_offset = 0;
};

class ProcessingBudgetTest : public QObject
{
    Q_OBJECT

private slots:
    void cumulativeDecodedBytesAreDocumentWide();
    void depthIsBoundedAndTyped();
    void elapsedTimeIsCooperativelyChecked();
    void parserObjectDepthUsesConfiguredBudget();
    void sequentialInputIsBoundedBeforeParsing();
    void namedPoolsReportExactKind();
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

void ProcessingBudgetTest::parserObjectDepthUsesConfiguredBudget()
{
    pdf::PDFProcessingLimits limits;
    limits.maxObjectDepth = 1;
    pdf::PDFProcessingBudget budget(limits);
    pdf::PDFParser parser(QByteArray("[[0]]"), nullptr, pdf::PDFParser::None, &budget);

    try
    {
        parser.getObject();
        QFAIL("expected parser object-depth budget failure");
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::ObjectDepth);
        QCOMPARE(exception.getDetail().limit, uint64_t(1));
        QCOMPARE(exception.getDetail().context, QStringLiteral("PDF object nesting"));
    }
}

void ProcessingBudgetTest::sequentialInputIsBoundedBeforeParsing()
{
    pdf::PDFProcessingLimits limits;
    limits.maxInputBytes = 4;
    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, false, false, limits);
    SequentialByteDevice device(QByteArrayLiteral("0123456789"));
    QVERIFY(device.open(QIODevice::ReadOnly));

    reader.readFromDevice(&device);

    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::Failed);
    QVERIFY(reader.getErrorMessage().contains(QStringLiteral("input-bytes")));
}

void ProcessingBudgetTest::namedPoolsReportExactKind()
{
    pdf::PDFProcessingLimits limits;
    limits.maxEvidenceCacheBytes = 8;
    pdf::PDFProcessingBudget budget(limits);
    try
    {
        budget.chargeEvidenceCacheBytes(16, QStringLiteral("evidence cache"));
        QFAIL("expected budget exception");
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::EvidenceCacheBytes);
        QCOMPARE(QString::fromLatin1(pdf::getPDFBudgetKindName(exception.getDetail().kind)),
                 QStringLiteral("evidence-cache-bytes"));
        QCOMPARE(exception.getDetail().context, QStringLiteral("evidence cache"));
    }
}

QTEST_MAIN(ProcessingBudgetTest)
#include "tst_processingbudgettest.moc"
