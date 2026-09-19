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

class OperationImpactTest : public QObject
{
    Q_OBJECT

private slots:
    void incompleteImpactSelectsFullRevalidation();
    void imagesOnlyPlanSelectsImageResolution();
    void unmappedCheckForcesFullPlan();
    void unaffectedChecksAreMarkedReusable();
    void fullRewriteSelectsFullRevalidation();
    void standardsConvertRequiresOracle();
    void registeredOperationsDeclareImpact();
    void targetedWithoutBaselineIsIncomplete();
    void targetedMatchesFullOnImageProfile();
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
    impact.impactComplete = false;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });
    QVERIFY(plan.full);
    QCOMPARE(plan.checkIds.size(), 2);
}

void OperationImpactTest::imagesOnlyPlanSelectsImageResolution()
{
    pdf::PDFOperationImpact impact;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });
    QVERIFY(!plan.full);
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("image-resolution") });
}

void OperationImpactTest::unmappedCheckForcesFullPlan()
{
    pdf::PDFOperationImpact impact;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution"), QStringLiteral("bleed") });
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("unmapped-check"));
}

void OperationImpactTest::unaffectedChecksAreMarkedReusable()
{
    pdf::PDFOperationImpact impact;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("embedded-fonts") });
    QVERIFY(!plan.full);
    QVERIFY(plan.checkIds.isEmpty());
    QCOMPARE(plan.reusedCheckIds, QStringList{ QStringLiteral("embedded-fonts") });
    QVERIFY(plan.invalidatedEvidenceDomains.testFlag(pdf::PDFEvidenceDomain::Images));
    QVERIFY(plan.reusableEvidenceDomains.testFlag(pdf::PDFEvidenceDomain::Fonts));
}

void OperationImpactTest::fullRewriteSelectsFullRevalidation()
{
    pdf::PDFOperationImpact impact;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    impact.fullRewrite = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution") });
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("full-rewrite"));
    QCOMPARE(plan.invalidatedEvidenceDomains, pdf::pdfEvidenceAllDomains());
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
        QVERIFY2(impact.impactComplete || impact.requiresIndependentOracle,
                 qPrintable(QStringLiteral("Operation '%1' inherits an undeclared/incomplete impact.").arg(id)));
        if (impact.impactComplete && impact.mutatesDocument)
        {
            QVERIFY2(impact.documentWide || impact.domains != pdf::PDFEvidenceDomains() ||
                         !impact.pages.isEmpty() || !impact.objectIds.isEmpty(),
                     qPrintable(QStringLiteral("Operation '%1' declares mutation without an impact scope.").arg(id)));
        }
        QVERIFY(impact.toJson().contains(QStringLiteral("mutates_document")));
    }
}

void OperationImpactTest::targetedWithoutBaselineIsIncomplete()
{
    pdf::PDFDocument document = buildLowDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    pdf::PDFOperationImpact impact;
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
                                        << int(pdf::PDFEvidenceDomain::Images);
    QTest::newRow("reuse-image-failure") << QStringLiteral("image-dpi-low.pdf")
                                         << int(pdf::PDFEvidenceDomain::Fonts);
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
    impact.domains = pdf::PDFEvidenceDomain(affectedDomain);
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
