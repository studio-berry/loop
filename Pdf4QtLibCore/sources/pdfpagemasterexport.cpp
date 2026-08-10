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

#include "pdfpagemasterexport.h"
#include "pdfdocumentwriter.h"
#include "pdfsafefilewriter.h"
#include "pdfprogress.h"
#include "preflightengine.h"
#include "pdfdocumentsession.h"
#include "pdfcontourbleedfixup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <utility>

namespace pdf
{

namespace
{

constexpr int MANIFEST_SCHEMA_VERSION = 2;
constexpr QLatin1String MANIFEST_FILE_NAME(".loupe-batch.json");
constexpr QLatin1String OUTPUT_STATUS_PENDING("pending");
constexpr QLatin1String OUTPUT_STATUS_WRITTEN("written");
constexpr QLatin1String OUTPUT_STATUS_FAILED("failed");

bool isCancelRequested(const PDFPageMasterExportJob& job)
{
    return job.cancelFlag && job.cancelFlag->load(std::memory_order_acquire);
}

PDFProgress* activeProgress(const PDFPageMasterExportJob& job)
{
    if (job.progressAlive && !job.progressAlive->load(std::memory_order_acquire))
    {
        return nullptr;
    }
    return job.progress;
}

PDFPageMasterExportResult createExportError(QString message, QStringList writtenFiles = {}, QString manifestPath = {}, QJsonObject manifest = {})
{
    PDFPageMasterExportResult result;
    result.success = false;
    result.errorMessage = std::move(message);
    result.writtenFiles = std::move(writtenFiles);
    result.manifestPath = std::move(manifestPath);
    result.manifest = std::move(manifest);
    return result;
}

PDFPageMasterExportResult createExportCancelled(QStringList writtenFiles = {}, QString manifestPath = {}, QJsonObject manifest = {})
{
    PDFPageMasterExportResult result;
    result.success = false;
    result.cancelled = true;
    result.writtenFiles = std::move(writtenFiles);
    result.manifestPath = std::move(manifestPath);
    result.manifest = std::move(manifest);
    return result;
}

void finishProgressIfActive(PDFProgress* progress)
{
    if (progress)
    {
        progress->finish();
    }
}

QString resolveManifestPath(const PDFPageMasterExportJob& job)
{
    if (!job.manifestPath.isEmpty())
    {
        return job.manifestPath;
    }

    if (job.outputFileNames.empty())
    {
        return {};
    }

    const QFileInfo firstOutput(job.outputFileNames.front());
    return QDir(firstOutput.absolutePath()).filePath(QString(MANIFEST_FILE_NAME));
}

QStringList plannedOutputPaths(const PDFPageMasterExportJob& job, const QString& manifestPath)
{
    QStringList paths;
    paths.reserve(int(job.outputFileNames.size()) * 3 + (manifestPath.isEmpty() ? 0 : 1));
    for (const QString& outputPath : job.outputFileNames)
    {
        paths.append(outputPath);
        if (job.hasPreflightGate && (!job.preflightProfilePath.isEmpty() || job.hasPreflightContext))
        {
            paths.append(outputPath + QStringLiteral(".preflight.json"));
            if (job.revalidatePreflightAfterFixups)
            {
                paths.append(outputPath + QStringLiteral(".preflight-final.json"));
            }
        }
    }
    if (!manifestPath.isEmpty())
    {
        paths.append(manifestPath);
    }
    return paths;
}

QString outputConflictMessage(const PDFOutputConflict& conflict)
{
    if (conflict.code == QStringLiteral("output.duplicate-planned-path"))
    {
        return QCoreApplication::translate("pdf::PDFPageMasterExport",
                                           "Output path '%1' is planned more than once.").arg(conflict.path);
    }
    if (conflict.code == QStringLiteral("output.destination-is-directory"))
    {
        return QCoreApplication::translate("pdf::PDFPageMasterExport",
                                           "Output path '%1' is a directory.").arg(conflict.path);
    }
    return QCoreApplication::translate("pdf::PDFPageMasterExport",
                                       "Output path '%1' already exists.").arg(conflict.path);
}

QJsonObject productionReport(const PDFPageMasterProductionSettings& settings,
                             const PDFDocument& document)
{
    const PDFProductionValidationReport validation = validateProductionGeometry(settings.geometry);
    QJsonObject report = validation.toJson();
    report.insert(QStringLiteral("schema"), QStringLiteral("loupe-production-report/1"));
    report.insert(QStringLiteral("contourBleedEnabled"), settings.contourBleedEnabled);
    report.insert(QStringLiteral("grommetsEnabled"), settings.grommetsEnabled);

    QJsonArray contourBleedPlans;
    if (settings.contourBleedEnabled)
    {
        for (const PDFProductionContour& contour : settings.geometry.contours)
        {
            const PDFContourBleedPlan plan = planContourBleed(contour, settings.contourBleed);
            contourBleedPlans.append(plan.toJson());
            report.insert(QStringLiteral("valid"), report.value(QStringLiteral("valid")).toBool() && plan.valid);
        }
    }
    report.insert(QStringLiteral("contourBleedPlans"), contourBleedPlans);

    QJsonArray grommetPages;
    if (settings.grommetsEnabled && document.getCatalog())
    {
        for (size_t index = 0; index < document.getCatalog()->getPageCount(); ++index)
        {
            const PDFPage* page = document.getCatalog()->getPage(index);
            if (!page)
            {
                continue;
            }
            const QRectF productionRect = page->getTrimBox().isValid() ? page->getTrimBox() : page->getCropBox();
            QJsonObject pageReport = placeGrommets(productionRect, settings.grommets).toJson();
            pageReport.insert(QStringLiteral("page"), int(index + 1));
            const QJsonArray diagnostics = pageReport.value(QStringLiteral("diagnostics")).toArray();
            for (const QJsonValue& diagnostic : diagnostics)
            {
                if (diagnostic.toObject().value(QStringLiteral("severity")).toString() == QStringLiteral("error"))
                {
                    report.insert(QStringLiteral("valid"), false);
                    break;
                }
            }
            grommetPages.append(pageReport);
        }
    }
    report.insert(QStringLiteral("grommetPages"), grommetPages);
    return report;
}

QString normalizedOutputPath(const QString& path)
{
    QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    normalized = normalized.toCaseFolded();
#endif
    return normalized;
}

bool writeFileAtomically(const QString& finalPath, const QByteArray& payload)
{
    const PDFOperationResult result = PDFSafeFileWriter::writeData(finalPath, payload, PDFSafeFileWriter::OverwritePolicy::Overwrite);
    return static_cast<bool>(result);
}

QJsonObject createManifestObject(const QString& batchId, const QStringList& outputFileNames)
{
    QJsonArray outputs;
    for (const QString& path : outputFileNames)
    {
        outputs.append(QJsonObject{
            { QStringLiteral("path"), path },
            { QStringLiteral("status"), QString(OUTPUT_STATUS_PENDING) }
        });
    }

    return QJsonObject{
        { QStringLiteral("schema_version"), MANIFEST_SCHEMA_VERSION },
        { QStringLiteral("batch_id"), batchId },
        { QStringLiteral("outputs"), outputs }
    };
}

bool persistManifest(const QString& manifestPath, const QJsonObject& manifest)
{
    const QByteArray payload = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    return writeFileAtomically(manifestPath, payload);
}

QString outputStatusAt(const QJsonObject& manifest, int index)
{
    const QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    if (index < 0 || index >= outputs.size())
    {
        return {};
    }

    return outputs.at(index).toObject().value(QStringLiteral("status")).toString();
}

void setOutputStatus(QJsonObject& manifest, int index, const QString& status, const QString& error = {})
{
    QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    if (index < 0 || index >= outputs.size())
    {
        return;
    }

    QJsonObject entry = outputs.at(index).toObject();
    entry.insert(QStringLiteral("status"), status);
    if (error.isEmpty())
    {
        entry.remove(QStringLiteral("error"));
    }
    else
    {
        entry.insert(QStringLiteral("error"), error);
    }
    outputs.replace(index, entry);
    manifest.insert(QStringLiteral("outputs"), outputs);
}

QString bleedSideName(PDFBleedFixupSide side)
{
    switch (side)
    {
        case PDFBleedFixupSide::Left: return QStringLiteral("left");
        case PDFBleedFixupSide::Right: return QStringLiteral("right");
        case PDFBleedFixupSide::Top: return QStringLiteral("top");
        case PDFBleedFixupSide::Bottom: return QStringLiteral("bottom");
    }
    return QStringLiteral("unknown");
}

QJsonObject bleedFixupReportToJson(const PDFBleedFixupReport& report)
{
    QJsonArray pages;
    for (const PDFBleedFixupPageReport& page : report.pages)
    {
        QStringList sidesRequested;
        QStringList sidesEligible;
        QStringList sidesApplied;
        for (PDFBleedFixupSide side : { PDFBleedFixupSide::Left,
                                        PDFBleedFixupSide::Bottom,
                                        PDFBleedFixupSide::Right,
                                        PDFBleedFixupSide::Top })
        {
            if (isBleedFixupSideEnabled(page.sidesRequested, side))
            {
                sidesRequested.append(bleedSideName(side));
            }
            if (isBleedFixupSideEnabled(page.sidesEligible, side))
            {
                sidesEligible.append(bleedSideName(side));
            }
        }
        for (PDFBleedFixupSide side : page.sidesApplied)
        {
            sidesApplied.append(bleedSideName(side));
        }

        const bool eligible = !sidesEligible.isEmpty();
        const bool applied = !sidesApplied.isEmpty();
        pages.append(QJsonObject{
            { QStringLiteral("page"), int(page.pageIndex + 1) },
            { QStringLiteral("sides_requested"), QJsonArray::fromStringList(sidesRequested) },
            { QStringLiteral("sides_eligible"), QJsonArray::fromStringList(sidesEligible) },
            { QStringLiteral("sides_applied"), QJsonArray::fromStringList(sidesApplied) },
            { QStringLiteral("skip_reasons"), QJsonArray::fromStringList(page.skipReasons) },
            { QStringLiteral("eligible"), eligible },
            { QStringLiteral("applied"), applied }
        });
    }

    bool eligible = false;
    bool applied = false;
    bool partial = false;
    for (const QJsonValue& value : pages)
    {
        const QJsonObject page = value.toObject();
        const bool pageEligible = page.value(QStringLiteral("eligible")).toBool();
        const bool pageApplied = page.value(QStringLiteral("applied")).toBool();
        eligible = eligible || pageEligible;
        applied = applied || pageApplied;
        partial = partial || (pageEligible && !pageApplied);
    }

    QString status = QStringLiteral("not-needed");
    if (eligible && applied)
    {
        status = partial ? QStringLiteral("partial") : QStringLiteral("applied");
    }
    else if (eligible)
    {
        status = QStringLiteral("eligible");
    }

    return QJsonObject{
        { QStringLiteral("pages"), pages },
        { QStringLiteral("eligible"), eligible },
        { QStringLiteral("applied"), applied },
        { QStringLiteral("status"), status }
    };
}

void setOutputBleedReport(QJsonObject& manifest, int index, const PDFBleedFixupReport& report)
{
    QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    if (index < 0 || index >= outputs.size())
    {
        return;
    }

    QJsonObject entry = outputs.at(index).toObject();
    entry.insert(QStringLiteral("bleed_report"), bleedFixupReportToJson(report));
    outputs.replace(index, entry);
    manifest.insert(QStringLiteral("outputs"), outputs);
}

void setOutputBleedFailure(QJsonObject& manifest, int index, const QString& error)
{
    QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    if (index < 0 || index >= outputs.size())
    {
        return;
    }

    QJsonObject entry = outputs.at(index).toObject();
    entry.insert(QStringLiteral("bleed_report"), QJsonObject{
        { QStringLiteral("eligible"), QJsonValue::Null },
        { QStringLiteral("applied"), false },
        { QStringLiteral("status"), QStringLiteral("failed") },
        { QStringLiteral("error"), error }
    });
    outputs.replace(index, entry);
    manifest.insert(QStringLiteral("outputs"), outputs);
}

void setOutputPreflightReport(QJsonObject& manifest, int index, const QString& stage, const QJsonObject& report)
{
    QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    if (index < 0 || index >= outputs.size())
    {
        return;
    }

    QJsonObject entry = outputs.at(index).toObject();
    QJsonObject preflight = entry.value(QStringLiteral("preflight")).toObject();
    preflight.insert(stage, report);
    entry.insert(QStringLiteral("preflight"), preflight);
    outputs.replace(index, entry);
    manifest.insert(QStringLiteral("outputs"), outputs);
}

bool writePreflightReport(const QString& outputPath, const QJsonObject& report)
{
    const QString reportPath = outputPath + QStringLiteral(".preflight.json");
    const QByteArray payload = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return writeFileAtomically(reportPath, payload);
}

bool loadExistingManifest(const QString& manifestPath, QJsonObject* manifest, QString* errorMessage)
{
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly))
    {
        if (errorMessage)
        {
            *errorMessage = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                          "Could not read batch manifest at %1.").arg(manifestPath);
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (errorMessage)
        {
            *errorMessage = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                      "Batch manifest at %1 is not valid JSON.").arg(manifestPath);
        }
        return false;
    }

    *manifest = document.object();
    return true;
}

