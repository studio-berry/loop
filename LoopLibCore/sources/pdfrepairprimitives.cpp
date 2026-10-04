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

#include "pdfrepairoperation.h"

#include "pdfbleedfixup.h"
#include "pdfcatalog.h"
#include "pdfdocumentbuilder.h"
#include "pdfimagedownsamplefixup.h"
#include "pdfimageoptimizer.h"
#include "pdfrgbtocmykfixup.h"
#include "pdfstandardconversion.h"

#include <QJsonValue>
#include <QProcess>
#include <QtMath>

#include <cmath>
#include <memory>

namespace pdf
{

namespace
{

PDFOperationResult bleedModeOrError(const QJsonObject& parameters, PDFBleedFixupMode* modeOut)
{
    if (!modeOut)
    {
        return PDFOperationResult(QStringLiteral("Bleed mode output is null."));
    }
    const QString mode = parameters.value(QStringLiteral("mode")).toString(QStringLiteral("mirror")).trimmed().toLower();
    if (mode.isEmpty() || mode == QStringLiteral("mirror"))
    {
        *modeOut = PDFBleedFixupMode::Mirror;
        return PDFOperationResult(true);
    }
    if (mode == QStringLiteral("pixel-repeat") || mode == QStringLiteral("repeat"))
    {
        *modeOut = PDFBleedFixupMode::PixelRepeat;
        return PDFOperationResult(true);
    }
    if (mode == QStringLiteral("stretch"))
    {
        *modeOut = PDFBleedFixupMode::Stretch;
        return PDFOperationResult(true);
    }
    return PDFOperationResult(QStringLiteral("Unknown bleed mode '%1'.").arg(mode));
}

PDFOperationResult bleedSettingsOrError(const QJsonObject& parameters, bool analyzeOnly, PDFBleedFixupSettings* settings)
{
    if (!settings)
    {
        return PDFOperationResult(QStringLiteral("Bleed settings output is null."));
    }
    PDFBleedFixupMode mode = PDFBleedFixupMode::Mirror;
    const PDFOperationResult modeResult = bleedModeOrError(parameters, &mode);
    if (!modeResult)
    {
        return modeResult;
    }
    settings->mode = mode;
    const double bleedMm = parameters.value(QStringLiteral("bleed_mm")).toDouble(3.0);
    const double safeBleedMm = std::isfinite(bleedMm) ? qBound(0.0, bleedMm, 1000.0) : 0.0;
    settings->bleedMM = QMarginsF(safeBleedMm, safeBleedMm, safeBleedMm, safeBleedMm);
    settings->pageRange = parameters.value(QStringLiteral("page_range")).toString(QStringLiteral("-"));
    settings->force = parameters.value(QStringLiteral("force")).toBool(false);
    settings->skipIfAlreadyBleeding = parameters.value(QStringLiteral("skip_if_already_bleeding")).toBool(false);
    if (settings->force)
    {
        settings->skipIfAlreadyBleeding = false;
    }
    settings->analyzeOnly = analyzeOnly;
    return PDFOperationResult(true);
}

QJsonObject addBleedParameterSchema()
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("additionalProperties"), false },
        { QStringLiteral("properties"), QJsonObject{
                                            { QStringLiteral("mode"), QJsonObject{
                                                                          { QStringLiteral("type"), QStringLiteral("string") },
                                                                          { QStringLiteral("enum"), QJsonArray{ QStringLiteral("mirror"), QStringLiteral("pixel-repeat"), QStringLiteral("stretch") } } } },
                                            { QStringLiteral("bleed_mm"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") }, { QStringLiteral("minimum"), 0.0 }, { QStringLiteral("maximum"), 1000.0 } } },
                                            { QStringLiteral("page_range"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                                            { QStringLiteral("force"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                                            { QStringLiteral("skip_if_already_bleeding"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } } } }
    };
}

void addBleedPlanTargets(const PDFBleedFixupReport& report, PDFRepairPlan* plan)
{
    for (const PDFBleedFixupPageReport& page : report.pages)
    {
        const bool boxesChanged = page.originalMediaBox != page.newMediaBox ||
                                  page.originalCropBox != page.newCropBox ||
                                  page.originalBleedBox != page.newBleedBox ||
                                  page.originalTrimBox != page.newTrimBox;
        if (!boxesChanged && page.sidesApplied.isEmpty())
        {
            plan->warnings.append(page.skipReasons);
            continue;
        }
        plan->targets.append({ int(page.pageIndex), {}, QStringLiteral("pages/%1/bleed").arg(page.pageIndex) });
    }
}

class PDFAddBleedRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("add-bleed"); }
    bool isPreflightFixup() const override { return true; }
    PDFRepairRisk risk() const override { return PDFRepairRisk::Medium; }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("bleed correction must preserve the trusted source")); }
    QJsonObject parameterSchema() const override { return addBleedParameterSchema(); }
    PDFRepairDomains domains() const override
    {
        return PDFRepairDomain::PageGeometry | PDFRepairDomain::Images | PDFRepairDomain::Structure;
    }
    PDFOperationImpact impact(const PDFDocument*, const QJsonObject&) const override
    {
        PDFOperationImpact declared;
        declared.declared = true;
        declared.allPages = true;
        declared.domains = PDFEvidenceDomains(PDFEvidenceDomain::Images) |
                           PDFEvidenceDomain::Colorants |
                           PDFEvidenceDomain::Strokes |
                           PDFEvidenceDomain::OverprintTransparency;
        declared.impactComplete = true;
        return declared;
    }

    PDFOperationResult analyze(const PDFDocument& source,
                               const QJsonObject& parameters,
                               PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("Bleed repair plan is null."));
        }
        plan->operationId = id();
        plan->operationVersion = version();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->expectedChanges.pageBoxes = true;
        plan->expectedChanges.pageContent = true;
        plan->expectedChanges.images = true;
        plan->expectedChanges.metadata = true;
        plan->validators = { PDFRepairValidatorKind::StructuralIntegrity,
                             PDFRepairValidatorKind::NormalPreflight };

