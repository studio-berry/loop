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

#include "pdfoperationimpact.h"
#include "pdfrepairoperation.h"

#include <QtTest>

class OperationImpactTest : public QObject
{
    Q_OBJECT

private slots:
    void incompleteImpactRequiresFullRevalidation();
    void mergeUnionsDomainsAndKeepsIncomplete();
    void registeredOperationsDeclareImpact();
};

void OperationImpactTest::incompleteImpactRequiresFullRevalidation()
{
    pdf::PDFOperationImpact impact;
    QVERIFY(impact.requiresFullRevalidation());
    impact.impactComplete = true;
    QVERIFY(!impact.requiresFullRevalidation());
    impact.documentWide = true;
    QVERIFY(impact.requiresFullRevalidation());
}

void OperationImpactTest::mergeUnionsDomainsAndKeepsIncomplete()
{
    pdf::PDFOperationImpact first;
    first.domains = pdf::PDFEvidenceDomain::Images;
    first.impactComplete = true;
    first.pages = {1};
    pdf::PDFOperationImpact second;
    second.domains = pdf::PDFEvidenceDomain::Fonts;
    second.impactComplete = false;
    second.pages = {2};
    const pdf::PDFOperationImpact merged = pdf::mergePDFOperationImpact(first, second);
    QVERIFY(merged.domains.testFlag(pdf::PDFEvidenceDomain::Images));
    QVERIFY(merged.domains.testFlag(pdf::PDFEvidenceDomain::Fonts));
    QVERIFY(merged.requiresFullRevalidation());
    QCOMPARE(merged.pages.size(), 2);
}

void OperationImpactTest::registeredOperationsDeclareImpact()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    QVERIFY(!registry.operationIds().isEmpty());
    for (const QString& operationId : registry.operationIds())
    {
        const pdf::PDFRepairOperation* operation = registry.find(operationId);
        QVERIFY(operation);
        const pdf::PDFOperationImpact impact = operation->impact();
        QVERIFY(impact.requiresFullRevalidation() || impact.impactComplete);
    }
}

QTEST_APPLESS_MAIN(OperationImpactTest)
#include "tst_operationimpacttest.moc"