bool manifestCompatibleWithJob(const QJsonObject& manifest, const PDFPageMasterExportJob& job)
{
    const QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    if (outputs.size() != int(job.outputFileNames.size()))
    {
        return false;
    }

    for (int index = 0; index < outputs.size(); ++index)
    {
        const QString manifestPath = outputs.at(index).toObject().value(QStringLiteral("path")).toString();
        if (normalizedOutputPath(manifestPath) != normalizedOutputPath(job.outputFileNames[size_t(index)]))
        {
            return false;
        }
    }

    return true;
}

int findOutputIndexByPath(const QJsonObject& manifest, const QString& fileName)
{
    const QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    const QString absolutePath = normalizedOutputPath(fileName);
    for (int index = 0; index < outputs.size(); ++index)
    {
        const QString entryPath = outputs.at(index).toObject().value(QStringLiteral("path")).toString();
        if (normalizedOutputPath(entryPath) == absolutePath)
        {
            return index;
        }
    }

    return -1;
}

bool shouldSkipResumedOutput(const PDFPageMasterExportJob& job, const QJsonObject& manifest, const QString& fileName)
{
    if (!job.resume)
    {
        return false;
    }

    const int index = findOutputIndexByPath(manifest, fileName);
    if (index < 0)
    {
        return false;
    }

    const QString status = outputStatusAt(manifest, index);
    return status == OUTPUT_STATUS_WRITTEN && QFile::exists(fileName);
}

} // namespace