        PDFBleedFixupSettings settings;
        const PDFOperationResult settingsResult = bleedSettingsOrError(parameters, true, &settings);
        if (!settingsResult)
        {
            plan->unsupportedReasons.append(settingsResult.getErrorMessage());
            return settingsResult;
        }

        PDFDocument candidate = source;
        PDFBleedFixupReport report;
        const PDFOperationResult result = PDFBleedFixup::apply(&candidate,
                                                               settings,
                                                               &report);
        if (!result)
        {
            return result;
        }
        addBleedPlanTargets(report, plan);
        return PDFOperationResult(true);
    }

    PDFOperationResult apply(PDFDocument* candidate,
                             const PDFRepairPlan& plan,
                             PDFRepairResult* result) const override
    {
        if (!candidate || !result)
        {
            return PDFOperationResult(QStringLiteral("Bleed repair candidate or result is null."));
        }
        PDFBleedFixupSettings settings;
        const PDFOperationResult settingsResult = bleedSettingsOrError(plan.parameters, false, &settings);
        if (!settingsResult)
        {
            return settingsResult;
        }
        PDFBleedFixupReport report;
        const PDFOperationResult fixupResult = PDFBleedFixup::apply(candidate,
                                                                    settings,
                                                                    &report);
        if (!fixupResult)
        {
            return fixupResult;
        }

        for (const PDFBleedFixupPageReport& page : report.pages)
        {
            const QString path = QStringLiteral("pages/%1/bleed").arg(page.pageIndex);
            const bool boxesChanged = page.originalMediaBox != page.newMediaBox ||
                                      page.originalCropBox != page.newCropBox ||
                                      page.originalBleedBox != page.newBleedBox ||
                                      page.originalTrimBox != page.newTrimBox;
            if (boxesChanged)
            {
                result->changes.append({ { int(page.pageIndex), {}, path + QStringLiteral("/boxes") },
                                         QStringLiteral("page-box"),
                                         QStringLiteral("original page boxes"),
                                         QStringLiteral("expanded page boxes"),
                                         true });
            }
            if (!page.sidesApplied.isEmpty())
            {
                result->changes.append({ { int(page.pageIndex), {}, path + QStringLiteral("/content") },
                                         QStringLiteral("generated-bleed-content"),
                                         QStringLiteral("no generated bleed content"),
                                         QStringLiteral("edge-extension content"),
                                         true });
            }
            result->warnings.append(page.skipReasons);
        }
        return PDFOperationResult(true);
    }
};

