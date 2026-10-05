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

#include "pdfstandardconversion.h"
#include "pdfsafefilewriter.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"
#include "pdfdocumentreader.h"
#include "pdfstreamfilters.h"
#include "pdfrgbtocmykfixup.h"
#include "pdftransparencyflattener.h"
#include "preflightengine.h"
#include "pdfpreflightverdict.h"
#include "pdfutils.h"
#include "pdfworkloadenvelope.h"

#include <QJsonArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QBuffer>
#include <QDir>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <lcms2.h>

#include <algorithm>
#include <memory>

namespace pdf
{

namespace
{

bool isPDFX(PDFStandardTarget target)
{
    return target != PDFStandardTarget::PDFA2b;
}

bool normalizesColorByDefault(PDFStandardTarget target)
{
    return target == PDFStandardTarget::PDFX1a2001 || target == PDFStandardTarget::PDFX3_2002;
}

// PDF/X-1a and PDF/X-3 prohibit live transparency (see docs/PDFX_POLICY_MATRIX.md);
// PDF/X-4 and PDF/A-2b permit it, so flattening is opt-in there.
bool flattensTransparencyByDefault(PDFStandardTarget target)
{
    return target == PDFStandardTarget::PDFX1a2001 || target == PDFStandardTarget::PDFX3_2002;
}

QByteArray targetMarker(PDFStandardTarget target)
{
    return pdfStandardTargetToString(target).toUtf8();
}

PDFVersion minimumVersion(PDFStandardTarget target)
{
    switch (target)
    {
        case PDFStandardTarget::PDFX1a2001:
        case PDFStandardTarget::PDFX3_2002:
            return PDFVersion(1, 3);
        case PDFStandardTarget::PDFX4:
            return PDFVersion(1, 4);
        case PDFStandardTarget::PDFA2b:
            return PDFVersion(1, 7);
    }
    return PDFVersion(1, 7);
}

QByteArray pdfVersionName(PDFVersion version)
{
    return QByteArray::number(version.major) + QByteArrayLiteral(".") + QByteArray::number(version.minor);
}

QByteArray xmpForTarget(PDFStandardTarget target)
{
    const QByteArray marker = targetMarker(target);
    if (target == PDFStandardTarget::PDFA2b)
    {
        return QByteArrayLiteral("<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                                 "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                                 "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                                 "<rdf:Description rdf:about=\"\" xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\" "
                                 "pdfaid:part=\"2\" pdfaid:conformance=\"B\"/></rdf:RDF></x:xmpmeta>\n"
                                 "<?xpacket end=\"w\"?>\n");
    }

    return QByteArrayLiteral("<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                             "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                             "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                             "<rdf:Description rdf:about=\"\" xmlns:pdfxid=\"http://www.npes.org/pdfx/ns/id/\" "
                             "pdfxid:GTS_PDFXVersion=\"") +
           marker + QByteArrayLiteral("\"/></rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>\n");
}

QJsonObject pdfxProfile(PDFStandardTarget target)
{
    // PreflightEngine::parseProfile() rejects a profile whose 'checks' array is
    // empty before it looks at 'pdfx', so this profile must carry the shared
    // checks a PDF/X policy layers onto - the same shape as
    // loop-preflight/examples/profile-pdfx-x1a2001.json. The PDF/X rule set
    // itself comes from the target, not from this list. Without them, no PDF/X
    // rule ever ran: preview() reported no blockers for any PDF/X target and
    // every apply() failed at postflight.
    return QJsonObject{
        { QStringLiteral("name"), QStringLiteral("Loop standard conversion preflight") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{ { QStringLiteral("id"), QStringLiteral("color-inventory") },
                                                     { QStringLiteral("severity"), QStringLiteral("info") } },
                                        QJsonObject{ { QStringLiteral("id"), QStringLiteral("transparency-risk") },
                                                     { QStringLiteral("severity"), QStringLiteral("warning") } } } },
        { QStringLiteral("pdfx"), QJsonObject{ { QStringLiteral("target"), pdfStandardTargetToString(target) } } }
    };
}

PDFOperationResult validateIcc(const PDFStandardConversionSettings& settings)
{
    if (settings.outputIntentIccData.isEmpty())
    {
        return PDFTranslationContext::tr("An embedded output-intent ICC profile is required.");
    }

    cmsHPROFILE profile = cmsOpenProfileFromMem(settings.outputIntentIccData.constData(),
                                                static_cast<cmsUInt32Number>(settings.outputIntentIccData.size()));
    if (!profile)
    {
        return PDFTranslationContext::tr("The output-intent ICC profile could not be opened.");
    }
    const bool knownColorSpace = cmsGetColorSpace(profile) == cmsSigCmykData || cmsGetColorSpace(profile) == cmsSigRgbData || cmsGetColorSpace(profile) == cmsSigGrayData;
    const bool cmykRequired = settings.target == PDFStandardTarget::PDFX1a2001 || settings.target == PDFStandardTarget::PDFX3_2002 || settings.normalizeColor;
    const bool valid = knownColorSpace && (!cmykRequired || cmsGetColorSpace(profile) == cmsSigCmykData);
    cmsCloseProfile(profile);
    if (!valid)
    {
        return cmykRequired
                   ? PDFTranslationContext::tr("PDF/X-1a and PDF/X-3 conversion requires a CMYK ICC profile.")
                   : PDFTranslationContext::tr("The output-intent ICC profile has an unsupported color space.");
    }
    return true;
}

int profileComponents(const QByteArray& data)
{
    cmsHPROFILE profile = cmsOpenProfileFromMem(data.constData(), static_cast<cmsUInt32Number>(data.size()));
    if (!profile)
    {
        return 0;
    }
    int components = 0;
    switch (cmsGetColorSpace(profile))
    {
        case cmsSigCmykData:
            components = 4;
            break;
        case cmsSigRgbData:
            components = 3;
            break;
        case cmsSigGrayData:
            components = 1;
            break;
        default:
            break;
    }
    cmsCloseProfile(profile);
    return components;
}

void addOutputIntent(PDFDocumentBuilder* builder,
                     const PDFStandardConversionSettings& settings)
{
    // Not const: it is moved into the PDFStream below, which takes QByteArray&&.
    QByteArray compressed = PDFFlateDecodeFilter::compress(settings.outputIntentIccData);
    PDFDictionary profileDictionary;
    profileDictionary.addEntry(PDFInplaceOrMemoryString("N"), PDFObject::createInteger(profileComponents(settings.outputIntentIccData)));
    profileDictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(compressed.size()));
    profileDictionary.addEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
    const PDFObjectReference profileReference = builder->addObject(
        PDFObject::createStream(std::make_shared<PDFStream>(qMove(profileDictionary), qMove(compressed))));

