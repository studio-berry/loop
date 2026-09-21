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

#include "pdfproductiongeometry.h"
#include "pdfdocumentbuilder.h"
#include "pdfrepairoperation.h"
#include "preflightengine.h"

#include <QJsonDocument>
#include <QtTest>

#include <algorithm>

using namespace pdf;

class ProductionGeometryTest final : public QObject
{
    Q_OBJECT

private slots:
    void roundTripPreservesProcessingSemantics();
    void normalizesProcessingStepTypes();
    void processingStepChecksAreRegistered();
    void rejectsSelfIntersectingContours();
    void plansDeterministicContourBleedAndGrommets();
    void productionOperationsAreRegistered();
    void detectProcessingSteps_findsDeviceNCutContourStroke();
    void detectProcessingSteps_ignoresFillOnlyPathsUsingStaleStrokeColorSpace();
    void detectProcessingSteps_ignoresDeviceNCutContourWhenTintIsZero();
};

namespace
{

pdf::PDFObjectReference addType2TintFunction(pdf::PDFDocumentBuilder* builder, std::initializer_list<double> c1)
{
    pdf::PDFArray c1Array;
    for (double value : c1)
    {
        c1Array.appendItem(pdf::PDFObject::createReal(value));
    }

    pdf::PDFDictionary dictionary;
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("FunctionType"), pdf::PDFObject::createInteger(2));
    pdf::PDFArray domain;
    domain.appendItem(pdf::PDFObject::createReal(0.0));
    domain.appendItem(pdf::PDFObject::createReal(1.0));
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("Domain"), pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(domain)));
    pdf::PDFArray c0;
    for (int i = 0; i < 4; ++i)
    {
        c0.appendItem(pdf::PDFObject::createReal(0.0));
    }
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("C0"), pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(c0)));
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("C1"), pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(qMove(c1Array))));
    dictionary.addEntry(pdf::PDFInplaceOrMemoryString("N"), pdf::PDFObject::createReal(1.0));
    return builder->addObject(pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(qMove(dictionary))));
}

pdf::PDFObject createDeviceNColorSpace(const QStringList& colorantNames, pdf::PDFObjectReference tintFunction)
{
    pdf::PDFArray colorants;
    for (const QString& colorantName : colorantNames)
    {
        colorants.appendItem(pdf::PDFObject::createName(colorantName.toLatin1()));
    }

    pdf::PDFArray array;
    array.appendItem(pdf::PDFObject::createName("DeviceN"));
    array.appendItem(pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(qMove(colorants))));
    array.appendItem(pdf::PDFObject::createName("DeviceCMYK"));
    array.appendItem(pdf::PDFObject::createReference(tintFunction));
    return pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(qMove(array)));
}

pdf::PDFDocument buildDeviceNContentDocument(const QByteArray& content,
                                             const QString& colorSpaceName,
                                             const QStringList& colorantNames,
                                             std::initializer_list<double> tintFunctionC1)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0.0, 0.0, 200.0, 200.0));
    const pdf::PDFObjectReference tintFunction = addType2TintFunction(&builder, tintFunctionC1);

    pdf::PDFDictionary colorSpaces;
    colorSpaces.addEntry(pdf::PDFInplaceOrMemoryString(colorSpaceName.toLatin1()),
                         createDeviceNColorSpace(colorantNames, tintFunction));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(qMove(colorSpaces))));

    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(content.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(qMove(contentDictionary), QByteArray(content))));

    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(qMove(resources))));
    builder.mergeTo(pageReference,
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(qMove(pageUpdate))));
    return builder.build();
}

pdf::PDFDocument buildDeviceNCutContourDocument()
{
    return buildDeviceNContentDocument(QByteArrayLiteral("q /CutContourCS CS 1 SCN 10 10 180 180 re S Q\n"),
                                       QStringLiteral("CutContourCS"),
                                       { QStringLiteral("CutContour") },
                                       { 1.0, 0.0, 0.0, 0.0 });
}

