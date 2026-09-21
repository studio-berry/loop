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

#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentsession.h"
#include "pdfimage.h"
#include "pdfoperationimpact.h"
#include "pdfpreflightverdict.h"
#include "pdfrepairoperation.h"
#include "preflightengine.h"

#include <QtTest>
#include <QFile>

#include <algorithm>

class OperationImpactTest : public QObject
{
    Q_OBJECT

private slots:
    void incompleteImpactSelectsFullRevalidation();
    void fullRewriteSelectsFullRevalidation();
    void documentPolicySelectsFullRevalidation();
    void fullRewriteAndDocumentWideImpactsDoNotAdvertiseNarrowedPages();
    void stepPlannerCannotNarrowDocumentWideOrOracleImpactWithPageTargets();
    void imagesOnlyPlanSelectsImageResolution();
    void unmappedCheckForcesFullPlan();
    void unaffectedChecksAreMarkedReusable();
    void standardsConvertRequiresOracle();
    void registeredOperationsDeclareImpact();
    void targetedWithoutBaselineIsIncomplete();
    void targetedMatchesFullOnImageProfile();
    void goldenCorpusTargetedMatchesFullAndReportsReuse();
    void targetedReusesBaselineFindings();
    void goldenFixtureSubsetMatchesFull_data();
    void goldenFixtureSubsetMatchesFull();
};

namespace
{

pdf::PDFDocument buildLowDpiImagePage()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));
    QImage image(10, 10, QImage::Format_ARGB32);
    image.fill(qRgb(32, 32, 32));
    pdf::PDFImage::ImageEncodeOptions imageOptions;
    imageOptions.compression = pdf::PDFImage::ImageCompression::Flate;
    const pdf::PDFObjectReference imageReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFImage::createStreamFromImage(image, imageOptions))));
    QByteArray content("q 144 0 0 144 0 0 cm /Im1 Do Q");
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(content.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFStream(std::move(contentDictionary), std::move(content)))));
    pdf::PDFDictionary xObject;
    xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(pageReference, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    return builder.build();
}

QJsonObject imageProfile()
{
    return QJsonObject{
        { QStringLiteral("name"), QStringLiteral("Images only") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 300 },
                                        { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };
}

pdf::PDFDocument loadGoldenFixture(const QString& fileName)
{
    const QString path = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/") + fileName;
    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(path);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        qFatal("Failed to load golden fixture '%s'", qPrintable(fileName));
    }
    return document;
}

QJsonObject multiCheckImageProfile()
{
    return QJsonObject{
        { QStringLiteral("name"), QStringLiteral("Images and fonts") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                            { QStringLiteral("min_dpi"), 300 },
                                            { QStringLiteral("severity"), QStringLiteral("error") } },
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("embedded-fonts") },
                                            { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };
}

}   // namespace

void OperationImpactTest::incompleteImpactSelectsFullRevalidation()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.impactComplete = false;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });
    QVERIFY(plan.full);
    QCOMPARE(plan.checkIds.size(), 2);
}

void OperationImpactTest::fullRewriteSelectsFullRevalidation()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.allPages = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.fullRewrite = true;
    impact.impactComplete = true;

    const pdf::PDFRevalidationPlan plan =
        pdf::planRevalidation(impact, { QStringLiteral("image-resolution") });
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("full-rewrite"));
    QVERIFY(!plan.reusePriorEvidence);
    QVERIFY(plan.invalidatedEvidenceDomains == pdf::pdfEvidenceAllDomains());
}

void OperationImpactTest::documentPolicySelectsFullRevalidation()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.allPages = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;

    const pdf::PDFRevalidationPlan plan =
        pdf::planRevalidation(impact, { QStringLiteral("image-resolution") }, true);
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("document-policy"));
}

void OperationImpactTest::fullRewriteAndDocumentWideImpactsDoNotAdvertiseNarrowedPages()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.mutatesDocument = true;
    impact.impactComplete = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.pages = { 0 };
    impact.fullRewrite = true;
    QVERIFY(impact.isFullRevalidation());
    const QStringList checks{ QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") };
    pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, checks);
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("full-rewrite"));
    QCOMPARE(plan.checkIds, checks);
    QVERIFY(plan.pages.isEmpty());

    impact.fullRewrite = false;
    impact.documentWide = true;
    QVERIFY(impact.isFullRevalidation());
    plan = pdf::planRevalidation(impact, checks);
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("document-wide"));
    QVERIFY(plan.pages.isEmpty());

    impact.documentWide = false;
    impact.impactComplete = false;
    plan = pdf::planRevalidation(impact, checks);
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("impact-incomplete"));
    QVERIFY(plan.pages.isEmpty());

    impact.impactComplete = true;
    impact.requiresIndependentOracle = true;
    plan = pdf::planRevalidation(impact, checks);
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("independent-oracle"));
    QVERIFY(plan.pages.isEmpty());
}

