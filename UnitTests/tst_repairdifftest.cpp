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
#include "pdfrepairdiff.h"

#include <QJsonDocument>
#include <QtTest>

class RepairDiffTest : public QObject
{
    Q_OBJECT

private slots:
    void identicalDocuments_haveEmptyStructuralDiff();
    void pageBoxChange_isClassifiedByExpectedChangeSet();
    void reportJson_isStableAcrossRepeatedRuns();
};

void RepairDiffTest::identicalDocuments_haveEmptyStructuralDiff()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument document = builder.build();

    pdf::PDFRepairDiffOptions options;
    options.renderVisualDiff = false;
    pdf::PDFRepairDiffReport report;
    QVERIFY(pdf::PDFRepairDiffEngine::compare(document, document, options, &report));
    QVERIFY(report.structuralChanges.isEmpty());
    QCOMPARE(report.status, pdf::PDFRepairDiffStatus::Complete);
}

void RepairDiffTest::pageBoxChange_isClassifiedByExpectedChangeSet()
{
    pdf::PDFDocumentBuilder beforeBuilder;
    beforeBuilder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument before = beforeBuilder.build();

    pdf::PDFDocumentBuilder afterBuilder;
    afterBuilder.appendPage(QRectF(0, 0, 110, 100));
    const pdf::PDFDocument after = afterBuilder.build();

    pdf::PDFRepairDiffOptions options;
    options.renderVisualDiff = false;
    options.expected.pageBoxes = true;
    pdf::PDFRepairDiffReport report;
    QVERIFY(pdf::PDFRepairDiffEngine::compare(before, after, options, &report));
    QCOMPARE(report.structuralChanges.size(), 1);
    QCOMPARE(report.structuralChanges.front().classification, pdf::PDFRepairChangeClass::Expected);
}

void RepairDiffTest::reportJson_isStableAcrossRepeatedRuns()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument document = builder.build();

    pdf::PDFRepairDiffOptions options;
    options.renderVisualDiff = false;
    pdf::PDFRepairDiffReport first;
    pdf::PDFRepairDiffReport second;
    QVERIFY(pdf::PDFRepairDiffEngine::compare(document, document, options, &first));
    QVERIFY(pdf::PDFRepairDiffEngine::compare(document, document, options, &second));
    QCOMPARE(QJsonDocument(first.toJson()).toJson(QJsonDocument::Compact),
             QJsonDocument(second.toJson()).toJson(QJsonDocument::Compact));
}

QTEST_GUILESS_MAIN(RepairDiffTest)

#include "tst_repairdifftest.moc"