pdf::PDFDocument buildDeviceNCutContourStrokeThenCmykFillDocument()
{
    return buildDeviceNContentDocument(
        QByteArrayLiteral("q /CutContourCS CS 1 SCN 10 10 80 80 re S /DeviceCMYK cs 0 1 0 0 scn 100 100 80 80 re f Q\n"),
        QStringLiteral("CutContourCS"),
        { QStringLiteral("CutContour") },
        { 1.0, 0.0, 0.0, 0.0 });
}

pdf::PDFDocument buildDeviceNCyanOnlyDocument()
{
    return buildDeviceNContentDocument(QByteArrayLiteral("q /SpotCS CS 0 1 SCN 10 10 180 180 re S Q\n"),
                                       QStringLiteral("SpotCS"),
                                       { QStringLiteral("CutContour"), QStringLiteral("Cyan") },
                                       { 1.0, 0.0, 0.0, 0.0 });
}

const pdf::PDFProcessingStep* findLegacyCutContourStep(const QList<pdf::PDFProcessingStep>& steps)
{
    const auto it = std::find_if(steps.cbegin(), steps.cend(), [](const pdf::PDFProcessingStep& step)
                                 { return step.spotColorName == QStringLiteral("CutContour") && step.detectionMethod == QStringLiteral("legacy-spot-color"); });
    return it == steps.cend() ? nullptr : &(*it);
}

}

void ProductionGeometryTest::roundTripPreservesProcessingSemantics()
{
    PDFProductionGeometryModel model;
    PDFProductionContour contour;
    contour.id = QStringLiteral("cut-1");
    contour.pageIndex = 0;
    contour.path.addRect(QRectF(10.0, 20.0, 100.0, 50.0));
    contour.sourceEvidence = QStringLiteral("explicit-selection");
    model.contours.append(contour);
    model.processingSteps.append({ QStringLiteral("step-cut"), PDFProcessingStepKind::Cut, QStringLiteral("Cut contour"), QStringLiteral("CutContour"), false, true, { { QStringLiteral("vendor"), QStringLiteral("test") } } });

    const PDFProductionGeometryModel reopened = PDFProductionGeometryModel::fromJson(model.toJson());
    QCOMPARE(reopened.schemaVersion, PDFProductionGeometrySchemaVersion);
    QCOMPARE(reopened.contours.size(), 1);
    QCOMPARE(reopened.processingSteps.front().kind, PDFProcessingStepKind::Cut);
    QCOMPARE(reopened.processingSteps.front().spotColorName, QStringLiteral("CutContour"));
    QCOMPARE(reopened.contours.front().sourceEvidence, QStringLiteral("explicit-selection"));
}

void ProductionGeometryTest::normalizesProcessingStepTypes()
{
    QCOMPARE(pdfProcessingStepTypeFromString(QStringLiteral("Cutting")), PDFProcessingStepType::CuttingDie);
    QCOMPARE(pdfProcessingStepTypeFromString(QStringLiteral("perforating-cut")), PDFProcessingStepType::PerforatingCut);
    QCOMPARE(pdfProcessingStepTypeFromString(QStringLiteral("Creasing")), PDFProcessingStepType::CreasingBend);
    QCOMPARE(pdfProcessingStepTypeFromString(QStringLiteral("foreground_varnish")), PDFProcessingStepType::ForegroundVarnish);
    QCOMPARE(pdfProcessingStepTypeFromString(QStringLiteral("not-a-step")), PDFProcessingStepType::Unknown);
}

void ProductionGeometryTest::processingStepChecksAreRegistered()
{
    PreflightEngine engine(nullptr);
    QVERIFY(engine.hasCheck(QStringLiteral("processing-steps")));
    QVERIFY(engine.hasCheck(QStringLiteral("dieline")));
}