QJsonObject PDFPageMasterProductionSettings::toJson() const
{
    return QJsonObject{
        { QStringLiteral("enabled"), enabled },
        { QStringLiteral("geometry"), geometry.toJson() },
        { QStringLiteral("contourBleedEnabled"), contourBleedEnabled },
        { QStringLiteral("contourBleed"), QJsonObject{
            { QStringLiteral("amountPt"), contourBleed.amountPt },
            { QStringLiteral("flatteningTolerancePt"), contourBleed.flatteningTolerancePt },
            { QStringLiteral("maxSegments"), contourBleed.maxSegments },
            { QStringLiteral("dpi"), contourBleedDpi },
            { QStringLiteral("maxRasterPixels"), double(contourBleedMaxRasterPixels) }
        } },
        { QStringLiteral("grommetsEnabled"), grommetsEnabled },
        { QStringLiteral("grommets"), grommets.toJson() }
    };
}

PDFPageMasterProductionSettings PDFPageMasterProductionSettings::fromJson(const QJsonObject& object)
{
    PDFPageMasterProductionSettings settings;
    settings.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    settings.geometry = PDFProductionGeometryModel::fromJson(object.value(QStringLiteral("geometry")).toObject());
    settings.contourBleedEnabled = object.value(QStringLiteral("contourBleedEnabled")).toBool(false);
    const QJsonObject contourBleed = object.value(QStringLiteral("contourBleed")).toObject();
    settings.contourBleed.amountPt = contourBleed.value(QStringLiteral("amountPt")).toDouble(settings.contourBleed.amountPt);
    settings.contourBleed.flatteningTolerancePt = contourBleed.value(QStringLiteral("flatteningTolerancePt")).toDouble(settings.contourBleed.flatteningTolerancePt);
    settings.contourBleed.maxSegments = contourBleed.value(QStringLiteral("maxSegments")).toInt(settings.contourBleed.maxSegments);
    settings.contourBleedDpi = contourBleed.value(QStringLiteral("dpi")).toInt(settings.contourBleedDpi);
    settings.contourBleedMaxRasterPixels = qint64(contourBleed.value(QStringLiteral("maxRasterPixels")).toDouble(double(settings.contourBleedMaxRasterPixels)));
    settings.grommetsEnabled = object.value(QStringLiteral("grommetsEnabled")).toBool(false);
    settings.grommets = PDFGrommetSpec::fromJson(object.value(QStringLiteral("grommets")).toObject());
    return settings;
}