    const QString identifier = settings.outputIntentName.isEmpty()
                                   ? QString::fromLatin1(QCryptographicHash::hash(settings.outputIntentIccData, QCryptographicHash::Sha256).toHex())
                                   : settings.outputIntentName;
    PDFDictionary intentDictionary;
    intentDictionary.addEntry(PDFInplaceOrMemoryString("Type"), PDFObject::createName("OutputIntent"));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("S"), PDFObject::createName(isPDFX(settings.target) ? "GTS_PDFX" : "GTS_PDFA1"));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("OutputConditionIdentifier"), PDFObject::createString(identifier.toUtf8()));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("OutputCondition"), PDFObject::createString(settings.outputIntentName.toUtf8()));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("DestOutputProfile"), PDFObject::createReference(profileReference));
    const PDFObjectReference intentReference = builder->addObject(
        PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(intentDictionary))));

    PDFArray outputIntents;
    outputIntents.appendItem(PDFObject::createReference(intentReference));
    PDFDictionary catalogUpdate;
    catalogUpdate.addEntry(PDFInplaceOrMemoryString("OutputIntents"),
                           PDFObject::createArray(std::make_shared<PDFArray>(qMove(outputIntents))));
    builder->mergeTo(builder->getCatalogReference(),
                     PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(catalogUpdate))));
}

void addVersion(PDFDocumentBuilder* builder, PDFVersion version)
{
    PDFDictionary catalogUpdate;
    catalogUpdate.addEntry(PDFInplaceOrMemoryString("Version"), PDFObject::createName(pdfVersionName(version)));
    builder->mergeTo(builder->getCatalogReference(),
                     PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(catalogUpdate))));
}

