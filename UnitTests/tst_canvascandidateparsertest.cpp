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

#include "../CanvasBenchmark/candidate.h"

#include <QtTest>

class CanvasCandidateParserTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void acceptsKnownCandidates();
    void rejectsUnknownCandidates();
};

void CanvasCandidateParserTest::acceptsKnownCandidates()
{
    const auto quickItem = canvasbenchmark::parseCandidate(QStringLiteral("quick-item"));
    QVERIFY(quickItem.has_value());
    QCOMPARE(*quickItem, canvasbenchmark::Candidate::QuickItem);

    const auto widget = canvasbenchmark::parseCandidate(QStringLiteral("widget-baseline"));
    QVERIFY(widget.has_value());
    QCOMPARE(*widget, canvasbenchmark::Candidate::Widget);
}

void CanvasCandidateParserTest::rejectsUnknownCandidates()
{
    QVERIFY(!canvasbenchmark::parseCandidate(QStringLiteral("typo")));
    QVERIFY(!canvasbenchmark::parseCandidate(QString()));
}

QTEST_APPLESS_MAIN(CanvasCandidateParserTest)
#include "tst_canvascandidateparsertest.moc"
