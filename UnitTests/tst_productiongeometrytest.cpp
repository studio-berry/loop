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
#include "pdfrepairoperation.h"

#include <QJsonDocument>
#include <QtTest>

#include <algorithm>

using namespace pdf;

class ProductionGeometryTest final : public QObject
{
    Q_OBJECT

private slots:
    void roundTripPreservesProcessingSemantics();
    void rejectsSelfIntersectingContours();
    void plansDeterministicContourBleedAndGrommets();
    void productionOperationsAreRegistered();
};

void ProductionGeometryTest::roundTripPreservesProcessingSemantics()
{
    PDFProductionGeometryModel model;
    PDFProductionContour contour;
    contour.id = QStringLiteral("cut-1");
    contour.pageIndex = 0;
    contour.path.addRect(QRectF(10.0, 20.0, 100.0, 50.0));
    contour.sourceEvidence = QStringLiteral("explicit-selection");
    model.contours.append(contour);
    model.processingSteps.append({ QStringLiteral("step-cut"), PDFProcessingStepKind::Cut,
                                   QStringLiteral("Cut contour"), QStringLiteral("CutContour"), false, true,
                                   { { QStringLiteral("vendor"), QStringLiteral("test") } } });

    const PDFProductionGeometryModel reopened = PDFProductionGeometryModel::fromJson(model.toJson());
    QCOMPARE(reopened.schemaVersion, PDFProductionGeometrySchemaVersion);
    QCOMPARE(reopened.contours.size(), 1);
    QCOMPARE(reopened.processingSteps.front().kind, PDFProcessingStepKind::Cut);
    QCOMPARE(reopened.processingSteps.front().spotColorName, QStringLiteral("CutContour"));
    QCOMPARE(reopened.contours.front().sourceEvidence, QStringLiteral("explicit-selection"));
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
    {
        return diagnostic.id == QStringLiteral("production.contour.self_intersection");
    }));
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

QTEST_MAIN(ProductionGeometryTest)
#include "tst_productiongeometrytest.moc"