PDFImageDownsampleFixupSettings downsampleSettings(const QJsonObject& parameters)
{
    PDFImageDownsampleFixupSettings settings;
    settings.targetDpi = qBound(72, parameters.value(QStringLiteral("target_dpi")).toInt(300), 1200);
    settings.jpegQuality = qBound(50, parameters.value(QStringLiteral("quality")).toInt(90), 100);
    settings.keepOriginalIfLarger = true;
    settings.preserveTransparency = true;
    settings.preserveColorMode = true;
    return settings;
}

QJsonObject downsampleImagesParameterSchema()
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("additionalProperties"), false },
        { QStringLiteral("properties"), QJsonObject{
                                            { QStringLiteral("target_dpi"), QJsonObject{
                                                                                { QStringLiteral("type"), QStringLiteral("integer") },
                                                                                { QStringLiteral("minimum"), 72 },
                                                                                { QStringLiteral("maximum"), 1200 } } },
                                            { QStringLiteral("quality"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") }, { QStringLiteral("minimum"), 50 }, { QStringLiteral("maximum"), 100 } } } } }
    };
}

class PDFDownsampleImagesRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("downsample-images"); }
    bool isPreflightFixup() const override { return true; }
    PDFRepairRisk risk() const override { return PDFRepairRisk::Medium; }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::fullRewrite(QStringLiteral("image downsampling removes prior image data")); }
    QJsonObject parameterSchema() const override { return downsampleImagesParameterSchema(); }
    PDFRepairDomains domains() const override { return PDFRepairDomain::Images | PDFRepairDomain::Color; }
    PDFOperationImpact impact(const PDFDocument*, const QJsonObject&) const override
    {
        PDFOperationImpact declared;
        declared.declared = true;
        declared.domains.setFlag(PDFEvidenceDomain::Images);
        declared.domains.setFlag(PDFEvidenceDomain::Colorants);
        declared.fullRewrite = true;
        declared.documentWide = true;
        declared.impactComplete = true;
        return declared;
    }

    PDFOperationResult analyze(const PDFDocument& source,
                               const QJsonObject& parameters,
                               PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("Image repair plan is null."));
        }
        plan->operationId = id();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->expectedChanges.images = true;
        plan->expectedChanges.colorSpaces = true;
        plan->validators = { PDFRepairValidatorKind::StructuralIntegrity,
                             PDFRepairValidatorKind::ImageResolution,
                             PDFRepairValidatorKind::NormalPreflight };

        const std::vector<PDFImageOptimizer::ImageInfo> infos = PDFImageOptimizer::collectImageInfos(&source);
        const int targetDpi = downsampleSettings(parameters).targetDpi;
        for (const PDFImageOptimizer::ImageInfo& info : infos)
        {
            if (info.isImageMask)
            {
                continue;
            }
            const bool highX = std::isfinite(info.minimalDpi.x()) && info.minimalDpi.x() > targetDpi * 1.15;
            const bool highY = std::isfinite(info.minimalDpi.y()) && info.minimalDpi.y() > targetDpi * 1.15;
            if (highX || highY)
            {
                plan->targets.append({ -1, info.reference,
                                       QStringLiteral("resources/images/%1").arg(info.reference.objectNumber) });
            }
        }
        if (plan->targets.isEmpty())
        {
            plan->warnings.append(QStringLiteral("no-images-require-downsampling"));
        }
        return PDFOperationResult(true);
    }

    PDFOperationResult apply(PDFDocument* candidate,
                             const PDFRepairPlan& plan,
                             PDFRepairResult* result) const override
    {
        if (!candidate || !result)
        {
            return PDFOperationResult(QStringLiteral("Image repair candidate or result is null."));
        }
        PDFImageDownsampleFixupReport report;
        const PDFOperationResult fixupResult = PDFImageDownsampleFixup::apply(candidate,
                                                                              downsampleSettings(plan.parameters),
                                                                              &report);
        if (!fixupResult)
        {
            return fixupResult;
        }
        for (const PDFImageOptimizer::ImageResult& image : report.images)
        {
            if (!image.keptOriginal)
            {
                result->changes.append({ { -1, image.reference,
                                           QStringLiteral("resources/images/%1").arg(image.reference.objectNumber) },
                                         QStringLiteral("image-resource"),
                                         QStringLiteral("original image resource"),
                                         QStringLiteral("optimized image resource"),
                                         true });
            }
            if (!image.message.isEmpty())
            {
                result->warnings.append(image.message);
            }
        }
        return PDFOperationResult(true);
    }
};