void collectPreflightBlockers(const PDFStandardConversionSettings& settings,
                              const PreflightResult& result,
                              PDFStandardConversionReport* report)
{
    if (!result.pdfx.has_value())
    {
        return;
    }

    const bool normalizeColor = settings.normalizeColor || normalizesColorByDefault(settings.target);
    const bool flattenTransparency = flattensTransparency(settings);
    for (const PDFXRuleResult& rule : result.pdfx->rules)
    {
        if (rule.state != PDFXRuleState::Failed && rule.state != PDFXRuleState::NotInspected)
        {
            continue;
        }
        const bool fixable = rule.ruleId == QStringLiteral("pdfx.metadata.identification") || rule.ruleId == QStringLiteral("pdfx.output-intent.present") || rule.ruleId == QStringLiteral("pdfx.output-intent.identity") || rule.ruleId == QStringLiteral("pdfx.output-intent.subtype") || rule.ruleId == QStringLiteral("pdfx.output-intent.profile") || rule.ruleId == QStringLiteral("pdfx.output-intent.profile-space") || rule.ruleId == QStringLiteral("pdfx.page.trim-box") || rule.ruleId == QStringLiteral("pdfx.page.bleed-box") || rule.ruleId == QStringLiteral("pdfx.document.version") || (rule.ruleId == QStringLiteral("pdfx.color.device-rgb") && normalizeColor) || (rule.ruleId == QStringLiteral("pdfx.transparency.allowed") && flattenTransparency);
        if (!fixable)
        {
            report->blockers.append(rule.ruleId + QStringLiteral(": ") + rule.diagnostic);
        }
    }
}


}   // namespace

bool flattensTransparency(const PDFStandardConversionSettings& settings)
{
    switch (settings.transparencyFlatten)
    {
        case PDFTransparencyFlattenPolicy::Always:
            return true;
        case PDFTransparencyFlattenPolicy::Never:
            return false;
        case PDFTransparencyFlattenPolicy::Automatic:
            break;
    }
    return flattensTransparencyByDefault(settings.target);
}

QString pdfStandardTargetToString(PDFStandardTarget target)
{
    switch (target)
    {
        case PDFStandardTarget::PDFX1a2001:
            return QStringLiteral("PDF/X-1a:2001");
        case PDFStandardTarget::PDFX3_2002:
            return QStringLiteral("PDF/X-3:2002");
        case PDFStandardTarget::PDFX4:
            return QStringLiteral("PDF/X-4");
        case PDFStandardTarget::PDFA2b:
            return QStringLiteral("PDF/A-2b");
    }
    return QString();
}

bool pdfStandardTargetFromString(const QString& value, PDFStandardTarget* target)
{
    if (!target)
    {
        return false;
    }
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("pdf/x-1a:2001"))
        *target = PDFStandardTarget::PDFX1a2001;
    else if (normalized == QStringLiteral("pdf/x-3:2002"))
        *target = PDFStandardTarget::PDFX3_2002;
    else if (normalized == QStringLiteral("pdf/x-4"))
        *target = PDFStandardTarget::PDFX4;
    else if (normalized == QStringLiteral("pdf/a-2b"))
        *target = PDFStandardTarget::PDFA2b;
    else
        return false;
    return true;
}

QStringList supportedPDFStandardTargets()
{
    return { QStringLiteral("PDF/X-1a:2001"), QStringLiteral("PDF/X-3:2002"),
             QStringLiteral("PDF/X-4"), QStringLiteral("PDF/A-2b") };
}

QJsonObject PDFStandardConversionChange::toJson() const
{
    return QJsonObject{ { QStringLiteral("id"), id },
                        { QStringLiteral("before"), before },
                        { QStringLiteral("after"), after } };
}

QJsonObject PDFStandardConversionReport::toJson() const
{
    QJsonArray changesArray;
    for (const PDFStandardConversionChange& change : changes)
        changesArray.append(change.toJson());
    return QJsonObject{
        { QStringLiteral("target"), target },
        { QStringLiteral("conversion_attempted"), conversionAttempted },
        { QStringLiteral("independent_validation_passed"), independentValidationPassed },
        { QStringLiteral("postflight_passed"), postflightPassed },
        { QStringLiteral("preflight_before"), preflightBefore },
        { QStringLiteral("postflight_after"), postflightAfter },
        { QStringLiteral("changes"), changesArray },
        { QStringLiteral("blockers"), QJsonArray::fromStringList(blockers) },
        { QStringLiteral("warnings"), QJsonArray::fromStringList(warnings) },
        { QStringLiteral("validator"), validator },
        { QStringLiteral("transparency_flatten"), transparencyFlatten }
    };
}