void OperationImpactTest::stepPlannerCannotNarrowDocumentWideOrOracleImpactWithPageTargets()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 144, 144));
    builder.appendPage(QRectF(0, 0, 144, 144));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFRepairPlan repair;
    repair.targets.append({ 0, {}, QStringLiteral("page/1/image") });
    const QStringList enabled{ QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") };

    const pdf::PDFRepairOperation* downsample =
        pdf::PDFRepairRegistry::instance().find(QStringLiteral("downsample-images"));
    QVERIFY(downsample);
    const pdf::PDFRevalidationPlan full = pdf::planRepairStepPreflight(
        downsample, document, QJsonObject{ { QStringLiteral("target_dpi"), 150 } },
        enabled, repair);
    QVERIFY(full.full);
    QCOMPARE(full.checkIds, enabled);
    QVERIFY(full.pages.isEmpty());

    const pdf::PDFRepairOperation* standard =
        pdf::PDFRepairRegistry::instance().find(QStringLiteral("standards-convert"));
    QVERIFY(standard);
    const pdf::PDFRevalidationPlan oracle = pdf::planRepairStepPreflight(
        standard, document, QJsonObject{ { QStringLiteral("target"), QStringLiteral("PDF/X-4") } },
        enabled, repair);
    QVERIFY(oracle.full);
    QCOMPARE(oracle.checkIds, enabled);
    QVERIFY(oracle.pages.isEmpty());

    class PageLocalRepair final : public pdf::PDFRepairOperation
    {
    public:
        QString id() const override { return QStringLiteral("test-page-local"); }
        pdf::PDFRepairRisk risk() const override { return pdf::PDFRepairRisk::Low; }
        pdf::PDFRepairDomains domains() const override { return pdf::PDFRepairDomain::Images; }
        pdf::PDFOperationImpact impact(const pdf::PDFDocument*, const QJsonObject&) const override
        {
            pdf::PDFOperationImpact declared;
            declared.declared = true;
            declared.mutatesDocument = true;
            declared.impactComplete = true;
            declared.domains = pdf::PDFEvidenceDomain::Images;
            declared.pages.insert(0);
            return declared;
        }
        pdf::PDFOperationResult analyze(const pdf::PDFDocument&, const QJsonObject&,
                                        pdf::PDFRepairPlan*) const override
        {
            return pdf::PDFOperationResult(true);
        }
        pdf::PDFOperationResult apply(pdf::PDFDocument*, const pdf::PDFRepairPlan&,
                                      pdf::PDFRepairResult*) const override
        {
            return pdf::PDFOperationResult(true);
        }
    } local;

    repair.targets.clear();
    repair.targets.append({ 1, {}, QStringLiteral("page/2/image") });
    const pdf::PDFRevalidationPlan narrowed = pdf::planRepairStepPreflight(
        &local, document, {}, enabled, repair);
    QVERIFY(!narrowed.full);
    QCOMPARE(narrowed.checkIds, QStringList{ QStringLiteral("image-resolution") });
    QCOMPARE(narrowed.pages, (QSet<int>{ 0, 1 }));

    repair.targets.append({ -1, {}, QStringLiteral("document/all") });
    const pdf::PDFRevalidationPlan documentTarget = pdf::planRepairStepPreflight(
        &local, document, {}, enabled, repair);
    QVERIFY(!documentTarget.full);
    QVERIFY(documentTarget.pages.isEmpty());
    QCOMPARE(documentTarget.reason, QStringLiteral("document-target"));
}

void OperationImpactTest::imagesOnlyPlanSelectsImageResolution()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });
    QVERIFY(!plan.full);
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("image-resolution") });
}

void OperationImpactTest::unmappedCheckForcesFullPlan()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution"), QStringLiteral("bleed") });
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("unmapped-check"));
}

void OperationImpactTest::unaffectedChecksAreMarkedReusable()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("embedded-fonts") });
    QVERIFY(!plan.full);
    QVERIFY(plan.checkIds.isEmpty());
    QCOMPARE(plan.reusedCheckIds, QStringList{ QStringLiteral("embedded-fonts") });
    QVERIFY(plan.invalidatedEvidenceDomains.testFlag(pdf::PDFEvidenceDomain::Images));
    QVERIFY(plan.reusableEvidenceDomains.testFlag(pdf::PDFEvidenceDomain::Fonts));
}

void OperationImpactTest::standardsConvertRequiresOracle()
{
    const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(QStringLiteral("standards-convert"));
    QVERIFY(operation);
    const pdf::PDFOperationImpact impact = operation->impact(nullptr, QJsonObject());
    QVERIFY(impact.requiresIndependentOracle);
    QVERIFY(!impact.impactComplete);
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("color-mode") });
    QVERIFY(plan.full);
    QVERIFY(plan.requiresIndependentOracle);
    QCOMPARE(plan.reason, QStringLiteral("independent-oracle"));
}

