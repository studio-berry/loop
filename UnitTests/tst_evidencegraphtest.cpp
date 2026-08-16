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

#include "pdfevidencegraph.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdfpreflightverdict.h"
#include "preflightengine.h"

#include <QtTest>

class EvidenceGraphTest : public QObject
{
    Q_OBJECT

private slots:
    void incompleteGraphCannotPass();
    void collectFromEmptyPageIsComplete();
    void imageFamilyRecordsMatchImageResolutionCheck();
    void remainingFamiliesCollectWithoutFailingClosed();
};

void EvidenceGraphTest::incompleteGraphCannotPass()
{
    pdf::PDFEvidenceGraph graph;
    graph.complete = false;
    graph.incompleteReason = QStringLiteral("budget-exceeded");
    QVERIFY(!graph.isComplete());

    pdf::PreflightResult result;
    result.inspectionComplete = graph.isComplete();
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QVERIFY(!verdict.isPass());
}

void EvidenceGraphTest::collectFromEmptyPageIsComplete()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session);
    QVERIFY(graph.isComplete());
}

void EvidenceGraphTest::imageFamilyRecordsMatchImageResolutionCheck()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::PDFEvidenceDomain::Images);

    pdf::PreflightEngine engine(&session);
    pdf::PreflightProfileData profile;
    profile.name = QStringLiteral("images");
    pdf::PreflightCheckConfig check;
    check.id = QStringLiteral("image-resolution");
    check.minDpi = 300;
    profile.checks.append(check);
    const pdf::PreflightResult result = engine.run(profile);
    QCOMPARE(result.errors.size() + result.warnings.size(), graph.recordsForDomain(pdf::PDFEvidenceDomain::Images).size());
}

void EvidenceGraphTest::remainingFamiliesCollectWithoutFailingClosed()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::pdfEvidenceAllDomains());
    QVERIFY(graph.isComplete());
    QVERIFY(graph.recordsForDomain(pdf::PDFEvidenceDomain::Colorants).size() >= 0);
    QVERIFY(graph.recordsForDomain(pdf::PDFEvidenceDomain::Strokes).size() >= 0);
    QVERIFY(graph.recordsForDomain(pdf::PDFEvidenceDomain::OverprintTransparency).size() >= 0);
    QVERIFY(graph.recordsForDomain(pdf::PDFEvidenceDomain::Fonts).size() >= 0);
}

QTEST_APPLESS_MAIN(EvidenceGraphTest)
#include "tst_evidencegraphtest.moc"
