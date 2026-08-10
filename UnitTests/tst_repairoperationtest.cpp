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
#include "pdfstandardconversion.h"

#include <QJsonDocument>
#include <QJsonValue>
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
    void builtInOperations_declareSavePolicies();
    void analyze_doesNotMutateSource();
    void unsupportedPrecondition_preventsApply();
    void failedOperation_discardsCandidate();
    void standardTargets_areExplicitAndStable();
};

void RepairOperationTest::builtInOperations_areRegistered()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    QVERIFY(registry.find(QStringLiteral("add-bleed")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("downsample-images")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("rgb-to-cmyk")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("standards-convert")) != nullptr);
    QVERIFY(registry.descriptors().size() >= 4);
    for (const QJsonValue& descriptorValue : registry.descriptors())
    {
        const QJsonObject descriptor = descriptorValue.toObject();
        QVERIFY(descriptor.contains(QStringLiteral("preflight_fixup")));
        QVERIFY(descriptor.contains(QStringLiteral("save_policy")));
        const QJsonObject savePolicy = descriptor.value(QStringLiteral("save_policy")).toObject();
        QVERIFY(savePolicy.contains(QStringLiteral("mode")));
        QVERIFY(savePolicy.contains(QStringLiteral("invalidates_signatures")));
        QVERIFY(savePolicy.contains(QStringLiteral("reversible_in_session")));
        if (descriptor.value(QStringLiteral("id")).toString() == QStringLiteral("add-bleed")
            || descriptor.value(QStringLiteral("id")).toString() == QStringLiteral("downsample-images")
            || descriptor.value(QStringLiteral("id")).toString() == QStringLiteral("rgb-to-cmyk"))
        {
            QVERIFY(descriptor.value(QStringLiteral("preflight_fixup")).toBool());
        }
    }
}

void RepairOperationTest::builtInOperations_declareSavePolicies()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    const auto policyMode = [&registry](const QString& id) {
        return registry.find(id)->descriptor().value(QStringLiteral("save_policy")).toObject().value(QStringLiteral("mode")).toString();
    };

    QCOMPARE(policyMode(QStringLiteral("add-bleed")), QStringLiteral("save-as-new-artifact"));
    QCOMPARE(policyMode(QStringLiteral("downsample-images")), QStringLiteral("full-rewrite"));
    QCOMPARE(policyMode(QStringLiteral("rgb-to-cmyk")), QStringLiteral("save-as-new-artifact"));
    QCOMPARE(policyMode(QStringLiteral("production.validate-wide-format")), QStringLiteral("incremental-append"));
    QCOMPARE(policyMode(QStringLiteral("production.add-contour-bleed")), QStringLiteral("save-as-new-artifact"));
    QCOMPARE(policyMode(QStringLiteral("production.place-grommets")), QStringLiteral("incremental-append"));

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(registry.find(QStringLiteral("add-bleed")), QJsonObject{
        { QStringLiteral("bleed_mm"), 3.0 },
        { QStringLiteral("force"), true }
    }));
    QCOMPARE(transaction.savePolicy().mode, pdf::PDFSaveMode::SaveAsNewArtifact);
    QVERIFY(transaction.savePolicy().invalidatesSignatures);
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

void RepairOperationTest::standardTargets_areExplicitAndStable()
{
    QCOMPARE(pdf::supportedPDFStandardTargets(), QStringList{
        QStringLiteral("PDF/X-1a:2001"), QStringLiteral("PDF/X-3:2002"),
        QStringLiteral("PDF/X-4"), QStringLiteral("PDF/A-2b") });
    pdf::PDFStandardTarget target = pdf::PDFStandardTarget::PDFX4;
    QVERIFY(pdf::pdfStandardTargetFromString(QStringLiteral("PDF/X-3:2002"), &target));
    QCOMPARE(target, pdf::PDFStandardTarget::PDFX3_2002);
    QCOMPARE(pdf::pdfStandardTargetToString(pdf::PDFStandardTarget::PDFA2b), QStringLiteral("PDF/A-2b"));
}

QTEST_GUILESS_MAIN(RepairOperationTest)

#include "tst_repairoperationtest.moc"