PDFOperationResult PDFStandardConversion::preview(const PDFDocument* document,
                                                  const PDFStandardConversionSettings& settings,
                                                  PDFStandardConversionReport* report)
{
    if (report)
        *report = PDFStandardConversionReport();
    if (!document || !report)
    {
        return PDFTranslationContext::tr("Standard conversion document or report is null.");
    }
    report->target = pdfStandardTargetToString(settings.target);

    const PDFOperationResult profileResult = validateIcc(settings);
    if (!profileResult)
    {
        report->blockers.append(profileResult.getErrorMessage());
        return profileResult;
    }

    const bool normalizeColor = settings.normalizeColor || normalizesColorByDefault(settings.target);
    report->changes.append({ QStringLiteral("metadata.identification"), QStringLiteral("source identification"), report->target });
    report->changes.append({ QStringLiteral("output-intent"), QStringLiteral("source output intent"), QStringLiteral("configured embedded ICC profile") });
    report->changes.append({ QStringLiteral("document.version"), QString::fromLatin1(document->getVersion()), QString::fromLatin1(pdfVersionName(minimumVersion(settings.target))) });
    report->changes.append({ QStringLiteral("page-boxes"), QStringLiteral("inherited or missing production boxes"), QStringLiteral("explicit TrimBox and BleedBox") });
    if (normalizeColor)
    {
        report->changes.append({ QStringLiteral("color.normalization"), QStringLiteral("source color spaces"), QStringLiteral("CMYK-managed color spaces") });
        PDFRgbToCmykSettings colorSettings;
        colorSettings.targetIccData = settings.outputIntentIccData;
        colorSettings.targetIccId = settings.outputIntentIccId;
        colorSettings.targetProfileName = settings.outputIntentName;
        colorSettings.blackPointCompensation = settings.blackPointCompensation;
        PDFRgbToCmykReport colorReport;
        const PDFOperationResult colorResult = PDFRgbToCmykFixup::previewRgbToCmyk(document, colorSettings, &colorReport);
        if (!colorResult)
        {
            report->blockers.append(colorResult.getErrorMessage());
            return colorResult;
        }
        for (const PDFRgbToCmykUnsupportedItem& item : colorReport.unsupported)
        {
            report->blockers.append(item.reason);
        }
    }

    const bool flattenTransparency = flattensTransparency(settings);
    if (flattenTransparency && PDFTransparencyFlattener::hasLiveTransparency(document))
    {
        report->changes.append({ QStringLiteral("transparency.flatten"), QStringLiteral("live transparency"), QStringLiteral("flattened to opaque raster content") });
    }

    if (isPDFX(settings.target))
    {
        PDFDocument copy = *document;
        PDFDocumentSession session(&copy);
        PreflightEngine engine(&session);
        const PreflightResult preflight = engine.run(pdfxProfile(settings.target));
        report->preflightBefore = preflight.toJson();
        collectPreflightBlockers(settings, preflight, report);
    }
    return report->blockers.isEmpty() ? PDFOperationResult(true)
                                      : PDFOperationResult(QStringLiteral("Standard conversion has unsupported blockers."));
}