void OperationImpactTest::registeredOperationsDeclareImpact()
{
    const QStringList ids = pdf::PDFRepairRegistry::instance().operationIds();
    QVERIFY(!ids.isEmpty());
    for (const QString& id : ids)
    {
        const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(id);
        QVERIFY(operation);
        const pdf::PDFOperationImpact impact = operation->impact(nullptr, QJsonObject());
        QVERIFY2(impact.declared || impact.impactComplete || impact.requiresIndependentOracle,
                 qPrintable(QStringLiteral("Operation '%1' has no impact declaration.").arg(id)));
        if (impact.declared)
        {
            const bool hasTargetScope = impact.allPages ||
                                        impact.documentWide ||
                                        !impact.pages.isEmpty() ||
                                        !impact.objectIds.isEmpty() ||
                                        !impact.mutatesDocument;
            QVERIFY2(hasTargetScope,
                     qPrintable(QStringLiteral("Operation '%1' has no declared target scope.").arg(id)));
        }
        if (impact.impactComplete && impact.mutatesDocument)
        {
            QVERIFY2(impact.documentWide || impact.domains != pdf::PDFEvidenceDomains() ||
                         !impact.pages.isEmpty() || !impact.objectIds.isEmpty(),
                     qPrintable(QStringLiteral("Operation '%1' declares mutation without an impact scope.").arg(id)));
        }
        QVERIFY(impact.toJson().contains(QStringLiteral("impact_complete")));
        QVERIFY(impact.toJson().contains(QStringLiteral("mutates_document")));
    }
}

void OperationImpactTest::targetedWithoutBaselineIsIncomplete()
{
    pdf::PDFDocument document = buildLowDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Fonts;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(
        impact,
        { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });

    const pdf::PreflightResult targeted = engine.run(multiCheckImageProfile(), plan);
    QVERIFY(!targeted.inspectionComplete);
    QCOMPARE(targeted.errorCode, QStringLiteral("revalidation-baseline-required"));
}

void OperationImpactTest::targetedMatchesFullOnImageProfile()
{
    pdf::PDFDocument document = buildLowDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profileObject = imageProfile();
    const pdf::PreflightResult full = engine.run(profileObject);

    pdf::PreflightProfileData profile;
    QString errorMessage;
    QVERIFY(pdf::PreflightEngine::parseProfile(profileObject, profile, errorMessage));

    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution") });
    QVERIFY(!plan.full);
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("image-resolution") });

    const pdf::PreflightResult targeted = engine.revalidate(profile, full, plan);
    QVERIFY(targeted.inspectionComplete);
    QCOMPARE(pdf::reducePreflightVerdict(targeted).state, pdf::reducePreflightVerdict(full).state);
    QCOMPARE(targeted.errors.size(), full.errors.size());
}

