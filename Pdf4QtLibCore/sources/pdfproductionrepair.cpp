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
#include "pdfcontourbleedfixup.h"
#include "pdfrepairoperation.h"

#include <QJsonArray>

#include <utility>

namespace pdf
{

namespace
{

bool hasErrors(const QVector<PDFProductionDiagnostic>& diagnostics)
{
    for (const PDFProductionDiagnostic& diagnostic : diagnostics)
    {
        if (diagnostic.severity == PDFProductionDiagnosticSeverity::Error)
        {
            return true;
        }
    }
    return false;
}

QStringList diagnosticMessages(const QVector<PDFProductionDiagnostic>& diagnostics)
{
    QStringList messages;
    for (const PDFProductionDiagnostic& diagnostic : diagnostics)
    {
        messages.append(QStringLiteral("%1: %2").arg(diagnostic.id, diagnostic.message));
    }
    return messages;
}

QJsonObject geometrySchema()
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("geometry") } },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("geometry"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("object") } } }
        } }
    };
}

class PDFValidateWideFormatRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("production.validate-wide-format"); }
    PDFRepairRisk risk() const override { return PDFRepairRisk::Low; }
    PDFRepairDomains domains() const override { return PDFRepairDomain::Paths | PDFRepairDomain::Layers | PDFRepairDomain::PageGeometry; }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::incrementalAppend(QStringLiteral("validation does not mutate printable content")); }
    QJsonObject parameterSchema() const override { return geometrySchema(); }

    PDFOperationResult analyze(const PDFDocument&, const QJsonObject& parameters, PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("Production validation plan is null."));
        }
        plan->operationId = id();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->requiresPreview = false;
        plan->requiresPostflight = false;
        const PDFProductionValidationReport report = validateProductionGeometry(PDFProductionGeometryModel::fromJson(parameters.value(QStringLiteral("geometry")).toObject()));
        plan->warnings.append(diagnosticMessages(report.diagnostics));
        plan->targets.append({ -1, {}, QStringLiteral("production/geometry") });
        return report.valid ? PDFOperationResult(true) : PDFOperationResult(QStringLiteral("Production geometry validation failed."));
    }

    PDFOperationResult apply(PDFDocument*, const PDFRepairPlan&, PDFRepairResult* result) const override
    {
        if (!result)
        {
            return PDFOperationResult(QStringLiteral("Production validation result is null."));
        }
        result->warnings.append(QStringLiteral("Validation completed; no printable PDF content was mutated."));
        return PDFOperationResult(true);
    }
};

class PDFPlanContourBleedRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("production.add-contour-bleed"); }
    PDFRepairRisk risk() const override { return PDFRepairRisk::High; }
    PDFRepairDomains domains() const override { return PDFRepairDomain::Paths | PDFRepairDomain::Images | PDFRepairDomain::PageGeometry; }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("production bleed is a corrective candidate")); }
    QJsonObject parameterSchema() const override
    {
        QJsonObject schema = geometrySchema();
        QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
        properties.insert(QStringLiteral("contour_id"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } });
        properties.insert(QStringLiteral("amount_pt"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") }, { QStringLiteral("minimum"), 0.001 } });
        schema.insert(QStringLiteral("properties"), properties);
        schema.insert(QStringLiteral("required"), QJsonArray{ QStringLiteral("geometry"), QStringLiteral("contour_id") });
        return schema;
    }

    PDFOperationResult analyze(const PDFDocument&, const QJsonObject& parameters, PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("Contour bleed plan is null."));
        }
        const PDFProductionGeometryModel model = PDFProductionGeometryModel::fromJson(parameters.value(QStringLiteral("geometry")).toObject());
        const QString contourId = parameters.value(QStringLiteral("contour_id")).toString();
        const PDFProductionContour* contour = nullptr;
        for (const PDFProductionContour& candidate : model.contours)
        {
            if (candidate.id == contourId)
            {
                contour = &candidate;
                break;
            }
        }
        if (!contour)
        {
            return PDFOperationResult(QStringLiteral("Requested production contour was not found."));
        }
        PDFContourBleedSettings settings;
        settings.amountPt = parameters.value(QStringLiteral("amount_pt")).toDouble(settings.amountPt);
        const PDFContourBleedPlan contourPlan = planContourBleed(*contour, settings);
        plan->operationId = id();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->expectedChanges.pageContent = true;
        plan->requiresPreview = true;
        plan->requiresPostflight = true;
        plan->targets.append({ contour->pageIndex, {}, QStringLiteral("production/contours/%1/bleed").arg(contour->id) });
        plan->warnings.append(diagnosticMessages(contourPlan.diagnostics));
        plan->preconditions.append(QStringLiteral("Contour geometry is explicit, closed, and self-intersection-free."));
        if (!contourPlan.valid || hasErrors(contourPlan.diagnostics))
        {
            return PDFOperationResult(QStringLiteral("Contour bleed planning failed."));
        }
        return PDFOperationResult(true);
    }

    PDFOperationResult apply(PDFDocument* candidate, const PDFRepairPlan& plan, PDFRepairResult* result) const override
    {
        if (!candidate || !result)
        {
            return PDFOperationResult(QStringLiteral("Contour bleed result is null."));
        }
        PDFProductionGeometryModel geometry = PDFProductionGeometryModel::fromJson(plan.parameters.value(QStringLiteral("geometry")).toObject());
        QVector<PDFProductionContour> selectedContours;
        for (const PDFProductionContour& contour : geometry.contours)
        {
            if (contour.id == plan.parameters.value(QStringLiteral("contour_id")).toString())
            {
                selectedContours.append(contour);
            }
        }
        geometry.contours = std::move(selectedContours);
        PDFContourBleedFixupSettings settings;
        settings.amountPt = plan.parameters.value(QStringLiteral("amount_pt")).toDouble(settings.amountPt);
        PDFContourBleedFixupReport report;
        const PDFOperationResult fixupResult = PDFContourBleedFixup::apply(candidate, geometry, settings, &report);
        if (!fixupResult)
        {
            return fixupResult;
        }
        for (const PDFContourBleedFixupPageReport& page : report.pages)
        {
            if (page.applied)
            {
                result->changes.append({ { int(page.pageIndex), {}, QStringLiteral("production/contours/%1/bleed").arg(page.contourId) },
                                         QStringLiteral("contour-bleed"),
                                         QStringLiteral("artwork inside the explicit cut contour"),
                                         QStringLiteral("deterministic raster edge extension outside the contour"),
                                         true });
            }
            result->warnings.append(page.warnings);
        }
        return PDFOperationResult(true);
    }
};

class PDFPlaceGrommetsRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("production.place-grommets"); }
    PDFRepairRisk risk() const override { return PDFRepairRisk::Medium; }
    PDFRepairDomains domains() const override { return PDFRepairDomain::Paths | PDFRepairDomain::PageGeometry; }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::incrementalAppend(QStringLiteral("planning-only operation does not mutate the document")); }
    QJsonObject parameterSchema() const override
    {
        return QJsonObject{
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("required"), QJsonArray{ QStringLiteral("rect") } },
            { QStringLiteral("properties"), QJsonObject{
                { QStringLiteral("rect"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("object") } } },
                { QStringLiteral("spec"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("object") } } }
            } }
        };
    }

    PDFOperationResult analyze(const PDFDocument&, const QJsonObject& parameters, PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("Grommet plan is null."));
        }
        const QJsonObject rectObject = parameters.value(QStringLiteral("rect")).toObject();
        const QRectF rect(rectObject.value(QStringLiteral("x")).toDouble(),
                          rectObject.value(QStringLiteral("y")).toDouble(),
                          rectObject.value(QStringLiteral("width")).toDouble(),
                          rectObject.value(QStringLiteral("height")).toDouble());
        const PDFGrommetPlacementReport grommets = placeGrommets(rect, PDFGrommetSpec::fromJson(parameters.value(QStringLiteral("spec")).toObject()));
        plan->operationId = id();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->requiresPreview = true;
        plan->requiresPostflight = true;
        plan->targets.append({ -1, {}, QStringLiteral("production/grommets") });
        plan->warnings.append(diagnosticMessages(grommets.diagnostics));
        return hasErrors(grommets.diagnostics) ? PDFOperationResult(QStringLiteral("Grommet placement failed.")) : PDFOperationResult(true);
    }

    PDFOperationResult apply(PDFDocument*, const PDFRepairPlan&, PDFRepairResult* result) const override
    {
        if (!result)
        {
            return PDFOperationResult(QStringLiteral("Grommet result is null."));
        }
        result->warnings.append(QStringLiteral("Grommet coordinates were planned deterministically; placement remains typed production geometry."));
        return PDFOperationResult(true);
    }
};

const bool registerProductionRepairOperations = []
{
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFValidateWideFormatRepair>());
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFPlanContourBleedRepair>());
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFPlaceGrommetsRepair>());
    return true;
}();

} // namespace

} // namespace pdf