PDFOperationResult PDFStandardConversion::prepare(PDFDocument* document,
                                                  const PDFStandardConversionSettings& settings,
                                                  PDFStandardConversionReport* report)
{
    PDFStandardConversionReport localReport;
    report = report ? report : &localReport;
    const PDFOperationResult previewResult = preview(document, settings, report);
    if (!previewResult)
    {
        return previewResult;
    }
    if (settings.dryRunOnly)
    {
        return true;
    }

    if (settings.target != PDFStandardTarget::PDFA2b)
    {
        report->warnings.append(QStringLiteral("PDF/X conversion is unqualified: no supported independent PDF/X oracle."));
        return PDFOperationResult(QStringLiteral("No qualified PDF/X oracle; no output was committed."));
    }
    if (settings.independentValidatorProgram.isEmpty())
    {
        return PDFOperationResult(QStringLiteral("An independent validator is required; no output was committed."));
    }
    PDFDocument candidate = *document;
    // Transparency flattening emits DeviceRGB page rasters. Run it before
    // normalization so the generated image XObjects are converted by the same
    // CMYK fixup as the source document's color content.
    const bool flattenTransparency = flattensTransparency(settings);
    if (flattenTransparency && PDFTransparencyFlattener::hasLiveTransparency(&candidate))
    {
        PDFTransparencyFlattenSettings transparencySettings = settings.transparencyFlattenSettings;
        transparencySettings.analyzeOnly = false;
        PDFTransparencyFlattenReport transparencyReport;
        const PDFOperationResult transparencyResult = PDFTransparencyFlattener::apply(&candidate, transparencySettings, &transparencyReport);
        report->transparencyFlatten = transparencyReport.toJson();
        if (!transparencyResult)
        {
            return transparencyResult;
        }
    }

    const bool normalizeColor = settings.normalizeColor || normalizesColorByDefault(settings.target);
    if (normalizeColor)
    {
        PDFRgbToCmykSettings colorSettings;
        colorSettings.targetIccData = settings.outputIntentIccData;
        colorSettings.targetIccId = settings.outputIntentIccId;
        colorSettings.targetProfileName = settings.outputIntentName;
        colorSettings.blackPointCompensation = settings.blackPointCompensation;
        colorSettings.embedOutputIntent = false;
        colorSettings.revalidate = false;
        PDFRgbToCmykReport colorReport;
        const PDFOperationResult colorResult = PDFRgbToCmykFixup::writeRgbToCmyk(&candidate, colorSettings, &colorReport);
        if (!colorResult)
        {
            return colorResult;
        }
    }

    PDFDocumentBuilder builder(&candidate);
    const PDFVersion version = minimumVersion(settings.target);
    addVersion(&builder, version);
    addOutputIntent(&builder, settings);
    builder.setCatalogMetadata(xmpForTarget(settings.target));

    for (size_t index = 0; index < candidate.getCatalog()->getPageCount(); ++index)
    {
        const PDFPage* page = candidate.getCatalog()->getPage(index);
        if (!page)
        {
            continue;
        }
        builder.setPageTrimBox(page->getPageReference(), page->getTrimBox());
        builder.setPageBleedBox(page->getPageReference(), page->getBleedBox());
    }
    candidate = builder.build();

    report->warnings.append(QStringLiteral("Prepared only. Independent validation is required on the final serialized bytes."));

    *document = qMove(candidate);
    report->conversionAttempted = true;
    return true;
}

PDFStandardConversionSettings standardConversionSettings(const QJsonObject& parameters)
{
    PDFStandardConversionSettings settings;
    pdfStandardTargetFromString(parameters.value(QStringLiteral("target")).toString(), &settings.target);
    settings.outputIntentIccData = QByteArray::fromBase64(parameters.value(QStringLiteral("target_icc_base64")).toString().toLatin1());
    settings.outputIntentIccId = parameters.value(QStringLiteral("target_icc_id")).toString(QStringLiteral("loop-output-intent")).toUtf8();
    settings.outputIntentName = parameters.value(QStringLiteral("target_profile_name")).toString();
    settings.normalizeColor = parameters.contains(QStringLiteral("normalize_color"))
                                  ? parameters.value(QStringLiteral("normalize_color")).toBool()
                                  : (settings.target == PDFStandardTarget::PDFX1a2001 || settings.target == PDFStandardTarget::PDFX3_2002);
    settings.blackPointCompensation = parameters.value(QStringLiteral("black_point_compensation")).toBool(true);
    settings.transparencyFlatten = parameters.contains(QStringLiteral("flatten_transparency"))
                                       ? (parameters.value(QStringLiteral("flatten_transparency")).toBool()
                                              ? PDFTransparencyFlattenPolicy::Always
                                              : PDFTransparencyFlattenPolicy::Never)
                                       : PDFTransparencyFlattenPolicy::Automatic;
    settings.independentValidatorProgram = parameters.value(QStringLiteral("validator_program")).toString();
    const QJsonValue validatorArguments = parameters.value(QStringLiteral("validator_arguments"));
    if (validatorArguments.isArray())
    {
        for (const QJsonValue& value : validatorArguments.toArray())
            settings.independentValidatorArguments.append(value.toString());
    }
    else
    {
        settings.independentValidatorArguments = QProcess::splitCommand(validatorArguments.toString());
    }
    settings.independentValidatorTimeoutMs = qBound(1000, parameters.value(QStringLiteral("validator_timeout_ms")).toInt(120000), 3600000);
    settings.dryRunOnly = parameters.value(QStringLiteral("dry_run_only")).toBool(false);
    return settings;
}