PDFRgbToCmykSettings cmykSettings(const QJsonObject& parameters)
{
    PDFRgbToCmykSettings settings;
    settings.targetIccData = QByteArray::fromBase64(parameters.value(QStringLiteral("target_icc_base64")).toString().toLatin1());
    settings.targetIccId = parameters.value(QStringLiteral("target_icc_id")).toString(QStringLiteral("loop-cmyk")).toUtf8();
    settings.targetProfileName = parameters.value(QStringLiteral("target_profile_name")).toString();
    const int intent = qBound(0, parameters.value(QStringLiteral("intent")).toInt(int(settings.intent)), 3);
    settings.intent = static_cast<RenderingIntent>(intent);
    settings.blackPointCompensation = parameters.value(QStringLiteral("black_point_compensation")).toBool(true);
    settings.embedOutputIntent = parameters.value(QStringLiteral("embed_output_intent")).toBool(true);
    return settings;
}

QJsonObject rgbToCmykParameterSchema()
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("additionalProperties"), false },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("target_icc_base64") } },
        { QStringLiteral("properties"), QJsonObject{
                                            { QStringLiteral("target_icc_base64"), QJsonObject{
                                                                                       { QStringLiteral("type"), QStringLiteral("string") },
                                                                                       { QStringLiteral("minLength"), 1 } } },
                                            { QStringLiteral("target_icc_id"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                                            { QStringLiteral("target_profile_name"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                                            { QStringLiteral("intent"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") }, { QStringLiteral("minimum"), 0 }, { QStringLiteral("maximum"), 3 } } },
                                            { QStringLiteral("black_point_compensation"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                                            { QStringLiteral("embed_output_intent"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } } } }
    };
}

class PDFRgbToCmykRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("rgb-to-cmyk"); }
    bool isPreflightFixup() const override { return true; }
    PDFRepairRisk risk() const override { return PDFRepairRisk::High; }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("color conversion creates a production candidate")); }
    QJsonObject parameterSchema() const override { return rgbToCmykParameterSchema(); }
    PDFRepairDomains domains() const override
    {
        return PDFRepairDomain::Color | PDFRepairDomain::Images | PDFRepairDomain::Structure;
    }
    PDFOperationImpact impact(const PDFDocument*, const QJsonObject&) const override
    {
        PDFOperationImpact declared;
        declared.declared = true;
        declared.domains = PDFEvidenceDomains(PDFEvidenceDomain::Colorants) | PDFEvidenceDomain::Images;
        declared.documentWide = true;
        declared.impactComplete = true;
        return declared;
    }

    PDFOperationResult analyze(const PDFDocument& source,
                               const QJsonObject& parameters,
                               PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("RGB-to-CMYK repair plan is null."));
        }
        PDFRgbToCmykSettings settings = cmykSettings(parameters);
        if (settings.targetIccData.isEmpty())
        {
            return PDFOperationResult(QStringLiteral("target_icc_base64 is required for rgb-to-cmyk."));
        }
        plan->operationId = id();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->expectedChanges.pageContent = true;
        plan->expectedChanges.images = true;
        plan->expectedChanges.colorSpaces = true;
        plan->expectedChanges.outputIntent = true;
        plan->validators = { PDFRepairValidatorKind::StructuralIntegrity,
                             PDFRepairValidatorKind::ColorMode,
                             PDFRepairValidatorKind::OutputIntent,
                             PDFRepairValidatorKind::NormalPreflight };

        PDFRgbToCmykReport report;
        const PDFOperationResult result = PDFRgbToCmykFixup::previewRgbToCmyk(&source, settings, &report);
        if (!result)
        {
            return result;
        }
        for (const PDFRgbToCmykUnsupportedItem& unsupported : report.unsupported)
        {
            plan->unsupportedReasons.append(unsupported.reason);
        }
        if (!report.unsupported.isEmpty())
        {
            return PDFOperationResult(QStringLiteral("RGB-to-CMYK contains unsupported constructs."));
        }
        plan->targets.append({ -1, {}, QStringLiteral("document/color") });
        return PDFOperationResult(true);
    }

    PDFOperationResult apply(PDFDocument* candidate,
                             const PDFRepairPlan& plan,
                             PDFRepairResult* result) const override
    {
        if (!candidate || !result)
        {
            return PDFOperationResult(QStringLiteral("RGB-to-CMYK repair candidate or result is null."));
        }
        PDFRgbToCmykReport report;
        const PDFOperationResult fixupResult = PDFRgbToCmykFixup::writeRgbToCmyk(candidate,
                                                                                 cmykSettings(plan.parameters),
                                                                                 &report);
        if (!fixupResult)
        {
            return fixupResult;
        }
        if (report.vectorPaintsConverted || report.imagesConverted || report.indexedPalettesConverted)
        {
            result->changes.append({ { -1, {}, QStringLiteral("document/color") },
                                     QStringLiteral("color-conversion"),
                                     QStringLiteral("source color spaces"),
                                     QStringLiteral("CMYK-managed color spaces"),
                                     true });
        }
        if (report.outputIntentChanged)
        {
            result->changes.append({ { -1, {}, QStringLiteral("document/output-intent") },
                                     QStringLiteral("output-intent"),
                                     QStringLiteral("original output intent"),
                                     QStringLiteral("configured CMYK output intent"),
                                     true });
        }
        result->warnings.append(report.warnings);
        return PDFOperationResult(true);
    }
};


