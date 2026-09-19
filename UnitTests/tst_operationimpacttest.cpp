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
    void imagesOnlyPlanSelectsImageResolution();
    void unmappedCheckForcesFullPlan();
    void emptyTargetedPlanFallsBackToFull();
    void standardsConvertRequiresOracle();
    void registeredOperationsDeclareImpact();
    void targetedMatchesFullOnImageProfile();
    void goldenCorpusTargetedMatchesFullAndReportsReuse();
    void targetedMatchesFullOnMultiCheckImageProfile();
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

void OperationImpactTest::emptyTargetedPlanFallsBackToFull()
{
    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("embedded-fonts") });
    QVERIFY(plan.full);
    QCOMPARE(plan.reason, QStringLiteral("no-targeted-checks"));
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("embedded-fonts") });
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
        QVERIFY2(impact.declared, qPrintable(QStringLiteral("Operation '%1' has no impact declaration.").arg(id)));
        const bool hasTargetScope = impact.allPages ||
                                    impact.documentWide ||
                                    !impact.pages.isEmpty() ||
                                    !impact.objectIds.isEmpty();
        QVERIFY2(hasTargetScope,
                 qPrintable(QStringLiteral("Operation '%1' has no declared target scope.").arg(id)));
        QVERIFY(impact.toJson().contains(QStringLiteral("impact_complete")));
    }
}

void OperationImpactTest::targetedMatchesFullOnImageProfile()
{
    pdf::PDFDocument document = buildLowDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const pdf::PreflightResult full = engine.run(imageProfile());

    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution") });
    QVERIFY(!plan.full);
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("image-resolution") });

    const pdf::PreflightResult targeted = engine.run(imageProfile(), plan);
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

void OperationImpactTest::targetedMatchesFullOnMultiCheckImageProfile()
{
    pdf::PDFDocument document = buildLowDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile = multiCheckImageProfile();
    const pdf::PreflightResult full = engine.run(profile);

    pdf::PDFOperationImpact impact;
    impact.declared = true;
    impact.domains = pdf::PDFEvidenceDomain::Images;
    impact.impactComplete = true;
    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(impact, { QStringLiteral("image-resolution"), QStringLiteral("embedded-fonts") });
    QVERIFY(!plan.full);
    QCOMPARE(plan.checkIds, QStringList{ QStringLiteral("image-resolution") });

    const pdf::PreflightResult targeted = engine.run(profile, plan);
    QCOMPARE(pdf::reducePreflightVerdict(targeted).state, pdf::reducePreflightVerdict(full).state);
    QCOMPARE(targeted.errors.size(), full.errors.size());
    for (const pdf::PreflightFinding& error : targeted.errors)
    {
        QCOMPARE(error.checkId, QStringLiteral("image-resolution"));
    }
}

QTEST_APPLESS_MAIN(OperationImpactTest)
#include "tst_operationimpacttest.moc"