QJsonObject PDFArtifactValidationResult::toJson() const
{
    QJsonObject result = evidence;
    result.insert(QStringLiteral("schema_version"), 2);
    result.insert(QStringLiteral("target"), target);
    result.insert(QStringLiteral("artifact_sha256"), artifactSha256);
    result.insert(QStringLiteral("artifact_bytes"), artifactBytes);
    result.insert(QStringLiteral("program"), validatorProgram);
    result.insert(QStringLiteral("version"), validatorVersion);
    result.insert(QStringLiteral("scope"), QStringLiteral("exact-final-artifact-bytes"));
    result.insert(QStringLiteral("limitations"), QJsonArray{ QStringLiteral("PDF/A-2b only; preparation and PDF/X inspection do not certify conformance.") });
    result.insert(QStringLiteral("report_sha256"), reportSha256);
    result.insert(QStringLiteral("reason_code"), reason);
    result.insert(QStringLiteral("status"), status == PDFArtifactValidationStatus::Passed ? QStringLiteral("passed") : status == PDFArtifactValidationStatus::Rejected ? QStringLiteral("rejected")
                                                                                                                                                                       : QStringLiteral("incomplete"));
    return result;
}

PDFArtifactValidationResult PDFStandardConversion::validateArtifact(const QByteArray& bytes,
                                                                    const PDFStandardConversionSettings& settings,
                                                                    const PDFOperationControl* control)
{
    PDFArtifactValidationResult result;
    result.target = pdfStandardTargetToString(settings.target);
    result.validatorProgram = settings.independentValidatorProgram;
    result.artifactBytes = bytes.size();
    result.artifactSha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    result.reason = QStringLiteral("validator-unavailable");
    if (PDFOperationControl::isOperationCancelled(control))
    {
        result.reason = QStringLiteral("validator-cancelled");
        return result;
    }
    if (settings.target != PDFStandardTarget::PDFA2b)
    {
        result.reason = QStringLiteral("unsupported-pdfx-oracle");
        return result;
    }
    const QString programName = QFileInfo(settings.independentValidatorProgram).completeBaseName().toLower();
    if (programName != QLatin1String("verapdf") || bytes.isEmpty() || settings.independentValidatorTimeoutMs <= 0)
    {
        return result;
    }
    // Arguments are fixed by the adapter: callers cannot switch the target or disable validation.
    const QStringList configured = settings.independentValidatorArguments;
    if (!configured.isEmpty() && configured != QStringList{ QStringLiteral("{input}") } &&
        configured != QStringList{ QStringLiteral("--format"), QStringLiteral("xml"), QStringLiteral("--flavour"), QStringLiteral("2b"), QStringLiteral("{input}") })
    {
        result.reason = QStringLiteral("validator-arguments-unsupported");
        return result;
    }
    const auto invoke = [&](const QStringList& arguments, QByteArray* output, QByteArray* errors, int* exitCode) -> bool
    {
        QProcess process;
        PDFSysUtils::configureScriptOrProgramProcess(process, settings.independentValidatorProgram, arguments);
        process.start();
        QElapsedTimer timer;
        timer.start();
        constexpr qsizetype maxReportBytes = 16 * 1024 * 1024;
        while (true)
        {
            process.waitForFinished(50);
            output->append(process.readAllStandardOutput());
            errors->append(process.readAllStandardError());
            if (PDFOperationControl::isOperationCancelled(control) || timer.elapsed() > settings.independentValidatorTimeoutMs ||
                output->size() > maxReportBytes || errors->size() > maxReportBytes)
            {
                result.reason = PDFOperationControl::isOperationCancelled(control) ? QStringLiteral("validator-cancelled") : timer.elapsed() > settings.independentValidatorTimeoutMs ? QStringLiteral("validator-timeout")
                                                                                                                                                                                      : QStringLiteral("validator-report-too-large");
                process.kill();
                process.waitForFinished(1000);
                return false;
            }
            if (process.state() == QProcess::NotRunning)
            {
                break;
            }
        }
        if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit)
        {
            result.reason = QStringLiteral("validator-invocation-failed");
            return false;
        }
        *exitCode = process.exitCode();
        return true;
    };
    QByteArray versionOutput, versionErrors;
    int exitCode = -1;
    if (!invoke({ QStringLiteral("--version") }, &versionOutput, &versionErrors, &exitCode))
    {
        return result;
    }
    const QRegularExpression versionPattern(QStringLiteral("veraPDF(?: CLI)? (?:version )?1\\.(?:24|26|28|30)\\.\\d+"), QRegularExpression::CaseInsensitiveOption);
    const auto versionMatch = versionPattern.match(QString::fromUtf8(versionOutput + versionErrors));
    if (exitCode != 0 || !versionMatch.hasMatch())
    {
        result.reason = QStringLiteral("validator-version-unsupported");
        return result;
    }
    result.validatorVersion = versionMatch.captured();
    QTemporaryDir directory;
    const QString inputPath = directory.filePath(QStringLiteral("candidate.pdf"));
    QFile input(inputPath);
    if (!directory.isValid() || !input.open(QIODevice::WriteOnly) || input.write(bytes) != bytes.size())
    {
        result.reason = QStringLiteral("validator-input-write-failed");
        return result;
    }
    input.close();
    const QStringList arguments{ QStringLiteral("--format"), QStringLiteral("xml"), QStringLiteral("--flavour"), QStringLiteral("2b"), inputPath };
    QByteArray output, errors;
    if (!invoke(arguments, &output, &errors, &exitCode))
    {
        return result;
    }
    result.reportSha256 = QString::fromLatin1(QCryptographicHash::hash(output, QCryptographicHash::Sha256).toHex());
    result.evidence = QJsonObject{ { QStringLiteral("program"), settings.independentValidatorProgram },
                                   { QStringLiteral("arguments"), QJsonArray::fromStringList(arguments) },
                                   { QStringLiteral("exit_code"), exitCode },
                                   { QStringLiteral("stdout"), QString::fromUtf8(output) },
                                   { QStringLiteral("stderr"), QString::fromUtf8(errors) } };
    if (!input.open(QIODevice::ReadOnly) || input.readAll() != bytes)
    {
        result.status = PDFArtifactValidationStatus::Rejected;
        result.reason = QStringLiteral("validator-input-mutated");
        return result;
    }
    QXmlStreamReader xml(output);
    int jobs = 0, reports = 0, items = 0, details = 0, summaries = 0, totals = 0;
    bool completeCounts = false, completeSummary = false, completeTotals = false;
    QString profile, compliant, itemName;
    bool exception = false;
    QStringList stack;
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isStartElement())
        {
            stack.append(xml.name().toString());
            const QString path = stack.join(QLatin1Char('/'));
            if (path == QLatin1String("report/jobs/job"))
                ++jobs;
            if (path == QLatin1String("report/jobs/job/item"))
                ++items;
            if (path == QLatin1String("report/jobs/job/item/name"))
            {
                itemName = xml.readElementText();
                stack.removeLast();
            }
            if (path == QLatin1String("report/jobs/job/validationReport"))
            {
                ++reports;
                const auto endStatus = xml.attributes().value(QLatin1String("jobEndStatus"));
                if (!endStatus.isEmpty() && endStatus != QLatin1String("normal"))
                    exception = true;
                profile = xml.attributes().value(QLatin1String("profileName")).toString();
                compliant = xml.attributes().value(QLatin1String("isCompliant")).toString();
            }
            if (path == QLatin1String("report/jobs/job/validationReport/details"))
            {
                ++details;
                const auto attributes = xml.attributes();
                const auto count = [&](const char* name, bool positive)
                {
                    const QString value = attributes.value(QLatin1String(name)).toString();
                    bool valid = false;
                    const qulonglong number = value.toULongLong(&valid);
                    return valid && QRegularExpression(QStringLiteral("^[0-9]+$")).match(value).hasMatch() &&
                           (positive ? number > 0 : number == 0);
                };
                completeCounts = count("passedRules", true) && count("passedChecks", true) &&
                                 count("failedRules", false) && count("failedChecks", false);
            }
            if (path == QLatin1String("report/batchSummary"))
            {
                ++summaries;
                const auto attributes = xml.attributes();
                completeSummary = attributes.value(QLatin1String("totalJobs")) == QLatin1String("1") &&
                                  attributes.value(QLatin1String("failedToParse")) == QLatin1String("0") &&
                                  attributes.value(QLatin1String("encrypted")) == QLatin1String("0") &&
                                  attributes.value(QLatin1String("outOfMemory")) == QLatin1String("0") &&
                                  attributes.value(QLatin1String("veraExceptions")) == QLatin1String("0");
            }
            if (path == QLatin1String("report/batchSummary/validationReports"))
            {
                ++totals;
                const auto attributes = xml.attributes();
                const QString total = xml.readElementText();
                completeTotals = attributes.value(QLatin1String("compliant")) == QLatin1String("1") &&
                                 attributes.value(QLatin1String("nonCompliant")) == QLatin1String("0") &&
                                 attributes.value(QLatin1String("failedJobs")) == QLatin1String("0") &&
                                 total == QLatin1String("1");
                stack.removeLast();
            }
            if (xml.name().toString().endsWith(QLatin1String("Exception")))
                exception = true;
        }
        else if (xml.isEndElement())
            stack.removeLast();
    }
    if (xml.hasError() || exception || jobs != 1 || reports != 1 || items != 1 || !QFileInfo(itemName).isAbsolute() ||
        QFileInfo(itemName).canonicalFilePath() != QFileInfo(inputPath).canonicalFilePath() ||
        profile.compare(QLatin1String("PDF/A-2B validation profile"), Qt::CaseInsensitive) != 0)
    {
        result.reason = QStringLiteral("validator-report-invalid");
        return result;
    }
    if (compliant == QLatin1String("false"))
    {
        result.status = PDFArtifactValidationStatus::Rejected;
        result.reason = QStringLiteral("validator-rejected");
    }
    else if (compliant == QLatin1String("true") && details == 1 && summaries == 1 && totals == 1 &&
             completeCounts && completeSummary && completeTotals && exitCode == 0)
    {
        result.status = PDFArtifactValidationStatus::Passed;
        result.reason.clear();
    }
    else
        result.reason = QStringLiteral("validator-report-incomplete");
    return result;
}