QJsonObject standardConversionParameterSchema()
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("additionalProperties"), false },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("target"), QStringLiteral("target_icc_base64"), QStringLiteral("validation_contract") } },
        { QStringLiteral("properties"), QJsonObject{
                                            { QStringLiteral("validation_contract"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") }, { QStringLiteral("enum"), QJsonArray{ 2 } } } },
                                            { QStringLiteral("target"), QJsonObject{
                                                                            { QStringLiteral("type"), QStringLiteral("string") },
                                                                            { QStringLiteral("enum"), QJsonArray::fromStringList(supportedPDFStandardTargets()) } } },
                                            { QStringLiteral("target_icc_base64"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") }, { QStringLiteral("minLength"), 1 } } },
                                            { QStringLiteral("target_icc_id"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                                            { QStringLiteral("target_profile_name"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                                            { QStringLiteral("normalize_color"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                                            { QStringLiteral("black_point_compensation"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                                            { QStringLiteral("flatten_transparency"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } },
                                            { QStringLiteral("validator_program"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } },
                                            { QStringLiteral("validator_arguments"), QJsonObject{ { QStringLiteral("oneOf"), QJsonArray{ QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } }, QJsonObject{ { QStringLiteral("type"), QStringLiteral("array") }, { QStringLiteral("items"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } } } } } } },
                                            { QStringLiteral("validator_timeout_ms"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") }, { QStringLiteral("minimum"), 1000 }, { QStringLiteral("maximum"), 3600000 } } },
                                            { QStringLiteral("dry_run_only"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("boolean") } } } } }
    };
}

class PDFStandardConversionRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("standards-convert"); }
    int version() const override { return 2; }
    PDFRepairRisk risk() const override { return PDFRepairRisk::Destructive; }
    QJsonObject parameterSchema() const override { return standardConversionParameterSchema(); }
    PDFRepairDomains domains() const override
    {
        return PDFRepairDomain::Color | PDFRepairDomain::Fonts | PDFRepairDomain::Images | PDFRepairDomain::Metadata | PDFRepairDomain::PageGeometry | PDFRepairDomain::Structure;
    }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::fullRewrite(QStringLiteral("standard conversion removes prior content")); }
    PDFOperationImpact impact(const PDFDocument*, const QJsonObject&) const override
    {
        PDFOperationImpact declared;
        declared.declared = true;
        declared.domains = pdfEvidenceAllDomains();
        declared.documentWide = true;
        declared.fullRewrite = true;
        declared.impactComplete = false;
        declared.requiresIndependentOracle = true;
        return declared;
    }

    PDFOperationResult analyze(const PDFDocument& source,
                               const QJsonObject& parameters,
                               PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("Standard conversion plan is null."));
        }
        if (parameters.value(QStringLiteral("validation_contract")).toInt() != 2)
        {
            return PDFOperationResult(QStringLiteral("standards-convert v2 requires validation_contract: 2; migrate the recipe for final-artifact validation."));
        }
        PDFStandardTarget target;
        if (!pdfStandardTargetFromString(parameters.value(QStringLiteral("target")).toString(), &target))
        {
            return PDFOperationResult(QStringLiteral("A supported PDF/X or PDF/A target is required."));
        }
        const PDFStandardConversionSettings settings = standardConversionSettings(parameters);
        plan->operationId = id();
        plan->operationVersion = version();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->expectedChanges.metadata = true;
        plan->expectedChanges.outputIntent = true;
        plan->expectedChanges.pageBoxes = true;
        plan->expectedChanges.colorSpaces = settings.normalizeColor;
        plan->expectedChanges.pageContent = settings.normalizeColor || flattensTransparency(settings);
        plan->expectedChanges.images = flattensTransparency(settings);
        plan->validators = { PDFRepairValidatorKind::StructuralIntegrity,
                             PDFRepairValidatorKind::OutputIntent,
                             PDFRepairValidatorKind::NormalPreflight };
        plan->warnings.append(QStringLiteral("An independent validator with a {input} argument is required before commit."));

        PDFStandardConversionReport report;
        const PDFOperationResult previewResult = PDFStandardConversion::preview(&source, settings, &report);
        plan->warnings.append(report.warnings);
        plan->unsupportedReasons.append(report.blockers);
        for (const PDFStandardConversionChange& change : report.changes)
        {
            plan->targets.append({ -1, {}, QStringLiteral("document/%1").arg(change.id) });
        }
        return previewResult;
    }

    PDFOperationResult apply(PDFDocument* candidate,
                             const PDFRepairPlan& plan,
                             PDFRepairResult* result) const override
    {
        if (!candidate || !result)
        {
            return PDFOperationResult(QStringLiteral("Standard conversion candidate or result is null."));
        }
        PDFStandardConversionReport report;
        const PDFOperationResult conversionResult = PDFStandardConversion::prepare(candidate,
                                                                                   standardConversionSettings(plan.parameters),
                                                                                   &report);
        result->warnings.append(report.warnings);
        for (const PDFStandardConversionChange& change : report.changes)
        {
            result->changes.append({ { -1, {}, QStringLiteral("document/%1").arg(change.id) },
                                     QStringLiteral("standard-conversion"),
                                     change.before,
                                     change.after,
                                     true });
        }
        PDFRepairValidationResult validation;
        validation.status = conversionResult ? PDFRepairStatus::Incomplete : PDFRepairStatus::Failed;
        validation.validatorId = QStringLiteral("independent-standard-validator");
        validation.summary = conversionResult ? QStringLiteral("Prepared; final artifact validation is pending.")
                                              : conversionResult.getErrorMessage();
        result->validations.append(validation);
        result->verdict = report.postflightAfter.value(QStringLiteral("verdict")).toObject();
        return conversionResult;
    }
};

struct PageBoxTranslation
{
    PDFObjectReference pageReference;
    int pageIndex = -1;
    QString box;
    QRectF before;
    QRectF after;
};

QRectF pageBoxOf(const PDFPage& page, const QString& box)
{
    if (box == QLatin1String("media"))
    {
        return page.getMediaBox();
    }
    if (box == QLatin1String("crop"))
    {
        return page.getCropBox();
    }
    if (box == QLatin1String("bleed"))
    {
        return page.getBleedBox();
    }
    if (box == QLatin1String("trim"))
    {
        return page.getTrimBox();
    }
    return page.getArtBox();
}

bool containsBox(const QRectF& outer, const QRectF& inner)
{
    constexpr qreal tolerance = 1e-6;
    return inner.left() >= outer.left() - tolerance && inner.right() <= outer.right() + tolerance &&
           inner.top() >= outer.top() - tolerance && inner.bottom() <= outer.bottom() + tolerance;
}

QJsonObject translatePageBoxParameterSchema()
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("additionalProperties"), false },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("box"), QStringLiteral("page_index"), QStringLiteral("dx"), QStringLiteral("dy") } },
        { QStringLiteral("properties"), QJsonObject{
                                            { QStringLiteral("box"), QJsonObject{
                                                                         { QStringLiteral("type"), QStringLiteral("string") },
                                                                         { QStringLiteral("enum"), QJsonArray{ QStringLiteral("media"), QStringLiteral("crop"), QStringLiteral("bleed"), QStringLiteral("trim"), QStringLiteral("art") } } } },
                                            { QStringLiteral("page_index"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("integer") }, { QStringLiteral("minimum"), 0 } } },
                                            { QStringLiteral("dx"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } },
                                            { QStringLiteral("dy"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("number") } } } } }
    };
}