void ProductionGeometryTest::rejectsSelfIntersectingContours()
{
    PDFProductionContour contour;
    contour.id = QStringLiteral("bad");
    contour.path.moveTo(0.0, 0.0);
    contour.path.lineTo(100.0, 100.0);
    contour.path.lineTo(0.0, 100.0);
    contour.path.lineTo(100.0, 0.0);
    contour.path.closeSubpath();

    PDFProductionGeometryModel model;
    model.contours.append(contour);
    const PDFProductionValidationReport report = validateProductionGeometry(model);
    QVERIFY(!report.valid);
    QVERIFY(std::any_of(report.diagnostics.cbegin(), report.diagnostics.cend(), [](const PDFProductionDiagnostic& diagnostic)
                        { return diagnostic.id == QStringLiteral("production.contour.self_intersection"); }));
}

void ProductionGeometryTest::plansDeterministicContourBleedAndGrommets()
{
    PDFProductionContour contour;
    contour.id = QStringLiteral("cut");
    contour.path.addRect(QRectF(0.0, 0.0, 720.0, 360.0));
    const PDFContourBleedPlan bleed = planContourBleed(contour, { 9.0, 0.1, 1000 });
    QVERIFY(bleed.valid);
    QVERIFY(bleed.bleedBounds.width() > contour.path.boundingRect().width());

    PDFGrommetSpec spec;
    spec.targetSpacingPt = 240.0;
    const PDFGrommetPlacementReport first = placeGrommets(QRectF(0.0, 0.0, 720.0, 360.0), spec);
    const PDFGrommetPlacementReport second = placeGrommets(QRectF(0.0, 0.0, 720.0, 360.0), spec);
    QCOMPARE(first.points, second.points);
    QVERIFY(!first.points.isEmpty());
    QVERIFY(first.diagnostics.isEmpty());
}

void ProductionGeometryTest::productionOperationsAreRegistered()
{
    const PDFRepairRegistry& registry = PDFRepairRegistry::instance();
    QVERIFY(registry.find(QStringLiteral("production.validate-wide-format")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("production.add-contour-bleed")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("production.place-grommets")) != nullptr);
}

void ProductionGeometryTest::detectProcessingSteps_findsDeviceNCutContourStroke()
{
    const pdf::PDFDocument document = buildDeviceNCutContourDocument();
    const QList<pdf::PDFProcessingStep> steps = detectProcessingSteps(document);
    const pdf::PDFProcessingStep* cutContourStep = findLegacyCutContourStep(steps);
    QVERIFY(cutContourStep != nullptr);
    QCOMPARE(cutContourStep->kind, pdf::PDFProcessingStepKind::Cut);
    QVERIFY(!cutContourStep->geometry.isEmpty());
    QCOMPARE(cutContourStep->pageIndices, QVector<int>({ 0 }));
}

void ProductionGeometryTest::detectProcessingSteps_ignoresFillOnlyPathsUsingStaleStrokeColorSpace()
{
    const pdf::PDFDocument document = buildDeviceNCutContourStrokeThenCmykFillDocument();
    const QList<pdf::PDFProcessingStep> steps = detectProcessingSteps(document);
    const pdf::PDFProcessingStep* cutContourStep = findLegacyCutContourStep(steps);
    QVERIFY(cutContourStep != nullptr);
    const QRectF bounds = cutContourStep->geometry.boundingRect();
    QVERIFY(bounds.contains(QPointF(10.0, 10.0)));
    QVERIFY(bounds.contains(QPointF(90.0, 90.0)));
    QVERIFY(!bounds.contains(QPointF(100.0, 100.0)));
    QVERIFY(!bounds.contains(QPointF(180.0, 180.0)));
}

void ProductionGeometryTest::detectProcessingSteps_ignoresDeviceNCutContourWhenTintIsZero()
{
    const pdf::PDFDocument document = buildDeviceNCyanOnlyDocument();
    const QList<pdf::PDFProcessingStep> steps = detectProcessingSteps(document);
    QVERIFY(findLegacyCutContourStep(steps) == nullptr);
}

QTEST_MAIN(ProductionGeometryTest)
#include "tst_productiongeometrytest.moc"