PDFOperationResult PDFStandardConversion::validateArtifacts(const QByteArray& bytes,
                                                            const QList<PDFStandardConversionSettings>& requirements,
                                                            QJsonArray* evidence,
                                                            const PDFOperationControl* control)
{
    if (evidence)
        *evidence = {};
    if (PDFOperationControl::isOperationCancelled(control))
        return PDFOperationResult(QStringLiteral("Artifact validation was cancelled."));
    for (const auto& settings : requirements)
    {
        const auto result = validateArtifact(bytes, settings, control);
        if (evidence)
            evidence->append(result.toJson());
        if (result.status != PDFArtifactValidationStatus::Passed)
            return PDFOperationResult(QStringLiteral("Independent validation did not pass: %1").arg(result.reason));
    }
    return PDFOperationResult(true);
}

PDFOperationResult PDFStandardConversion::writeCandidate(const PDFDocument& document,
                                                         const QString& path,
                                                         const QList<PDFStandardConversionSettings>& requirements,
                                                         PDFDocument* reopened,
                                                         QByteArray* bytes,
                                                         QJsonArray* evidence,
                                                         const PDFOperationControl* control)
{
    if (evidence)
        *evidence = {};
    QByteArray candidateBytes;
    QBuffer buffer(&candidateBytes);
    buffer.open(QIODevice::WriteOnly);
    PDFDocumentWriter writer(nullptr, control);
    const auto serialized = writer.write(&buffer, &document);
    if (!serialized)
        return serialized;
    const auto validated = validateArtifacts(candidateBytes, requirements, evidence, control);
    if (!validated)
        return validated;
    PDFDocumentReader reader(nullptr, [](bool*)
                             { return QString(); }, false, false);
    const PDFDocument candidate = reader.readFromBuffer(candidateBytes);
    if (reader.getReadingResult() != PDFDocumentReader::Result::OK)
        return PDFOperationResult(QStringLiteral("Serialized candidate could not be reopened."));
    if (PDFOperationControl::isOperationCancelled(control))
        return PDFOperationResult(QStringLiteral("Artifact publication was cancelled."));
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return PDFOperationResult(QStringLiteral("Candidate destination directory is unavailable."));
    const auto written = PDFSafeFileWriter::writeData(path, candidateBytes, PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!written)
        return written;
    QFile published(path);
    if (!published.open(QIODevice::ReadOnly) || published.readAll() != candidateBytes)
        return PDFOperationResult(QStringLiteral("Published artifact identity does not match validation."));
    if (reopened)
        *reopened = candidate;
    if (bytes)
        *bytes = candidateBytes;
    return PDFOperationResult(true);
}

}   // namespace pdf