/// Resolves a translate-page-box request against the source document. The
/// translation is in unrotated PDF user space, the same space the page-box
/// hit-test targets report. A result that would leave a box outside the media
/// box, or the trim box outside the bleed box, is refused rather than repaired.
PDFOperationResult resolvePageBoxTranslation(const PDFDocument& document,
                                             const QJsonObject& parameters,
                                             PageBoxTranslation* translation)
{
    if (!translation)
    {
        return PDFOperationResult(QStringLiteral("Page-box translation output is null."));
    }
    const QString box = parameters.value(QStringLiteral("box")).toString();
    static const QStringList boxes = { QStringLiteral("media"), QStringLiteral("crop"), QStringLiteral("bleed"), QStringLiteral("trim"), QStringLiteral("art") };
    if (!boxes.contains(box))
    {
        return PDFOperationResult(QStringLiteral("Unknown page box '%1'.").arg(box));
    }
    const QJsonValue pageIndexValue = parameters.value(QStringLiteral("page_index"));
    const QJsonValue dxValue = parameters.value(QStringLiteral("dx"));
    const QJsonValue dyValue = parameters.value(QStringLiteral("dy"));
    if (!pageIndexValue.isDouble() || !dxValue.isDouble() || !dyValue.isDouble())
    {
        return PDFOperationResult(QStringLiteral("page_index, dx and dy are required numbers."));
    }
    const double dx = dxValue.toDouble();
    const double dy = dyValue.toDouble();
    if (!std::isfinite(dx) || !std::isfinite(dy))
    {
        return PDFOperationResult(QStringLiteral("Translation must be finite."));
    }
    if (dx == 0.0 && dy == 0.0)
    {
        return PDFOperationResult(QStringLiteral("Translation is zero; nothing to move."));
    }

    const PDFCatalog* catalog = document.getCatalog();
    const int pageIndex = pageIndexValue.toInt(-1);
    if (!catalog || pageIndex < 0 || pageIndex >= int(catalog->getPageCount()))
    {
        return PDFOperationResult(QStringLiteral("Page index %1 is outside the document.").arg(pageIndex));
    }
    const PDFPage* page = catalog->getPage(pageIndex);
    if (!page)
    {
        return PDFOperationResult(QStringLiteral("Page %1 is not available.").arg(pageIndex));
    }

    const QRectF before = pageBoxOf(*page, box);
    const QRectF after = before.translated(dx, dy);
    if (box == QLatin1String("media"))
    {
        for (const QString& inner : { QStringLiteral("crop"), QStringLiteral("bleed"), QStringLiteral("trim"), QStringLiteral("art") })
        {
            if (!containsBox(after, pageBoxOf(*page, inner)))
            {
                return PDFOperationResult(QStringLiteral("Moving the media box would leave the %1 box outside it.").arg(inner));
            }
        }
    }
    else
    {
        if (!containsBox(page->getMediaBox(), after))
        {
            return PDFOperationResult(QStringLiteral("Moving the %1 box would leave it outside the media box.").arg(box));
        }
        if (box == QLatin1String("trim") && !containsBox(page->getBleedBox(), after))
        {
            return PDFOperationResult(QStringLiteral("Moving the trim box would leave it outside the bleed box."));
        }
        if (box == QLatin1String("bleed") && !containsBox(after, page->getTrimBox()))
        {
            return PDFOperationResult(QStringLiteral("Moving the bleed box would leave the trim box outside it."));
        }
    }

    translation->pageReference = page->getPageReference();
    translation->pageIndex = pageIndex;
    translation->box = box;
    translation->before = before;
    translation->after = after;
    return PDFOperationResult(true);
}

