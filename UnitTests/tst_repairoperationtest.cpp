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
#include "pdfrepairoperation.h"

#include <QJsonDocument>
#include <QtTest>

namespace
{

class FailingRepair final : public pdf::PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("test-failing"); }
    pdf::PDFRepairRisk risk() const override { return pdf::PDFRepairRisk::Low; }
    pdf::PDFRepairDomains domains() const override { return pdf::PDFRepairDomain::Metadata; }

    pdf::PDFOperationResult analyze(const pdf::PDFDocument&, const QJsonObject&, pdf::PDFRepairPlan* plan) const override
    {
        plan->operationId = id();
        plan->expectedChanges.metadata = true;
        return pdf::PDFOperationResult(true);
    }

    pdf::PDFOperationResult apply(pdf::PDFDocument*, const pdf::PDFRepairPlan&, pdf::PDFRepairResult*) const override
    {
        return pdf::PDFOperationResult(QStringLiteral("intentional test failure"));
    }
};

} // namespace

class RepairOperationTest : public QObject
{
    Q_OBJECT

private slots:
    void builtInOperations_areRegistered();
    void analyze_doesNotMutateSource();
    void unsupportedPrecondition_preventsApply();
    void failedOperation_discardsCandidate();
};

void RepairOperationTest::builtInOperations_areRegistered()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    QVERIFY(registry.find(QStringLiteral("add-bleed")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("downsample-images")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("rgb-to-cmyk")) != nullptr);
    QVERIFY(registry.descriptors().size() >= 3);
}

void RepairOperationTest::analyze_doesNotMutateSource()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    const qreal originalWidth = source.getCatalog()->getPage(0)->getMediaBox().width();

    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")), QJsonObject{
        { QStringLiteral("bleed_mm"), 3.0 },
        { QStringLiteral("force"), true }
    }));
    QVERIFY(transaction.analyze());
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), originalWidth);
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Planned);
    QVERIFY(!transaction.plans().front().targets.isEmpty());

    const QByteArray first = QJsonDocument(transaction.plans().front().toJson()).toJson(QJsonDocument::Compact);
    pdf::PDFRepairTransaction second(source);
    QVERIFY(second.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")), QJsonObject{
        { QStringLiteral("bleed_mm"), 3.0 },
        { QStringLiteral("force"), true }
    }));
    QVERIFY(second.analyze());
    QCOMPARE(first, QJsonDocument(second.plans().front().toJson()).toJson(QJsonDocument::Compact));
}

void RepairOperationTest::unsupportedPrecondition_preventsApply()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();

    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("rgb-to-cmyk")), QJsonObject()));
    QVERIFY(transaction.analyze());
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Unsupported);
    QVERIFY(!transaction.apply());
    QVERIFY(transaction.candidate() == nullptr);
}

void RepairOperationTest::failedOperation_discardsCandidate()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();

    FailingRepair operation;
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(&operation, QJsonObject()));
    QVERIFY(transaction.analyze());
    QVERIFY(!transaction.apply());
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Failed);
    QVERIFY(transaction.candidate() == nullptr);
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
}

QTEST_GUILESS_MAIN(RepairOperationTest)

#include "tst_repairoperationtest.moc"