PDFPageMasterExportResult PDFPageMasterExport::run(PDFPageMasterExportJob job)
{
    const auto persistManifestForJob = [&job](const QString& path, const QJsonObject& value)
    {
        return job.manifestPersist ? job.manifestPersist(path, value) : persistManifest(path, value);
    };

    if (isCancelRequested(job))
    {
        return createExportCancelled();
    }

    PDFDocumentManipulator manipulator;
    manipulator.setOutlineMode(job.outlineMode);

    for (const auto& documentItem : job.documents)
    {
        manipulator.addDocument(documentItem.first, &documentItem.second);
    }
    for (const auto& imageItem : job.images)
    {
        manipulator.addImage(imageItem.first, imageItem.second);
    }

    if (job.assembledDocuments.size() != job.outputFileNames.size())
    {
        return createExportError(QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                             "Export job has %1 assembled output(s) but %2 output filename(s).")
                                     .arg(job.assembledDocuments.size())
                                     .arg(job.outputFileNames.size()));
    }

    if (job.hasBleedFixupSettings
        && job.bleedConfirmationPolicy == PDFPageMasterBleedConfirmationPolicy::BeforeBatch
        && !job.bleedConfirmationGranted)
    {
        return createExportError(QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                             "Bleed fixup confirmation is required before starting the export batch."));
    }

    if (job.hasProductionGeometrySettings && !job.productionGeometrySettings.enabled)
    {
        job.hasProductionGeometrySettings = false;
    }

    const QString manifestPath = resolveManifestPath(job);
    const QStringList plannedPaths = plannedOutputPaths(job, manifestPath);
    for (const PDFOutputConflict& conflict : PDFSafeFileWriter::findOutputConflicts(plannedPaths, false))
    {
        return createExportError(outputConflictMessage(conflict));
    }

    QJsonObject manifest;
    QString batchId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    if (job.resume && !manifestPath.isEmpty() && QFile::exists(manifestPath))
    {
        QString manifestError;
        if (!loadExistingManifest(manifestPath, &manifest, &manifestError))
        {
            return createExportError(std::move(manifestError));
        }

        if (manifest.value(QStringLiteral("schema_version")).toInt(-1) != MANIFEST_SCHEMA_VERSION)
        {
            return createExportError(QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                 "Batch manifest schema is incompatible with this PageMaster export version."));
        }

        if (!manifestCompatibleWithJob(manifest, job))
        {
            // Stale or mismatched batch — start a fresh manifest for this job.
            job.resume = false;
            for (const PDFOutputConflict& conflict : PDFSafeFileWriter::findOutputConflicts(plannedPaths, !job.overwriteFiles))
            {
                return createExportError(outputConflictMessage(conflict));
            }
            manifest = createManifestObject(batchId, QStringList(job.outputFileNames.begin(), job.outputFileNames.end()));
            if (!persistManifestForJob(manifestPath, manifest))
            {
                return createExportError(QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                     "Could not write batch manifest at %1.").arg(manifestPath));
            }
        }
        else
        {
            batchId = manifest.value(QStringLiteral("batch_id")).toString(batchId);
        }
    }
    else
    {
        job.resume = false;
        for (const PDFOutputConflict& conflict : PDFSafeFileWriter::findOutputConflicts(plannedPaths, !job.overwriteFiles))
        {
            return createExportError(outputConflictMessage(conflict));
        }
        manifest = createManifestObject(batchId, QStringList(job.outputFileNames.begin(), job.outputFileNames.end()));
        if (!manifestPath.isEmpty() && !persistManifestForJob(manifestPath, manifest))
        {
            return createExportError(QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                 "Could not write batch manifest at %1.").arg(manifestPath));
        }
    }

    PDFPageMasterExportResult result;
    result.manifestPath = manifestPath;
    result.manifest = manifest;
    result.success = true;
    result.writtenFiles.reserve(int(job.assembledDocuments.size()));

    PDFProgress* progress = activeProgress(job);
    const bool trackProgress = progress && !job.assembledDocuments.empty();
    if (trackProgress)
    {
        ProgressStartupInfo info;
        info.showDialog = true;
        info.text = QCoreApplication::translate("pdf::PDFPageMasterExport", "Exporting documents...");
        progress->start(job.assembledDocuments.size(), std::move(info));
    }

    const bool runPreflight = job.hasPreflightGate && (!job.preflightProfilePath.isEmpty() || job.hasPreflightContext);
    QJsonObject preflightProfile;
    QJsonObject preflightResolution;
    if (runPreflight)
    {
        PreflightProfileResolver resolver;
        PreflightResolvedProfile resolved;
        QString profileError;
        if (!job.preflightProfilePath.isEmpty())
        {
            if (!PreflightEngine::loadProfile(job.preflightProfilePath, preflightProfile, profileError))
            {
                finishProgressIfActive(activeProgress(job));
                return createExportError(std::move(profileError), std::move(result.writtenFiles), manifestPath, manifest);
            }
            resolved = resolver.resolveExplicitProfile(preflightProfile,
                                                       QFileInfo(job.preflightProfilePath).completeBaseName(),
                                                       QStringLiteral("explicit"));
        }
        else
        {
            PreflightProfileSnapshot snapshot;
            if (!PreflightProfileStore::loadDirectory(job.preflightProfileStorePath, snapshot, profileError))
            {
                finishProgressIfActive(activeProgress(job));
                return createExportError(std::move(profileError), std::move(result.writtenFiles), manifestPath, manifest);
            }
            resolved = resolver.resolve(job.preflightContext, snapshot);
        }
        if (!resolved.ok)
        {
            finishProgressIfActive(activeProgress(job));
            return createExportError(resolved.errorMessage, std::move(result.writtenFiles), manifestPath, manifest);
        }
        preflightProfile = resolved.effectiveProfile;
        preflightResolution = resolved.provenance();
    }

    for (size_t index = 0; index < job.assembledDocuments.size(); ++index)
    {
        if (isCancelRequested(job))
        {
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportCancelled(std::move(result.writtenFiles), manifestPath, manifest);
        }

        const QString& fileName = job.outputFileNames[index];

        if (shouldSkipResumedOutput(job, manifest, fileName))
        {
            result.writtenFiles << fileName;
            if (PDFProgress* stepProgress = activeProgress(job))
            {
                stepProgress->step();
            }
            continue;
        }

        PDFOperationResult currentResult = manipulator.assemble(job.assembledDocuments[index]);
        if (!currentResult)
        {
            setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, currentResult.getErrorMessage());
            persistManifestForJob(manifestPath, manifest);
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportError(currentResult.getErrorMessage(), std::move(result.writtenFiles), manifestPath, manifest);
        }

        PDFDocument assembledDocument = manipulator.takeAssembledDocument();

        if (isCancelRequested(job))
        {
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportCancelled(std::move(result.writtenFiles), manifestPath, manifest);
        }

        if (runPreflight)
        {
            PDFDocumentSession session(&assembledDocument);
            PreflightEngine engine(&session);
            PreflightResult preflightResult = engine.run(preflightProfile);
            preflightResult.profileResolution = preflightResolution;
            const QJsonObject preflightReport = preflightResult.toJson(fileName);
            setOutputPreflightReport(manifest, int(index), QStringLiteral("initial"), preflightReport);
            writePreflightReport(fileName, preflightReport);

            if (!preflightResult.pass && !job.forcePreflight)
            {
                const QString message = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                    "Preflight failed for '%1'.").arg(fileName);
                setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, message);
                persistManifestForJob(manifestPath, manifest);
                finishProgressIfActive(activeProgress(job));
                result.manifest = manifest;
                return createExportError(message, std::move(result.writtenFiles), manifestPath, manifest);
            }
        }

        if (isCancelRequested(job))
        {
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportCancelled(std::move(result.writtenFiles), manifestPath, manifest);
        }

        if (job.hasPageGeometrySettings)
        {
            const PDFOperationResult geometryResult = PDFPageGeometry::apply(&assembledDocument, job.pageGeometrySettings);
            if (!geometryResult)
            {
                setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, geometryResult.getErrorMessage());
                persistManifestForJob(manifestPath, manifest);
                finishProgressIfActive(activeProgress(job));
                result.manifest = manifest;
                return createExportError(geometryResult.getErrorMessage(), std::move(result.writtenFiles), manifestPath, manifest);
            }
        }

        if (job.hasProductionGeometrySettings)
        {
            QJsonObject report = productionReport(job.productionGeometrySettings, assembledDocument);
            QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
            QJsonObject output = outputs.at(int(index)).toObject();
            output.insert(QStringLiteral("production"), report);
            outputs.replace(int(index), output);
            manifest.insert(QStringLiteral("outputs"), outputs);
            if (!report.value(QStringLiteral("valid")).toBool(false))
            {
                const QString message = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                    "Production geometry validation failed for '%1'.").arg(fileName);
                setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, message);
                persistManifest(manifestPath, manifest);
                finishProgressIfActive(activeProgress(job));
                result.manifest = manifest;
                return createExportError(message, std::move(result.writtenFiles), manifestPath, manifest);
            }
            if (job.productionGeometrySettings.contourBleedEnabled)
            {
                PDFContourBleedFixupSettings contourBleedSettings;
                contourBleedSettings.amountPt = job.productionGeometrySettings.contourBleed.amountPt;
                contourBleedSettings.flatteningTolerancePt = job.productionGeometrySettings.contourBleed.flatteningTolerancePt;
                contourBleedSettings.maxSegments = job.productionGeometrySettings.contourBleed.maxSegments;
                contourBleedSettings.dpi = job.productionGeometrySettings.contourBleedDpi;
                contourBleedSettings.maxRasterPixels = job.productionGeometrySettings.contourBleedMaxRasterPixels;
                PDFContourBleedFixupReport contourBleedReport;
                const PDFOperationResult contourBleedResult = PDFContourBleedFixup::apply(&assembledDocument,
                                                                                           job.productionGeometrySettings.geometry,
                                                                                           contourBleedSettings,
                                                                                           &contourBleedReport);
                report.insert(QStringLiteral("contourBleedFixup"), contourBleedReport.toJson());
                outputs = manifest.value(QStringLiteral("outputs")).toArray();
                output = outputs.at(int(index)).toObject();
                output.insert(QStringLiteral("production"), report);
                outputs.replace(int(index), output);
                manifest.insert(QStringLiteral("outputs"), outputs);
                if (!contourBleedResult)
                {
                    const QString message = contourBleedResult.getErrorMessage();
                    setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, message);
                    persistManifest(manifestPath, manifest);
                    finishProgressIfActive(activeProgress(job));
                    result.manifest = manifest;
                    return createExportError(message, std::move(result.writtenFiles), manifestPath, manifest);
                }
            }
        }

        if (isCancelRequested(job))
        {
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportCancelled(std::move(result.writtenFiles), manifestPath, manifest);
        }

        if (job.hasBleedFixupSettings)
        {
            PDFBleedFixupReport bleedReport;
            const PDFOperationResult bleedResult = PDFBleedFixup::apply(&assembledDocument, job.bleedFixupSettings, &bleedReport);
            if (!bleedResult)
            {
                setOutputBleedFailure(manifest, int(index), bleedResult.getErrorMessage());
                setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, bleedResult.getErrorMessage());
                persistManifestForJob(manifestPath, manifest);
                finishProgressIfActive(activeProgress(job));
                result.manifest = manifest;
                return createExportError(bleedResult.getErrorMessage(), std::move(result.writtenFiles), manifestPath, manifest);
            }
            setOutputBleedReport(manifest, int(index), bleedReport);
        }

        if (isCancelRequested(job))
        {
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportCancelled(std::move(result.writtenFiles), manifestPath, manifest);
        }

        if (job.hasTransparencyFlattenSettings)
        {
            PDFTransparencyFlattenReport transparencyReport;
            PDFTransparencyFlattenSettings transparencySettings = job.transparencyFlattenSettings;
            transparencySettings.analyzeOnly = false;
            const PDFOperationResult transparencyResult = PDFTransparencyFlattener::apply(&assembledDocument,
                                                                                            transparencySettings,
                                                                                            &transparencyReport,
                                                                                            nullptr);
            QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
            QJsonObject output = outputs.at(int(index)).toObject();
            output.insert(QStringLiteral("transparencyFlatten"), transparencyReport.toJson());
            outputs.replace(int(index), output);
            manifest.insert(QStringLiteral("outputs"), outputs);
            if (!transparencyResult)
            {
                setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, transparencyResult.getErrorMessage());
                persistManifestForJob(manifestPath, manifest);
                finishProgressIfActive(activeProgress(job));
                result.manifest = manifest;
                return createExportError(transparencyResult.getErrorMessage(), std::move(result.writtenFiles), manifestPath, manifest);
            }
        }

        if (isCancelRequested(job))
        {
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportCancelled(std::move(result.writtenFiles), manifestPath, manifest);
        }

        if (job.optimizeImages)
        {
            PDFImageOptimizer imageOptimizer;
            PDFImageOptimizer::Settings optimizeSettings = job.imageOptimizationSettings;
            optimizeSettings.enabled = true;
            assembledDocument = imageOptimizer.optimize(&assembledDocument, optimizeSettings, {}, nullptr);
        }

        if (runPreflight && job.revalidatePreflightAfterFixups)
        {
            PDFDocumentSession session(&assembledDocument);
            PreflightEngine engine(&session);
            PreflightResult preflightResult = engine.run(preflightProfile);
            preflightResult.profileResolution = preflightResolution;
            const QJsonObject preflightReport = preflightResult.toJson(fileName);
            setOutputPreflightReport(manifest, int(index), QStringLiteral("revalidation"), preflightReport);
            writeFileAtomically(fileName + QStringLiteral(".preflight-final.json"),
                                QJsonDocument(preflightReport).toJson(QJsonDocument::Indented));

            if (!preflightResult.pass && !job.forcePreflight)
            {
                const QString message = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                    "Final preflight revalidation failed for '%1'.").arg(fileName);
                setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, message);
                persistManifestForJob(manifestPath, manifest);
                finishProgressIfActive(activeProgress(job));
                result.manifest = manifest;
                return createExportError(message, std::move(result.writtenFiles), manifestPath, manifest);
            }
        }

        if (isCancelRequested(job))
        {
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportCancelled(std::move(result.writtenFiles), manifestPath, manifest);
        }

        const bool isDocumentFileAlreadyExisting = QFile::exists(fileName);
        if (!job.overwriteFiles && isDocumentFileAlreadyExisting)
        {
            const QString message = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                  "Document with filename '%1' already exists.").arg(fileName);
            setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, message);
            persistManifestForJob(manifestPath, manifest);
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportError(message, std::move(result.writtenFiles), manifestPath, manifest);
        }

        // PDFDocumentWriter(safeWrite=true) uses QSaveFile: temp write then commit/rename
        // without deleting the previous final until the new bytes are durable.
        PDFDocumentWriter writer(nullptr);
        const PDFOperationResult writeResult = writer.write(fileName, &assembledDocument, true);
        if (!writeResult)
        {
            const QString message = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                                "Could not write document to '%1'.").arg(fileName);
            setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, message);
            persistManifestForJob(manifestPath, manifest);
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportError(message, std::move(result.writtenFiles), manifestPath, manifest);
        }

        setOutputStatus(manifest, int(index), OUTPUT_STATUS_WRITTEN);
        if (manifestPath.isEmpty() || !persistManifestForJob(manifestPath, manifest))
        {
            // Roll back only an output this run created. When the write replaced a
            // pre-existing file, the original is already gone, so removing the new
            // one would destroy user data and leave nothing in its place. In that
            // case keep the (valid) output and report the inconsistency instead.
            QString message;
            if (isDocumentFileAlreadyExisting)
            {
                message = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                      "Manifest update failed after overwriting '%1'. The new output was kept because the previous file had already been replaced; batch state may be stale, so verify before resuming.")
                              .arg(fileName);
            }
            else
            {
                QFile::remove(fileName);
                message = QCoreApplication::translate("pdf::PDFPageMasterExport",
                                                      "Manifest update failed after writing '%1'; output was removed to keep batch state consistent.")
                              .arg(fileName);
            }

            setOutputStatus(manifest, int(index), OUTPUT_STATUS_FAILED, message);
            persistManifestForJob(manifestPath, manifest);
            finishProgressIfActive(activeProgress(job));
            result.manifest = manifest;
            return createExportError(message, std::move(result.writtenFiles), manifestPath, manifest);
        }
        result.writtenFiles << fileName;

        if (PDFProgress* stepProgress = activeProgress(job))
        {
            stepProgress->step();
        }
    }

    if (trackProgress)
    {
        finishProgressIfActive(activeProgress(job));
    }

    result.manifest = manifest;
    return result;
}

} // namespace pdf