QString describeBox(const QRectF& rect)
{
    return QStringLiteral("[%1 %2 %3 %4]").arg(rect.left()).arg(rect.top()).arg(rect.right()).arg(rect.bottom());
}

class PDFTranslatePageBoxRepair final : public PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("translate-page-box"); }
    PDFRepairRisk risk() const override { return PDFRepairRisk::Low; }
    PDFOperationSavePolicy savePolicy() const override { return PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("page-box translation must preserve the trusted source")); }
    QJsonObject parameterSchema() const override { return translatePageBoxParameterSchema(); }
    PDFRepairDomains domains() const override { return PDFRepairDomain::PageGeometry; }
    PDFOperationImpact impact(const PDFDocument*, const QJsonObject& parameters) const override
    {
        // No evidence domain tracks page geometry, so the empty domain set makes
        // PDFOperationImpact::isFullRevalidation() true: a box move reruns the profile.
        PDFOperationImpact declared;
        declared.declared = true;
        declared.impactComplete = true;
        const QJsonValue pageIndex = parameters.value(QStringLiteral("page_index"));
        if (pageIndex.isDouble() && pageIndex.toInt(-1) >= 0)
        {
            declared.pages.insert(pageIndex.toInt());
        }
        else
        {
            declared.documentWide = true;
        }
        return declared;
    }

    PDFOperationResult analyze(const PDFDocument& source,
                               const QJsonObject& parameters,
                               PDFRepairPlan* plan) const override
    {
        if (!plan)
        {
            return PDFOperationResult(QStringLiteral("Page-box translation plan is null."));
        }
        plan->operationId = id();
        plan->operationVersion = version();
        plan->parameters = parameters;
        plan->risk = risk();
        plan->domains = domains();
        plan->expectedChanges.pageBoxes = true;
        plan->validators = { PDFRepairValidatorKind::StructuralIntegrity,
                             PDFRepairValidatorKind::NormalPreflight };

        PageBoxTranslation translation;
        const PDFOperationResult resolved = resolvePageBoxTranslation(source, parameters, &translation);
        if (!resolved)
        {
            plan->unsupportedReasons.append(resolved.getErrorMessage());
            return resolved;
        }
        plan->targets.append({ translation.pageIndex, {}, QStringLiteral("pages/%1/%2-box").arg(translation.pageIndex).arg(translation.box) });
        return PDFOperationResult(true);
    }

    PDFOperationResult apply(PDFDocument* candidate,
                             const PDFRepairPlan& plan,
                             PDFRepairResult* result) const override
    {
        if (!candidate || !result)
        {
            return PDFOperationResult(QStringLiteral("Page-box translation candidate or result is null."));
        }
        PageBoxTranslation translation;
        const PDFOperationResult resolved = resolvePageBoxTranslation(*candidate, plan.parameters, &translation);
        if (!resolved)
        {
            return resolved;
        }

        PDFDocumentModifier modifier(candidate);
        PDFDocumentBuilder* builder = modifier.getBuilder();
        if (translation.box == QLatin1String("media"))
        {
            builder->setPageMediaBox(translation.pageReference, translation.after);
        }
        else if (translation.box == QLatin1String("crop"))
        {
            builder->setPageCropBox(translation.pageReference, translation.after);
        }
        else if (translation.box == QLatin1String("bleed"))
        {
            builder->setPageBleedBox(translation.pageReference, translation.after);
        }
        else if (translation.box == QLatin1String("trim"))
        {
            builder->setPageTrimBox(translation.pageReference, translation.after);
        }
        else
        {
            builder->setPageArtBox(translation.pageReference, translation.after);
        }
        modifier.markReset();
        if (!modifier.finalize())
        {
            return PDFOperationResult(QStringLiteral("Failed to finalize page-box translation."));
        }
        *candidate = *modifier.getDocument();

        result->changes.append({ { translation.pageIndex, {}, QStringLiteral("pages/%1/%2-box").arg(translation.pageIndex).arg(translation.box) },
                                 QStringLiteral("page-box"),
                                 describeBox(translation.before),
                                 describeBox(translation.after),
                                 true });
        return PDFOperationResult(true);
    }
};

const bool registerBuiltInRepairOperations = []
{
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFAddBleedRepair>());
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFDownsampleImagesRepair>());
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFRgbToCmykRepair>());
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFStandardConversionRepair>());
    PDFRepairRegistry::instance().registerOperation(std::make_unique<PDFTranslatePageBoxRepair>());
    return true;
}();

}   // namespace

}   // namespace pdf