void OperationImpactTest::goldenCorpusTargetedMatchesFullAndReportsReuse()
{
    // image-dpi-low carries both graph-backed image evidence and a color-mode failure;
    // color-rgb only surfaces colorants in the evidence graph today.
    const QString fixturePath = QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/image-dpi-low.pdf");
    QVERIFY(QFile::exists(fixturePath));

    auto noPassword = [](bool*)
    { return QString(); };
    pdf::PDFDocumentReader reader(nullptr, noPassword, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Impact golden parity") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                            { QStringLiteral("min_dpi"), 300 },
                                            { QStringLiteral("severity"), QStringLiteral("error") } },
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("color-mode") },
                                            { QStringLiteral("allowed"), QJsonArray{ QStringLiteral("CMYK") } },
                                            { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };

    pdf::PreflightProfileData profile;
    QString profileError;
    QVERIFY2(pdf::PreflightEngine::parseProfile(profileObject, profile, profileError),
             qPrintable(profileError));

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const pdf::PreflightResult before = engine.run(profile);
    const pdf::PDFEvidenceGraph beforeEvidence = engine.lastEvidenceGraph();
    const pdf::PreflightResult fullAfter = engine.run(profile);

    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.allPages = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan =
        pdf::planRevalidation(impact,
                              { QStringLiteral("image-resolution"),
                                QStringLiteral("color-mode") });

    QVERIFY(!plan.full);
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("image-resolution") });
    QVERIFY(plan.reusePriorEvidence);

    const pdf::PreflightResult targeted =
        engine.revalidate(profile, plan, before, beforeEvidence);

    QCOMPARE(pdf::reducePreflightVerdict(targeted).state,
             pdf::reducePreflightVerdict(fullAfter).state);

    const auto findingIds = [](const QList<pdf::PreflightFinding>& findings)
    {
        QStringList ids;
        for (const pdf::PreflightFinding& finding : findings)
        {
            ids.append(finding.stableId());
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    QCOMPARE(findingIds(targeted.errors), findingIds(fullAfter.errors));
    QCOMPARE(findingIds(targeted.warnings), findingIds(fullAfter.warnings));

    const QJsonObject provenance = targeted.revalidation;
    QCOMPARE(provenance.value(QStringLiteral("mode")).toString(), QStringLiteral("targeted"));
    const QJsonArray reusedChecks = provenance.value(QStringLiteral("reused_check_ids")).toArray();
    const QJsonArray recomputedChecks = provenance.value(QStringLiteral("recomputed_check_ids")).toArray();
    QVERIFY(reusedChecks.contains(QStringLiteral("color-mode")));
    QVERIFY(recomputedChecks.contains(QStringLiteral("image-resolution")));
    QVERIFY(!provenance.value(QStringLiteral("reused_evidence_ids")).toArray().isEmpty());
    QVERIFY(!provenance.value(QStringLiteral("recomputed_evidence_ids")).toArray().isEmpty());
}

void OperationImpactTest::targetedReusesBaselineFindings()
{
    pdf::PDFDocument document = buildLowDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profileObject = multiCheckImageProfile();
    const pdf::PreflightResult full = engine.run(profileObject);
    QCOMPARE(full.errors.size(), 1);

    pdf::PreflightProfileData profile;
    QString errorMessage;
    QVERIFY(pdf::PreflightEngine::parseProfile(profileObject, profile, errorMessage));

    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Fonts;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(
        impact,
        { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });
    QVERIFY(!plan.full);
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("embedded-fonts") });
    QCOMPARE(plan.reusedCheckIds, QStringList{ QStringLiteral("image-resolution") });

    const pdf::PreflightResult targeted = engine.revalidate(profile, full, plan);
    QVERIFY(targeted.inspectionComplete);
    QCOMPARE(pdf::reducePreflightVerdict(targeted).state, pdf::reducePreflightVerdict(full).state);
    QCOMPARE(targeted.errors.size(), full.errors.size());
    QCOMPARE(targeted.errors.first().checkId, QStringLiteral("image-resolution"));

    const QJsonObject accounting = targeted.toJson().value(QStringLiteral("revalidation")).toObject();
    QCOMPARE(accounting.value(QStringLiteral("mode")).toString(), QStringLiteral("targeted"));
    QVERIFY(accounting.value(QStringLiteral("baseline_reused")).toBool());
    QCOMPARE(accounting.value(QStringLiteral("check_ids")).toArray().first().toString(),
             QStringLiteral("embedded-fonts"));
    QCOMPARE(accounting.value(QStringLiteral("reused_check_ids")).toArray().first().toString(),
             QStringLiteral("image-resolution"));
}


void OperationImpactTest::goldenFixtureSubsetMatchesFull_data()
{
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<int>("affectedDomain");

    QTest::newRow("reuse-font-failure") << QStringLiteral("font-not-embedded.pdf")
                                        << static_cast<int>(pdf::PDFEvidenceDomain::Images);
    QTest::newRow("reuse-image-failure") << QStringLiteral("image-dpi-low.pdf")
                                         << static_cast<int>(pdf::PDFEvidenceDomain::Fonts);
}

void OperationImpactTest::goldenFixtureSubsetMatchesFull()
{
    QFETCH(QString, fixture);
    QFETCH(int, affectedDomain);

    pdf::PDFDocument document = loadGoldenFixture(fixture);
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profileObject = multiCheckImageProfile();
    QJsonArray checks = profileObject.value(QStringLiteral("checks")).toArray();
    QJsonObject imageCheck = checks.at(0).toObject();
    imageCheck.insert(QStringLiteral("severity"), QStringLiteral("error"));
    checks.replace(0, imageCheck);
    profileObject.insert(QStringLiteral("checks"), checks);

    const pdf::PreflightResult full = engine.run(profileObject);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    QVERIFY(pdf::PreflightEngine::parseProfile(profileObject, profile, errorMessage));

    pdf::PDFOperationImpact impact;
    impact.domains = static_cast<pdf::PDFEvidenceDomain>(affectedDomain);
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(
        impact,
        { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });
    const pdf::PreflightResult targeted = engine.revalidate(profile, full, plan);

    QVERIFY(targeted.inspectionComplete);
    QCOMPARE(pdf::reducePreflightVerdict(targeted).state, pdf::reducePreflightVerdict(full).state);
    QCOMPARE(targeted.errors.size(), full.errors.size());
    QCOMPARE(targeted.warnings.size(), full.warnings.size());
}

QTEST_APPLESS_MAIN(OperationImpactTest)
#include "tst_operationimpacttest.moc"
