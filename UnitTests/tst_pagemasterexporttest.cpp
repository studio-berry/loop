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
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfglobal.h"
#include "pdfjobscheduler.h"
#include "pdfprogress.h"

#include <QtTest>
#include <QtConcurrent/QtConcurrent>

#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <memory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

class PageMasterExportTest : public QObject
{
    Q_OBJECT

private slots:
    void pipelineOrder_geometryThenBleedThenWrite();
    void pipelineOrder_skipsDisabledStages();
    void multiOutput_writesAll();
    void failure_assembleError_writesNothing();
    void failure_geometryError_noPartialWrite();
    void failure_bleedError_noPartialWrite();
    void failure_overwriteDisabled_existingFile();
    void failure_writeError_reportsMessage();
    void failure_mismatchedOutputCount_reportsError();
    void multiOutput_midBatchFailure_keepsEarlierWrites();
    void multiOutput_manifestRecordsFailureAndPendingOutputs();
    void progress_singleCombinedPhase();
    void multiOutput_benchmarkRecordsTiming();
    void cancel_beforeFirstOutput_writesNothing();
    void cancel_midOutput_beforeWrite_writesNothing();
    void cancel_betweenOutputs_keepsCommitted();
    void cancel_closeDetach_invalidatesProgressAndBoundedWait();
    void atomicWrite_leavesNoPartialFiles();
    void manifest_persistedWithWrittenStatuses();
    void manifest_persistFailure_removesNewOutput();
    void manifest_persistFailure_keepsOverwrittenOutput();
    void manifest_corruptResumeFailsClosed();
    void manifest_concurrentBatchesHaveIndependentState();
    void processKill_afterAtomicOutputLeavesNoPartialFile();
    void resume_skipsAlreadyWrittenOutputs();
    void resume_mismatchedManifestRejectsResume();
    void resume_configDriftAfterInterruptRejectsResume();
    void resume_sourceAndImageIdentityDriftRejectsResume();
    void resume_matchingSourceIdentityResumes();
    void resume_preflightProfileIdentityDriftRejectsResume();
    void preflight_gate_blocksFailedOutput();
    void preflight_sidecarWriteFailure_failsClosed();
    void preflight_finalSidecarWriteFailure_keepsPriorOutput();
    void bleed_confirmationGate_blocksBeforeAssembly();
    void bleed_manifestReportsEligibility();
};

namespace
{

pdf::PDFDocument buildFilledPage(const QRectF& mediaBox = QRectF(0, 0, 200, 200))
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(mediaBox);
    builder.setPageTrimBox(page, mediaBox.adjusted(10, 10, -10, -10));

    pdf::PDFPageContentStreamBuilder pageContentStreamBuilder(&builder,
                                                              pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = pageContentStreamBuilder.begin(page))
    {
        painter->fillRect(mediaBox.adjusted(10, 10, -10, -10), Qt::black);
        pageContentStreamBuilder.end(painter);
    }

    return builder.build();
}

QSizeF pageSizeMM(const pdf::PDFDocument& document)
{
    const pdf::PDFPage* page = document.getCatalog()->getPage(0);
    const QRectF mediaBox = page->getMediaBox();
    return QSizeF(mediaBox.width() * pdf::PDF_POINT_TO_MM, mediaBox.height() * pdf::PDF_POINT_TO_MM);
}

pdf::PDFDocumentManipulator::AssembledPage documentPage(int documentIndex, const pdf::PDFDocument& document)
{
    return pdf::PDFDocumentManipulator::createDocumentPage(documentIndex, 0, pageSizeMM(document), pdf::PageRotation::None);
}

pdf::PDFDocument readDocument(const QString& fileName)
{
    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fileName);
    Q_ASSERT(reader.getReadingResult() == pdf::PDFDocumentReader::Result::OK);
    return document;
}

pdf::PDFArtifactIdentity testArtifactIdentity(const QByteArray& bytes,
                                              const QString& mediaType,
                                              const QString& logicalName)
{
    pdf::PDFArtifactIdentity identity;
    identity.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    identity.size = bytes.size();
    identity.mediaType = mediaType;
    identity.logicalName = logicalName;
    return identity;
}

/// Rewrites the manifest at \p manifestPath so every output is pending again, which makes a
/// resuming run re-write them instead of skipping them.
bool resetManifestOutputsToPending(const QString& manifestPath)
{
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }

    QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    for (int index = 0; index < outputs.size(); ++index)
    {
        QJsonObject entry = outputs.at(index).toObject();
        entry.insert(QStringLiteral("status"), QStringLiteral("pending"));
        entry.remove(QStringLiteral("error"));
        outputs.replace(index, entry);
    }
    manifest.insert(QStringLiteral("outputs"), outputs);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }

    file.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

bool anyOutputExists(const QStringList& paths)
{
    for (const QString& path : paths)
    {
        if (QFile::exists(path))
        {
            return true;
        }
    }
    return false;
}

bool waitForExportFinishedBounded(QFutureWatcherBase* watcher, int timeoutMs)
{
    if (!watcher || watcher->isFinished())
    {
        return true;
    }

    QEventLoop loop;
    QObject::connect(watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    return watcher->isFinished();
}

int runCrashHarness(const QStringList& arguments)
{
    if (arguments.size() != 4)
    {
        return 2;
    }

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(arguments.at(2));
    job.overwriteFiles = true;
    job.manifestPath = arguments.at(3);
    job.manifestPersist = [](const QString& path, const QJsonObject& manifest)
    {
        const QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
        if (!outputs.isEmpty() && outputs.first().toObject().value(QStringLiteral("status")).toString() == QStringLiteral("written"))
        {
            // Simulate process death after the atomic PDF commit and before its
            // manifest update. The parent verifies the final path remains valid.
            std::quick_exit(91);
        }

        // Real persistence, mirroring PDFPageMasterExport::run()'s internal
        // persistManifest() (not reachable from here - it's file-local to
        // pdfpagemasterexport.cpp). Without this, no manifest ever reaches disk:
        // the harness only intercepted the crash condition and otherwise just
        // returned true without writing anything, so the initial 'pending'
        // persist call (made before any output is written) silently no-opped.
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        return file.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact)) >= 0;
    };

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    return result.success ? 0 : 1;
}

/// Peak resident set from /proc (Linux); returns 0 when unavailable.
qint64 readVmHWMKilobytes()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return 0;
    }

    const QString contents = QString::fromUtf8(status.readAll());
    for (const QString& rawLine : contents.split(QLatin1Char('\n')))
    {
        const QString line = rawLine.trimmed();
        if (!line.startsWith(QStringLiteral("VmHWM:")))
        {
            continue;
        }

        const QStringList parts = line.mid(6).simplified().split(QLatin1Char(' '));
        if (!parts.isEmpty())
        {
            bool ok = false;
            const qint64 value = parts.front().toLongLong(&ok);
            if (ok)
            {
                return value;
            }
        }
    }
    return 0;
}

}   // namespace

void PageMasterExportTest::pipelineOrder_geometryThenBleedThenWrite()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage();
    const qreal sourceMediaWidth = source.getCatalog()->getPage(0)->getMediaBox().width();

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("geometry-bleed.pdf")));
    job.overwriteFiles = true;

    job.hasPageGeometrySettings = true;
    job.pageGeometrySettings.useTargetPageSize = true;
    job.pageGeometrySettings.targetPageSizeMM = QSizeF(100.0, 100.0);
    job.pageGeometrySettings.applyMediaBox = true;
    job.pageGeometrySettings.applyCropBox = true;
    job.pageGeometrySettings.applyBleedBox = true;
    job.pageGeometrySettings.applyTrimBox = true;

    job.hasBleedFixupSettings = true;
    job.bleedConfirmationGranted = true;
    job.bleedFixupSettings.force = true;
    job.bleedFixupSettings.skipIfAlreadyBleeding = false;
    job.bleedFixupSettings.dpi = 72;
    job.bleedFixupSettings.bleedMM = QMarginsF(3.0, 3.0, 3.0, 3.0);

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.writtenFiles.size(), 1);
    QVERIFY(QFile::exists(result.writtenFiles.front()));

    const pdf::PDFDocument written = readDocument(result.writtenFiles.front());
    const pdf::PDFPage* page = written.getCatalog()->getPage(0);
    QVERIFY(page);

    const qreal geometryWidthPt = 100.0 * pdf::PDF_MM_TO_POINT;
    const qreal bleedDepthPt = 3.0 * pdf::PDF_MM_TO_POINT;
    const qreal expectedMediaWidth = geometryWidthPt + (2.0 * bleedDepthPt);

    QVERIFY(sourceMediaWidth < geometryWidthPt);
    QVERIFY2(qAbs(page->getMediaBox().width() - expectedMediaWidth) < 1.0,
             qPrintable(QStringLiteral("media width %1 expected ~%2")
                            .arg(page->getMediaBox().width())
                            .arg(expectedMediaWidth)));
    QVERIFY(page->getTrimBox().width() < page->getMediaBox().width());
}

void PageMasterExportTest::pipelineOrder_skipsDisabledStages()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage();
    const qreal sourceMediaWidth = source.getCatalog()->getPage(0)->getMediaBox().width();

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("plain.pdf")));
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY2(result.success, qPrintable(result.errorMessage));

    const pdf::PDFDocument written = readDocument(result.writtenFiles.front());
    const pdf::PDFPage* page = written.getCatalog()->getPage(0);
    QVERIFY(page);
    QVERIFY2(qAbs(page->getMediaBox().width() - sourceMediaWidth) < 1.0,
             qPrintable(QStringLiteral("media width changed without geometry/bleed: %1 vs %2")
                            .arg(page->getMediaBox().width())
                            .arg(sourceMediaWidth)));
}

void PageMasterExportTest::multiOutput_writesAll()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage();
    const auto page = documentPage(0, source);

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ page });
    job.assembledDocuments.push_back({ page });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("out-a.pdf")));
    job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("out-b.pdf")));
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.writtenFiles.size(), 2);
    for (const QString& path : result.writtenFiles)
    {
        QVERIFY(QFile::exists(path));
    }
}

void PageMasterExportTest::failure_assembleError_writesNothing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("assemble-fail.pdf"));

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    // Point at a non-existent document index so assemble fails.
    job.assembledDocuments.push_back({ pdf::PDFDocumentManipulator::createDocumentPage(99, 0, QSizeF(70.0, 70.0), pdf::PageRotation::None) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(!result.errorMessage.isEmpty());
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::failure_geometryError_noPartialWrite()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("geometry-fail.pdf"));

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;
    job.hasPageGeometrySettings = true;
    job.pageGeometrySettings.applyMediaBox = false;
    job.pageGeometrySettings.applyCropBox = false;
    job.pageGeometrySettings.applyBleedBox = false;
    job.pageGeometrySettings.applyTrimBox = false;
    job.pageGeometrySettings.applyArtBox = false;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("No target page box"), Qt::CaseInsensitive));
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::failure_bleedError_noPartialWrite()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("bleed-fail.pdf"));

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;
    job.hasBleedFixupSettings = true;
    job.bleedConfirmationGranted = true;
    job.bleedFixupSettings.dpi = 0;
    job.bleedFixupSettings.force = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("DPI"), Qt::CaseInsensitive));
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::failure_overwriteDisabled_existingFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("exists.pdf"));
    {
        QFile existing(outputPath);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("placeholder");
    }
    const qint64 existingSize = QFileInfo(outputPath).size();

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = false;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("already exists"), Qt::CaseInsensitive));
    QVERIFY(result.writtenFiles.isEmpty());
    QCOMPARE(QFileInfo(outputPath).size(), existingSize);
}

void PageMasterExportTest::failure_writeError_reportsMessage()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString missingDir = tempDir.filePath(QStringLiteral("missing-subdir"));
    const QString outputPath = QDir(missingDir).filePath(QStringLiteral("out.pdf"));
    QVERIFY(!QDir(missingDir).exists());

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(!result.errorMessage.isEmpty());
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(!anyOutputExists({ outputPath }));
}

void PageMasterExportTest::failure_mismatchedOutputCount_reportsError()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("only-one.pdf"));

    pdf::PDFDocument source = buildFilledPage();
    const auto page = documentPage(0, source);

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ page });
    job.assembledDocuments.push_back({ page });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("2"), Qt::CaseInsensitive));
    QVERIFY(result.errorMessage.contains(QStringLiteral("1"), Qt::CaseInsensitive));
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::multiOutput_midBatchFailure_keepsEarlierWrites()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString firstPath = tempDir.filePath(QStringLiteral("ok.pdf"));
    const QString missingDir = tempDir.filePath(QStringLiteral("missing-subdir"));
    const QString secondPath = QDir(missingDir).filePath(QStringLiteral("fail.pdf"));
    QVERIFY(!QDir(missingDir).exists());

    pdf::PDFDocument source = buildFilledPage();
    const auto page = documentPage(0, source);

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ page });
    job.assembledDocuments.push_back({ page });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(firstPath);
    job.outputFileNames.push_back(secondPath);
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(!result.errorMessage.isEmpty());
    QCOMPARE(result.writtenFiles.size(), 1);
    QCOMPARE(result.writtenFiles.front(), firstPath);
    QVERIFY(QFile::exists(firstPath));
    QVERIFY(!QFile::exists(secondPath));
}

void PageMasterExportTest::multiOutput_manifestRecordsFailureAndPendingOutputs()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString manifestPath = tempDir.filePath(QStringLiteral("mid-batch.json"));
    const QString failedDirectory = tempDir.filePath(QStringLiteral("missing-output-directory"));
    QVERIFY(!QDir(failedDirectory).exists());

    pdf::PDFDocument source = buildFilledPage();
    const auto page = documentPage(0, source);
    pdf::PDFPageMasterExportJob job;
    for (int index = 0; index < 5; ++index)
    {
        job.assembledDocuments.push_back({ page });
        job.outputFileNames.push_back(index == 2
                                          ? QDir(failedDirectory).filePath(QStringLiteral("output-3.pdf"))
                                          : tempDir.filePath(QStringLiteral("output-%1.pdf").arg(index + 1)));
    }
    job.documents.emplace(0, std::move(source));
    job.overwriteFiles = true;
    job.manifestPath = manifestPath;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));

    QVERIFY(!result.success);
    QCOMPARE(result.writtenFiles.size(), 2);
    QVERIFY(QFile::exists(result.writtenFiles.at(0)));
    QVERIFY(QFile::exists(result.writtenFiles.at(1)));
    QVERIFY(readDocument(result.writtenFiles.at(0)).getCatalog()->getPageCount() == 1);
    QVERIFY(readDocument(result.writtenFiles.at(1)).getCatalog()->getPageCount() == 1);
    QVERIFY(!QFile::exists(QDir(failedDirectory).filePath(QStringLiteral("output-3.pdf"))));
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("output-4.pdf"))));
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("output-5.pdf"))));

    const QJsonArray outputs = result.manifest.value(QStringLiteral("outputs")).toArray();
    QCOMPARE(outputs.size(), 5);
    QCOMPARE(outputs.at(0).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("written"));
    QCOMPARE(outputs.at(1).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("written"));
    QCOMPARE(outputs.at(2).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    QCOMPARE(outputs.at(3).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QCOMPARE(outputs.at(4).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QVERIFY(!outputs.at(2).toObject().value(QStringLiteral("error")).toString().isEmpty());

    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    QVERIFY(parseError.error == QJsonParseError::NoError);
    QCOMPARE(manifestDocument.object().value(QStringLiteral("outputs")).toArray().size(), 5);
}

void PageMasterExportTest::progress_singleCombinedPhase()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFProgress progress(nullptr);
    QSignalSpy startedSpy(&progress, &pdf::PDFProgress::progressStarted);
    QSignalSpy stepSpy(&progress, &pdf::PDFProgress::progressStep);
    QSignalSpy finishedSpy(&progress, &pdf::PDFProgress::progressFinished);

    // Use image pages so optimize actually finds images. If progress were
    // wrongly forwarded to PDFImageOptimizer, it would start nested
    // "Optimizing images..." phases and startedSpy.count() would exceed 1.
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(Qt::red);
    const auto page = pdf::PDFDocumentManipulator::createImagePage(0, QSizeF(50.0, 50.0), pdf::PageRotation::None);

    pdf::PDFPageMasterExportJob job;
    job.images.emplace(0, std::move(image));
    job.assembledDocuments.push_back({ page });
    job.assembledDocuments.push_back({ page });
    job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("prog-a.pdf")));
    job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("prog-b.pdf")));
    job.overwriteFiles = true;
    job.optimizeImages = true;
    // Leave settings.enabled at default (false) — service must treat
    // optimizeImages as authoritative so optimize still runs.
    job.imageOptimizationSettings = pdf::PDFImageOptimizer::Settings::createDefault();
    QVERIFY(!job.imageOptimizationSettings.enabled);
    job.progress = &progress;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.writtenFiles.size(), 2);

    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(stepSpy.count(), 2);

    const auto startedArgs = startedSpy.takeFirst();
    QCOMPARE(startedArgs.size(), 1);
    const pdf::ProgressStartupInfo info = qvariant_cast<pdf::ProgressStartupInfo>(startedArgs.at(0));
    QVERIFY(info.text.contains(QStringLiteral("Exporting"), Qt::CaseInsensitive));
    QVERIFY(!info.text.contains(QStringLiteral("Assembling"), Qt::CaseInsensitive));
    QVERIFY(!info.text.contains(QStringLiteral("Writing"), Qt::CaseInsensitive));
    QVERIFY(!info.text.contains(QStringLiteral("Optimizing"), Qt::CaseInsensitive));

    const int lastPercentage = stepSpy.last().at(0).toInt();
    QCOMPARE(lastPercentage, 100);
}

void PageMasterExportTest::multiOutput_benchmarkRecordsTiming()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    constexpr int outputCount = 8;
    pdf::PDFDocument source = buildFilledPage();
    const auto page = documentPage(0, source);

    pdf::PDFPageMasterExportJob job;
    job.documents.emplace(0, std::move(source));
    job.overwriteFiles = true;
    for (int i = 0; i < outputCount; ++i)
    {
        job.assembledDocuments.push_back({ page });
        job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("bench-%1.pdf").arg(i)));
    }

    const qint64 vmBefore = readVmHWMKilobytes();
    QElapsedTimer timer;
    timer.start();
    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    const qint64 elapsedMs = timer.elapsed();
    const qint64 vmAfter = readVmHWMKilobytes();

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.writtenFiles.size(), outputCount);

    qInfo("MIC-307 multi-output benchmark: outputs=%d wall_ms=%lld VmHWM_before_kB=%lld VmHWM_after_kB=%lld",
          outputCount,
          static_cast<long long>(elapsedMs),
          static_cast<long long>(vmBefore),
          static_cast<long long>(vmAfter));

    QVERIFY(elapsedMs >= 0);
}

void PageMasterExportTest::cancel_beforeFirstOutput_writesNothing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("cancel-before.pdf"));

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;

    std::atomic_bool cancel{ true };
    job.cancelFlag = &cancel;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(result.cancelled);
    QVERIFY(!result.success);
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::cancel_midOutput_beforeWrite_writesNothing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("cancel-mid.pdf"));

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;

    std::atomic_bool cancel{ false };
    job.cancelFlag = &cancel;

    auto resultHolder = std::make_shared<pdf::PDFPageMasterExportResult>();
    std::atomic_bool started{ false };
    pdf::PDFJobScheduler scheduler(1);
    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Export;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.operationId = QStringLiteral("pagemaster-export");
    const QString jobId = scheduler.submit(spec, [job = std::move(job), resultHolder, &started](pdf::PDFJobContext& context) mutable
                                           {
        started.store(true, std::memory_order_release);
        while (!context.isCancellationRequested())
        {
            std::this_thread::yield();
        }
        if (job.cancelFlag)
        {
            job.cancelFlag->store(true, std::memory_order_release);
        }
        *resultHolder = pdf::PDFPageMasterExport::run(std::move(job));
        if (context.isCancellationRequested())
        {
            resultHolder->cancelled = true;
            resultHolder->success = false;
        } });

    QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 1000);
    QVERIFY(scheduler.cancel(jobId));
    QVERIFY(scheduler.waitForFinished(jobId, 5000));

    const pdf::PDFJobSnapshot snapshot = scheduler.snapshot(jobId);
    QVERIFY(snapshot.status != pdf::PDFJobStatus::Succeeded);
    QCOMPARE(snapshot.status, pdf::PDFJobStatus::Cancelled);
    const pdf::PDFPageMasterExportResult result = *resultHolder;
    QVERIFY(result.cancelled);
    QVERIFY(!result.success);
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::cancel_betweenOutputs_keepsCommitted()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputA = tempDir.filePath(QStringLiteral("between-a.pdf"));
    const QString outputB = tempDir.filePath(QStringLiteral("between-b.pdf"));

    pdf::PDFDocument source = buildFilledPage();
    const auto page = documentPage(0, source);

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ page });
    job.assembledDocuments.push_back({ page });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputA);
    job.outputFileNames.push_back(outputB);
    job.overwriteFiles = true;

    std::atomic_bool cancel{ false };
    job.cancelFlag = &cancel;

    pdf::PDFProgress progress(nullptr);
    job.progress = &progress;
    int completedOutputs = 0;
    QObject::connect(&progress, &pdf::PDFProgress::progressStep, &progress, [&](int)
                     {
        ++completedOutputs;
        if (completedOutputs >= 1)
        {
            cancel.store(true, std::memory_order_release);
        } }, Qt::DirectConnection);

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(result.cancelled);
    QVERIFY(!result.success);
    QCOMPARE(result.writtenFiles.size(), 1);
    QVERIFY(QFile::exists(outputA));
    QVERIFY(!QFile::exists(outputB));
    QCOMPARE(result.writtenFiles.front(), outputA);
}

void PageMasterExportTest::cancel_closeDetach_invalidatesProgressAndBoundedWait()
{
    pdf::PDFPageMasterExportCancelToken token;
    token.requestCancelAndInvalidateProgress();
    QVERIFY(token.cancel->load(std::memory_order_acquire));
    QVERIFY(!token.progressAlive->load(std::memory_order_acquire));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(tempDir.filePath(QStringLiteral("detach.pdf")));
    job.overwriteFiles = true;
    job.cancelFlag = token.cancel.get();
    job.progressAlive = token.progressAlive.get();

    // Progress object would be unsafe after UI detach; invalidate must skip callbacks.
    pdf::PDFProgress progress(nullptr);
    job.progress = &progress;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(result.cancelled);
    QVERIFY(result.writtenFiles.isEmpty());

    QFutureWatcher<void> watcher;
    watcher.setFuture(QtConcurrent::run([]()
                                        { QThread::msleep(50); }));
    QVERIFY(waitForExportFinishedBounded(&watcher, pdf::PDFPageMasterExport::DefaultCancelWaitMs));
    QVERIFY(watcher.isFinished());

    QFutureWatcher<void> slowWatcher;
    slowWatcher.setFuture(QtConcurrent::run([]()
                                            { QThread::msleep(500); }));
    QVERIFY(!waitForExportFinishedBounded(&slowWatcher, 20));
    QVERIFY(!slowWatcher.isFinished());
    QVERIFY(waitForExportFinishedBounded(&slowWatcher, pdf::PDFPageMasterExport::DefaultCancelWaitMs));
}

void PageMasterExportTest::atomicWrite_leavesNoPartialFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage();
    const QString outputPath = tempDir.filePath(QStringLiteral("atomic.pdf"));

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(result.success);
    QVERIFY(QFile::exists(outputPath));

    const QDir directory(tempDir.path());
    const QStringList partialFiles = directory.entryList({ QStringLiteral("*.partial") }, QDir::Files);
    QVERIFY(partialFiles.isEmpty());
}

void PageMasterExportTest::manifest_persistedWithWrittenStatuses()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage();
    const QString outputPath = tempDir.filePath(QStringLiteral("manifest.pdf"));

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(result.success);
    QVERIFY(QFile::exists(result.manifestPath));

    const QJsonArray outputs = result.manifest.value(QStringLiteral("outputs")).toArray();
    QCOMPARE(outputs.size(), 1);
    QCOMPARE(outputs.first().toObject().value(QStringLiteral("status")).toString(), QStringLiteral("written"));
}

void PageMasterExportTest::manifest_persistFailure_removesNewOutput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString manifestDirPath = tempDir.filePath(QStringLiteral("manifest-dir"));
    QVERIFY(QDir().mkpath(manifestDirPath));
    const QString manifestPath = QDir(manifestDirPath).filePath(QStringLiteral("batch.json"));
    const QString outputPath = tempDir.filePath(QStringLiteral("rollback.pdf"));

    // Produce a manifest this job can resume from. The failure is injected on the
    // post-write persistence call so the test is portable across filesystem permissions.
    pdf::PDFDocument initialSource = buildFilledPage();
    pdf::PDFPageMasterExportJob initialJob;
    initialJob.assembledDocuments.push_back({ documentPage(0, initialSource) });
    initialJob.documents.emplace(0, std::move(initialSource));
    initialJob.documentSourceIdentities.emplace(0,
                                                testArtifactIdentity(QByteArrayLiteral("stable-source"),
                                                                     QStringLiteral("application/pdf"),
                                                                     QStringLiteral("source.pdf")));
    initialJob.outputFileNames.push_back(outputPath);
    initialJob.overwriteFiles = true;
    initialJob.manifestPath = manifestPath;

    const pdf::PDFPageMasterExportResult initialResult = pdf::PDFPageMasterExport::run(std::move(initialJob));
    QVERIFY(initialResult.success);
    QVERIFY(QFile::exists(outputPath));

    // The output must not exist when the resuming run writes it, so that the run is the
    // creator of the file and rollback is allowed to remove it.
    QVERIFY(QFile::remove(outputPath));
    QVERIFY(resetManifestOutputsToPending(manifestPath));
    QVERIFY(!QFile::exists(outputPath));

    pdf::PDFDocument resumeSource = buildFilledPage();
    pdf::PDFPageMasterExportJob resumeJob;
    resumeJob.assembledDocuments.push_back({ pdf::PDFDocumentManipulator::createDocumentPage(0,
                                                                                               0,
                                                                                               QSizeF(200.0 * pdf::PDF_POINT_TO_MM,
                                                                                                      200.0 * pdf::PDF_POINT_TO_MM),
                                                                                               pdf::PageRotation::None) });
    resumeJob.documents.emplace(0, std::move(resumeSource));
    resumeJob.documentSourceIdentities.emplace(0,
                                               testArtifactIdentity(QByteArrayLiteral("stable-source"),
                                                                    QStringLiteral("application/pdf"),
                                                                    QStringLiteral("source.pdf")));
    resumeJob.outputFileNames.push_back(outputPath);
    resumeJob.overwriteFiles = true;
    resumeJob.resume = true;
    resumeJob.manifestPath = manifestPath;
    bool observedValidWrittenDocument = false;
    resumeJob.manifestPersist = [&observedValidWrittenDocument, &outputPath](const QString&, const QJsonObject&)
    {
        if (QFile::exists(outputPath))
        {
            const pdf::PDFDocument written = readDocument(outputPath);
            observedValidWrittenDocument = written.getCatalog()->getPageCount() == 1 && written.getCatalog()->getPage(0)->getMediaBox().width() == 200.0;
        }
        return false;
    };

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(resumeJob));

    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("Manifest update failed")));
    QVERIFY(result.errorMessage.contains(QStringLiteral("removed"), Qt::CaseInsensitive));
    QVERIFY(result.writtenFiles.isEmpty());
    QVERIFY(observedValidWrittenDocument);

    // The point of the rollback: no PDF is left behind that the manifest does not know about.
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::manifest_persistFailure_keepsOverwrittenOutput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString manifestDirPath = tempDir.filePath(QStringLiteral("manifest-dir"));
    QVERIFY(QDir().mkpath(manifestDirPath));
    const QString manifestPath = QDir(manifestDirPath).filePath(QStringLiteral("batch.json"));
    const QString outputPath = tempDir.filePath(QStringLiteral("overwritten.pdf"));

    pdf::PDFDocument initialSource = buildFilledPage();
    pdf::PDFPageMasterExportJob initialJob;
    initialJob.assembledDocuments.push_back({ documentPage(0, initialSource) });
    initialJob.documents.emplace(0, std::move(initialSource));
    initialJob.documentSourceIdentities.emplace(0,
                                                testArtifactIdentity(QByteArrayLiteral("stable-source"),
                                                                     QStringLiteral("application/pdf"),
                                                                     QStringLiteral("source.pdf")));
    initialJob.outputFileNames.push_back(outputPath);
    initialJob.overwriteFiles = true;
    initialJob.manifestPath = manifestPath;

    const pdf::PDFPageMasterExportResult initialResult = pdf::PDFPageMasterExport::run(std::move(initialJob));
    QVERIFY(initialResult.success);
    QVERIFY(QFile::exists(outputPath));

    // Unlike the previous test, the output is left in place, so the resuming run overwrites
    // a pre-existing file instead of creating one.
    QVERIFY(resetManifestOutputsToPending(manifestPath));

    QFile originalFile(outputPath);
    QVERIFY(originalFile.open(QIODevice::ReadOnly));
    const QByteArray originalDigest = QCryptographicHash::hash(originalFile.readAll(), QCryptographicHash::Sha256);
    originalFile.close();

    pdf::PDFDocument resumeSource = buildFilledPage(QRectF(0, 0, 320, 320));
    pdf::PDFPageMasterExportJob resumeJob;
    resumeJob.assembledDocuments.push_back({ pdf::PDFDocumentManipulator::createDocumentPage(0,
                                                                                               0,
                                                                                               QSizeF(200.0 * pdf::PDF_POINT_TO_MM,
                                                                                                      200.0 * pdf::PDF_POINT_TO_MM),
                                                                                               pdf::PageRotation::None) });
    resumeJob.documents.emplace(0, std::move(resumeSource));
    resumeJob.documentSourceIdentities.emplace(0,
                                               testArtifactIdentity(QByteArrayLiteral("stable-source"),
                                                                    QStringLiteral("application/pdf"),
                                                                    QStringLiteral("source.pdf")));
    resumeJob.outputFileNames.push_back(outputPath);
    resumeJob.overwriteFiles = true;
    resumeJob.resume = true;
    resumeJob.manifestPath = manifestPath;
    resumeJob.manifestPersist = [](const QString&, const QJsonObject&)
    {
        return false;
    };

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(resumeJob));

    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("Manifest update failed")));
    QVERIFY(result.errorMessage.contains(QStringLiteral("kept"), Qt::CaseInsensitive));
    QVERIFY(result.errorMessage.contains(QStringLiteral("stale"), Qt::CaseInsensitive));
    QVERIFY(result.errorMessage.contains(QStringLiteral("verify"), Qt::CaseInsensitive));

    // Removing this output would destroy the file it replaced, so it is kept and the
    // inconsistency is reported instead.
    QVERIFY(QFile::exists(outputPath));
    QFile overwrittenFile(outputPath);
    QVERIFY(overwrittenFile.open(QIODevice::ReadOnly));
    const QByteArray overwrittenBytes = overwrittenFile.readAll();
    overwrittenFile.close();
    QVERIFY(!overwrittenBytes.isEmpty());
    QVERIFY(QCryptographicHash::hash(overwrittenBytes, QCryptographicHash::Sha256) != originalDigest);
    const pdf::PDFDocument overwritten = readDocument(outputPath);
    QCOMPARE(overwritten.getCatalog()->getPage(0)->getMediaBox().width(), 320.0);
}

void PageMasterExportTest::manifest_corruptResumeFailsClosed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString manifestPath = tempDir.filePath(QStringLiteral("corrupt.json"));
    const QString outputPath = tempDir.filePath(QStringLiteral("protected.pdf"));
    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob initialJob;
    initialJob.assembledDocuments.push_back({ documentPage(0, source) });
    initialJob.documents.emplace(0, std::move(source));
    initialJob.outputFileNames.push_back(outputPath);
    initialJob.overwriteFiles = true;
    initialJob.manifestPath = manifestPath;

    const pdf::PDFPageMasterExportResult initialResult = pdf::PDFPageMasterExport::run(std::move(initialJob));
    QVERIFY(initialResult.success);
    const pdf::PDFDocument original = readDocument(outputPath);
    QCOMPARE(original.getCatalog()->getPage(0)->getMediaBox().width(), 200.0);

    QFile corruptManifest(manifestPath);
    QVERIFY(corruptManifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(corruptManifest.write("{\"outputs\":[") > 0);
    corruptManifest.close();

    pdf::PDFDocument resumeSource = buildFilledPage(QRectF(0, 0, 320, 320));
    pdf::PDFPageMasterExportJob resumeJob;
    resumeJob.assembledDocuments.push_back({ documentPage(0, resumeSource) });
    resumeJob.documents.emplace(0, std::move(resumeSource));
    resumeJob.outputFileNames.push_back(outputPath);
    resumeJob.overwriteFiles = true;
    resumeJob.resume = true;
    resumeJob.manifestPath = manifestPath;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(resumeJob));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("not valid JSON"), Qt::CaseInsensitive));
    const pdf::PDFDocument preserved = readDocument(outputPath);
    QCOMPARE(preserved.getCatalog()->getPage(0)->getMediaBox().width(), 200.0);
}

void PageMasterExportTest::manifest_concurrentBatchesHaveIndependentState()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputA = tempDir.filePath(QStringLiteral("batch-a.pdf"));
    const QString outputB = tempDir.filePath(QStringLiteral("batch-b.pdf"));
    const QString manifestA = tempDir.filePath(QStringLiteral("batch-a.json"));
    const QString manifestB = tempDir.filePath(QStringLiteral("batch-b.json"));
    auto runBatch = [](QString outputPath, QString manifestPath)
    {
        pdf::PDFDocument source = buildFilledPage();
        pdf::PDFPageMasterExportJob job;
        job.assembledDocuments.push_back({ documentPage(0, source) });
        job.documents.emplace(0, std::move(source));
        job.outputFileNames.push_back(outputPath);
        job.overwriteFiles = true;
        job.manifestPath = manifestPath;
        return pdf::PDFPageMasterExport::run(std::move(job));
    };

    auto futureA = QtConcurrent::run(runBatch, outputA, manifestA);
    auto futureB = QtConcurrent::run(runBatch, outputB, manifestB);
    futureA.waitForFinished();
    futureB.waitForFinished();
    const pdf::PDFPageMasterExportResult resultA = futureA.result();
    const pdf::PDFPageMasterExportResult resultB = futureB.result();

    QVERIFY2(resultA.success, qPrintable(resultA.errorMessage));
    QVERIFY2(resultB.success, qPrintable(resultB.errorMessage));
    QVERIFY(QFile::exists(outputA));
    QVERIFY(QFile::exists(outputB));
    QVERIFY(QFile::exists(manifestA));
    QVERIFY(QFile::exists(manifestB));
    QVERIFY(readDocument(outputA).getCatalog()->getPageCount() == 1);
    QVERIFY(readDocument(outputB).getCatalog()->getPageCount() == 1);

    const QString batchIdA = resultA.manifest.value(QStringLiteral("batch_id")).toString();
    const QString batchIdB = resultB.manifest.value(QStringLiteral("batch_id")).toString();
    QVERIFY(!batchIdA.isEmpty());
    QVERIFY(!batchIdB.isEmpty());
    QVERIFY(batchIdA != batchIdB);
    QCOMPARE(resultA.manifestPath, manifestA);
    QCOMPARE(resultB.manifestPath, manifestB);
}

void PageMasterExportTest::processKill_afterAtomicOutputLeavesNoPartialFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("crash-safe.pdf"));
    const QString manifestPath = tempDir.filePath(QStringLiteral("crash-safe.json"));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--pagemaster-crash-harness"), outputPath, manifestPath });
    QVERIFY2(child.waitForFinished(10000), qPrintable(child.errorString()));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 91);

    QVERIFY(QFile::exists(outputPath));
    QVERIFY(readDocument(outputPath).getCatalog()->getPageCount() == 1);
    QVERIFY(QFile::exists(manifestPath));
    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument manifest = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    QVERIFY(parseError.error == QJsonParseError::NoError);
    QCOMPARE(manifest.object().value(QStringLiteral("outputs")).toArray().first().toObject().value(QStringLiteral("status")).toString(), QStringLiteral("pending"));

    const QStringList temporaryFiles = QDir(tempDir.path()).entryList(QDir::Files | QDir::Hidden);
    for (const QString& fileName : temporaryFiles)
    {
        QVERIFY2(!fileName.startsWith(QStringLiteral("crash-safe.pdf.")), qPrintable(fileName));
    }
}

void PageMasterExportTest::resume_skipsAlreadyWrittenOutputs()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument sourceA = buildFilledPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument sourceB = buildFilledPage(QRectF(0, 0, 300, 300));
    const pdf::PDFArtifactIdentity sourceAIdentity = testArtifactIdentity(QByteArrayLiteral("source-a"),
                                                                            QStringLiteral("application/pdf"),
                                                                            QStringLiteral("source-a.pdf"));
    const pdf::PDFArtifactIdentity sourceBIdentity = testArtifactIdentity(QByteArrayLiteral("source-b"),
                                                                            QStringLiteral("application/pdf"),
                                                                            QStringLiteral("source-b.pdf"));
    const QString outputA = tempDir.filePath(QStringLiteral("resume-a.pdf"));
    const QString outputB = tempDir.filePath(QStringLiteral("resume-b.pdf"));

    pdf::PDFPageMasterExportJob initialJob;
    initialJob.assembledDocuments.push_back({ documentPage(0, sourceA) });
    initialJob.assembledDocuments.push_back({ documentPage(1, sourceB) });
    initialJob.documents.emplace(0, std::move(sourceA));
    initialJob.documents.emplace(1, std::move(sourceB));
    initialJob.documentSourceIdentities.emplace(0, sourceAIdentity);
    initialJob.documentSourceIdentities.emplace(1, sourceBIdentity);
    initialJob.outputFileNames.push_back(outputA);
    initialJob.outputFileNames.push_back(outputB);
    initialJob.overwriteFiles = true;

    const pdf::PDFPageMasterExportResult initialResult = pdf::PDFPageMasterExport::run(std::move(initialJob));
    QVERIFY(initialResult.success);
    QVERIFY(QFile::exists(outputA));
    QVERIFY(QFile::exists(outputB));

    QVERIFY(QFile::remove(outputB));
    QJsonObject manifest = initialResult.manifest;
    QJsonArray outputs = manifest.value(QStringLiteral("outputs")).toArray();
    QJsonObject secondOutput = outputs.at(1).toObject();
    secondOutput.insert(QStringLiteral("status"), QStringLiteral("pending"));
    secondOutput.remove(QStringLiteral("error"));
    outputs.replace(1, secondOutput);
    manifest.insert(QStringLiteral("outputs"), outputs);
    QFile manifestFile(initialResult.manifestPath);
    QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    pdf::PDFPageMasterExportJob resumeJob;
    resumeJob.assembledDocuments.push_back({ documentPage(0, readDocument(outputA)) });
    resumeJob.assembledDocuments.push_back({ documentPage(1, buildFilledPage(QRectF(0, 0, 300, 300))) });
    resumeJob.documents.emplace(0, readDocument(outputA));
    resumeJob.documents.emplace(1, buildFilledPage(QRectF(0, 0, 300, 300)));
    resumeJob.documentSourceIdentities.emplace(0, sourceAIdentity);
    resumeJob.documentSourceIdentities.emplace(1, sourceBIdentity);
    resumeJob.outputFileNames.push_back(outputA);
    resumeJob.outputFileNames.push_back(outputB);
    resumeJob.overwriteFiles = true;
    resumeJob.resume = true;
    resumeJob.manifestPath = initialResult.manifestPath;

    const pdf::PDFPageMasterExportResult resumeResult = pdf::PDFPageMasterExport::run(std::move(resumeJob));
    QVERIFY(resumeResult.success);
    QCOMPARE(resumeResult.writtenFiles.size(), 2);
    QVERIFY(QFile::exists(outputB));

    const qreal resumedWidth = readDocument(outputB).getCatalog()->getPage(0)->getMediaBox().width();
    QCOMPARE(resumedWidth, 300.0);
}

void PageMasterExportTest::resume_mismatchedManifestRejectsResume()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage(QRectF(0, 0, 220, 220));
    const QString outputPath = tempDir.filePath(QStringLiteral("fresh.pdf"));
    const QString staleOther = tempDir.filePath(QStringLiteral("stale-other.pdf"));

    QJsonObject staleManifest{
        { QStringLiteral("schema_version"), 2 },
        { QStringLiteral("batch_id"), QStringLiteral("stale-batch") },
        { QStringLiteral("outputs"), QJsonArray{
                                         QJsonObject{
                                             { QStringLiteral("path"), staleOther },
                                             { QStringLiteral("status"), QStringLiteral("written") } } } }
    };
    const QString manifestPath = tempDir.filePath(QStringLiteral(".loupe-batch.json"));
    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    manifestFile.write(QJsonDocument(staleManifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;
    job.resume = true;
    job.manifestPath = manifestPath;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("schema"), Qt::CaseInsensitive) ||
            result.errorMessage.contains(QStringLiteral("configuration"), Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(outputPath));
    QFile leftover(manifestPath);
    QVERIFY(leftover.open(QIODevice::ReadOnly));
    const QJsonDocument leftoverManifest = QJsonDocument::fromJson(leftover.readAll());
    QCOMPARE(leftoverManifest.object().value(QStringLiteral("batch_id")).toString(), QStringLiteral("stale-batch"));
}

void PageMasterExportTest::resume_configDriftAfterInterruptRejectsResume()
{
    auto fillTwoOutputJob = [](QTemporaryDir& tempDir, pdf::PDFPageMasterExportJob* job, QString* outputA, QString* outputB)
    {
        pdf::PDFDocument sourceA = buildFilledPage(QRectF(0, 0, 200, 200));
        pdf::PDFDocument sourceB = buildFilledPage(QRectF(0, 0, 240, 240));
        *outputA = tempDir.filePath(QStringLiteral("drift-a.pdf"));
        *outputB = tempDir.filePath(QStringLiteral("drift-b.pdf"));
        job->assembledDocuments.push_back({ documentPage(0, sourceA) });
        job->assembledDocuments.push_back({ documentPage(1, sourceB) });
        job->documents.emplace(0, std::move(sourceA));
        job->documents.emplace(1, std::move(sourceB));
        job->outputFileNames.push_back(*outputA);
        job->outputFileNames.push_back(*outputB);
        job->overwriteFiles = true;
    };

    const auto mutations = std::vector<std::pair<QString, std::function<void(pdf::PDFPageMasterExportJob&)>>>{
        { QStringLiteral("page-geometry"), [](pdf::PDFPageMasterExportJob& job)
          {
              job.hasPageGeometrySettings = true;
              job.pageGeometrySettings.applyBleedBox = true;
              job.pageGeometrySettings.marginsMM = QMarginsF(2.0, 2.0, 2.0, 2.0);
          } },
        { QStringLiteral("bleed-fixup"), [](pdf::PDFPageMasterExportJob& job)
          {
              job.hasBleedFixupSettings = true;
              job.bleedConfirmationGranted = true;
              job.bleedFixupSettings.dpi = 72;
          } },
        { QStringLiteral("flatten"), [](pdf::PDFPageMasterExportJob& job)
          {
              job.hasTransparencyFlattenSettings = true;
              job.transparencyFlattenSettings.rasterizationDpi = 72;
          } },
        { QStringLiteral("optimize"), [](pdf::PDFPageMasterExportJob& job)
          {
              job.optimizeImages = true;
              job.imageOptimizationSettings = pdf::PDFImageOptimizer::Settings::createDefault();
          } },
        { QStringLiteral("outline"), [](pdf::PDFPageMasterExportJob& job)
          {
              job.outlineMode = pdf::PDFDocumentManipulator::OutlineMode::Join;
          } },
        { QStringLiteral("action-list"), [](pdf::PDFPageMasterExportJob& job)
          {
              job.hasActionList = true;
              job.actionList.id = QStringLiteral("drift-list");
              job.actionList.name = QStringLiteral("drift");
          } }
    };

    for (const auto& mutation : mutations)
    {
        QTemporaryDir tempDir;
        QVERIFY2(tempDir.isValid(), qPrintable(mutation.first));

        QString outputA;
        QString outputB;
        pdf::PDFPageMasterExportJob initialJob;
        fillTwoOutputJob(tempDir, &initialJob, &outputA, &outputB);

        std::atomic_bool cancel{ false };
        initialJob.cancelFlag = &cancel;
        pdf::PDFProgress progress(nullptr);
        initialJob.progress = &progress;
        int completedOutputs = 0;
        QObject::connect(&progress, &pdf::PDFProgress::progressStep, &progress, [&](int)
                         {
            ++completedOutputs;
            if (completedOutputs >= 1)
            {
                cancel.store(true, std::memory_order_release);
            } }, Qt::DirectConnection);

        const pdf::PDFPageMasterExportResult interrupted = pdf::PDFPageMasterExport::run(std::move(initialJob));
        QVERIFY2(interrupted.cancelled, qPrintable(mutation.first));
        QVERIFY(QFile::exists(outputA));
        QVERIFY(!QFile::exists(outputB));
        QFile original(outputA);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = original.readAll();
        original.close();

        pdf::PDFPageMasterExportJob resumeJob;
        fillTwoOutputJob(tempDir, &resumeJob, &outputA, &outputB);
        mutation.second(resumeJob);
        resumeJob.resume = true;
        resumeJob.overwriteFiles = true;
        resumeJob.manifestPath = interrupted.manifestPath;

        const pdf::PDFPageMasterExportResult drifted = pdf::PDFPageMasterExport::run(std::move(resumeJob));
        QVERIFY2(!drifted.success, qPrintable(mutation.first + QLatin1Char(' ') + drifted.errorMessage));
        QVERIFY2(drifted.errorMessage.contains(QStringLiteral("configuration"), Qt::CaseInsensitive),
                 qPrintable(mutation.first + QLatin1Char(' ') + drifted.errorMessage));
        QVERIFY(!QFile::exists(outputB));
        QFile kept(outputA);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), originalBytes);
    }
}

void PageMasterExportTest::resume_sourceAndImageIdentityDriftRejectsResume()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourceOutput = tempDir.filePath(QStringLiteral("source-output.pdf"));
    const QString sourceManifest = tempDir.filePath(QStringLiteral("source-manifest.json"));
    pdf::PDFDocument source = buildFilledPage();
    pdf::PDFPageMasterExportJob initialSourceJob;
    initialSourceJob.documents.emplace(0, source);
    initialSourceJob.documentSourceIdentities.emplace(0,
                                                      testArtifactIdentity(QByteArrayLiteral("source-a"),
                                                                           QStringLiteral("application/pdf"),
                                                                           QStringLiteral("source.pdf")));
    initialSourceJob.assembledDocuments.push_back({ documentPage(0, source) });
    initialSourceJob.outputFileNames.push_back(sourceOutput);
    initialSourceJob.overwriteFiles = true;
    initialSourceJob.manifestPath = sourceManifest;
    const pdf::PDFPageMasterExportResult initialSource = pdf::PDFPageMasterExport::run(std::move(initialSourceJob));
    QVERIFY2(initialSource.success, qPrintable(initialSource.errorMessage));

    pdf::PDFPageMasterExportJob changedSourceJob;
    changedSourceJob.documents.emplace(0, source);
    changedSourceJob.documentSourceIdentities.emplace(0,
                                                       testArtifactIdentity(QByteArrayLiteral("source-b"),
                                                                            QStringLiteral("application/pdf"),
                                                                            QStringLiteral("source.pdf")));
    changedSourceJob.assembledDocuments.push_back({ documentPage(0, source) });
    changedSourceJob.outputFileNames.push_back(sourceOutput);
    changedSourceJob.overwriteFiles = true;
    changedSourceJob.resume = true;
    changedSourceJob.manifestPath = sourceManifest;
    const pdf::PDFPageMasterExportResult changedSource = pdf::PDFPageMasterExport::run(std::move(changedSourceJob));
    QVERIFY(!changedSource.success);
    QVERIFY(changedSource.errorMessage.contains(QStringLiteral("configuration"), Qt::CaseInsensitive));

    const QString imageOutput = tempDir.filePath(QStringLiteral("image-output.pdf"));
    const QString imageManifest = tempDir.filePath(QStringLiteral("image-manifest.json"));
    const auto imagePage = pdf::PDFDocumentManipulator::createImagePage(0, QSizeF(50.0, 50.0), pdf::PageRotation::None);
    QImage red(32, 32, QImage::Format_RGB32);
    red.fill(Qt::red);
    pdf::PDFPageMasterExportJob initialImageJob;
    initialImageJob.images.emplace(0, red);
    initialImageJob.imageSourceIdentities.emplace(0,
                                                  testArtifactIdentity(QByteArrayLiteral("image-a"),
                                                                       QStringLiteral("image/png"),
                                                                       QStringLiteral("image.png")));
    initialImageJob.assembledDocuments.push_back({ imagePage });
    initialImageJob.outputFileNames.push_back(imageOutput);
    initialImageJob.overwriteFiles = true;
    initialImageJob.manifestPath = imageManifest;
    const pdf::PDFPageMasterExportResult initialImage = pdf::PDFPageMasterExport::run(std::move(initialImageJob));
    QVERIFY2(initialImage.success, qPrintable(initialImage.errorMessage));

    QImage blue(32, 32, QImage::Format_RGB32);
    blue.fill(Qt::blue);
    pdf::PDFPageMasterExportJob changedImageJob;
    changedImageJob.images.emplace(0, blue);
    changedImageJob.imageSourceIdentities.emplace(0,
                                                  testArtifactIdentity(QByteArrayLiteral("image-b"),
                                                                       QStringLiteral("image/png"),
                                                                       QStringLiteral("image.png")));
    changedImageJob.assembledDocuments.push_back({ imagePage });
    changedImageJob.outputFileNames.push_back(imageOutput);
    changedImageJob.overwriteFiles = true;
    changedImageJob.resume = true;
    changedImageJob.manifestPath = imageManifest;
    const pdf::PDFPageMasterExportResult changedImage = pdf::PDFPageMasterExport::run(std::move(changedImageJob));
    QVERIFY(!changedImage.success);
    QVERIFY(changedImage.errorMessage.contains(QStringLiteral("configuration"), Qt::CaseInsensitive));
}

void PageMasterExportTest::resume_matchingSourceIdentityResumes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString outputPath = tempDir.filePath(QStringLiteral("matching.pdf"));
    const QString manifestPath = tempDir.filePath(QStringLiteral("matching-manifest.json"));
    const pdf::PDFDocument source = buildFilledPage();
    const pdf::PDFArtifactIdentity identity = testArtifactIdentity(QByteArrayLiteral("same-source"),
                                                                    QStringLiteral("application/pdf"),
                                                                    QStringLiteral("source.pdf"));

    pdf::PDFPageMasterExportJob initialJob;
    initialJob.documents.emplace(0, source);
    initialJob.documentSourceIdentities.emplace(0, identity);
    initialJob.assembledDocuments.push_back({ documentPage(0, source) });
    initialJob.outputFileNames.push_back(outputPath);
    initialJob.overwriteFiles = true;
    initialJob.manifestPath = manifestPath;
    const pdf::PDFPageMasterExportResult initial = pdf::PDFPageMasterExport::run(std::move(initialJob));
    QVERIFY2(initial.success, qPrintable(initial.errorMessage));

    pdf::PDFPageMasterExportJob resumeJob;
    resumeJob.documents.emplace(0, source);
    resumeJob.documentSourceIdentities.emplace(0, identity);
    resumeJob.assembledDocuments.push_back({ documentPage(0, source) });
    resumeJob.outputFileNames.push_back(outputPath);
    resumeJob.overwriteFiles = true;
    resumeJob.resume = true;
    resumeJob.manifestPath = manifestPath;
    const pdf::PDFPageMasterExportResult resumed = pdf::PDFPageMasterExport::run(std::move(resumeJob));
    QVERIFY2(resumed.success, qPrintable(resumed.errorMessage));
    QCOMPARE(resumed.writtenFiles, QStringList{ outputPath });
}

void PageMasterExportTest::resume_preflightProfileIdentityDriftRejectsResume()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString profilePath = tempDir.filePath(QStringLiteral("profile.json"));
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/profiles/loupe-default.json");
    QVERIFY(QFile::copy(fixturePath, profilePath));
    const QString outputPath = tempDir.filePath(QStringLiteral("profile-output.pdf"));
    const QString manifestPath = tempDir.filePath(QStringLiteral("profile-manifest.json"));
    const pdf::PDFDocument source = buildFilledPage();

    auto makeJob = [&]()
    {
        pdf::PDFPageMasterExportJob job;
        job.documents.emplace(0, source);
        job.assembledDocuments.push_back({ documentPage(0, source) });
        job.outputFileNames.push_back(outputPath);
        job.overwriteFiles = true;
        job.hasPreflightGate = true;
        job.preflightProfilePath = profilePath;
        job.forcePreflight = true;
        job.manifestPath = manifestPath;
        return job;
    };

    pdf::PDFPageMasterExportJob initialJob = makeJob();
    const pdf::PDFPageMasterExportResult initial = pdf::PDFPageMasterExport::run(std::move(initialJob));
    QVERIFY2(initial.success, qPrintable(initial.errorMessage));

    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::ReadOnly));
    QJsonObject profile = QJsonDocument::fromJson(profileFile.readAll()).object();
    profileFile.close();
    profile.insert(QStringLiteral("name"), QStringLiteral("mutated-profile"));
    QVERIFY(profileFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(profileFile.write(QJsonDocument(profile).toJson(QJsonDocument::Compact)) > 0);
    profileFile.close();

    pdf::PDFPageMasterExportJob resumeJob = makeJob();
    resumeJob.resume = true;
    const pdf::PDFPageMasterExportResult resumed = pdf::PDFPageMasterExport::run(std::move(resumeJob));
    QVERIFY(!resumed.success);
    QVERIFY(resumed.errorMessage.contains(QStringLiteral("configuration"), Qt::CaseInsensitive));
}

void PageMasterExportTest::preflight_gate_blocksFailedOutput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/color-rgb.pdf");
    const QString profilePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/profiles/loupe-default.json");
    QVERIFY(QFile::exists(fixturePath));
    QVERIFY(QFile::exists(profilePath));

    const QString outputPath = tempDir.filePath(QStringLiteral("preflight-blocked.pdf"));
    pdf::PDFDocument source = readDocument(fixturePath);

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;
    job.hasPreflightGate = true;
    job.preflightProfilePath = profilePath;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(!QFile::exists(outputPath));
    QVERIFY(QFile::exists(outputPath + QStringLiteral(".preflight.json")));
}

void PageMasterExportTest::preflight_sidecarWriteFailure_failsClosed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/color-rgb.pdf");
    const QString profilePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/profiles/loupe-default.json");
    QVERIFY(QFile::exists(fixturePath));
    QVERIFY(QFile::exists(profilePath));

    const QString outputPath = tempDir.filePath(QStringLiteral("sidecar-blocked.pdf"));
    const QString sidecarPath = outputPath + QStringLiteral(".preflight.json");
    pdf::PDFDocument source = readDocument(fixturePath);

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;
    job.hasPreflightGate = true;
    job.preflightProfilePath = profilePath;
    job.forcePreflight = true;

    pdf::PDFProgress progress(nullptr);
    QObject::connect(&progress, &pdf::PDFProgress::progressStarted, &progress, [&](pdf::ProgressStartupInfo)
                     { QVERIFY(QDir().mkpath(sidecarPath)); }, Qt::DirectConnection);
    job.progress = &progress;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("preflight report"), Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(outputPath));
    QCOMPARE(result.manifest.value(QStringLiteral("outputs")).toArray().first().toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("failed"));
}

void PageMasterExportTest::preflight_finalSidecarWriteFailure_keepsPriorOutput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/color-rgb.pdf");
    const QString profilePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/profiles/loupe-default.json");
    QVERIFY(QFile::exists(fixturePath));
    QVERIFY(QFile::exists(profilePath));

    const QString outputPath = tempDir.filePath(QStringLiteral("final-sidecar.pdf"));
    pdf::PDFDocument priorSource = buildFilledPage(QRectF(0, 0, 180, 180));
    pdf::PDFPageMasterExportJob priorJob;
    priorJob.assembledDocuments.push_back({ documentPage(0, priorSource) });
    priorJob.documents.emplace(0, std::move(priorSource));
    priorJob.outputFileNames.push_back(outputPath);
    priorJob.overwriteFiles = true;
    const pdf::PDFPageMasterExportResult priorResult = pdf::PDFPageMasterExport::run(std::move(priorJob));
    QVERIFY(priorResult.success);
    QFile priorFile(outputPath);
    QVERIFY(priorFile.open(QIODevice::ReadOnly));
    const QByteArray priorBytes = priorFile.readAll();
    priorFile.close();

    pdf::PDFDocument source = readDocument(fixturePath);
    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;
    job.hasPreflightGate = true;
    job.preflightProfilePath = profilePath;
    job.forcePreflight = true;
    job.revalidatePreflightAfterFixups = true;

    const QString finalSidecarPath = outputPath + QStringLiteral(".preflight-final.json");
    pdf::PDFProgress progress(nullptr);
    QObject::connect(&progress, &pdf::PDFProgress::progressStarted, &progress, [&](pdf::ProgressStartupInfo)
                     { QVERIFY(QDir().mkpath(finalSidecarPath)); }, Qt::DirectConnection);
    job.progress = &progress;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("final preflight"), Qt::CaseInsensitive));
    QFile kept(outputPath);
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), priorBytes);
}

void PageMasterExportTest::bleed_confirmationGate_blocksBeforeAssembly()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    pdf::PDFDocument source = buildFilledPage();
    const QString outputPath = tempDir.filePath(QStringLiteral("confirmation.pdf"));

    pdf::PDFPageMasterExportJob job;
    job.assembledDocuments.push_back({ documentPage(0, source) });
    job.documents.emplace(0, std::move(source));
    job.outputFileNames.push_back(outputPath);
    job.overwriteFiles = true;
    job.hasBleedFixupSettings = true;

    const pdf::PDFPageMasterExportResult result = pdf::PDFPageMasterExport::run(std::move(job));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("confirmation"), Qt::CaseInsensitive));
    QVERIFY(!QFile::exists(outputPath));
}

void PageMasterExportTest::bleed_manifestReportsEligibility()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString appliedPath = tempDir.filePath(QStringLiteral("applied.pdf"));
    pdf::PDFDocument appliedSource = buildFilledPage(QRectF(0.0, 0.0, 200.0, 200.0));
    pdf::PDFPageMasterExportJob appliedJob;
    appliedJob.assembledDocuments.push_back({ documentPage(0, appliedSource) });
    appliedJob.documents.emplace(0, std::move(appliedSource));
    appliedJob.outputFileNames.push_back(appliedPath);
    appliedJob.overwriteFiles = true;
    appliedJob.hasBleedFixupSettings = true;
    appliedJob.bleedConfirmationGranted = true;
    appliedJob.bleedFixupSettings.force = true;
    appliedJob.bleedFixupSettings.dpi = 72;

    const pdf::PDFPageMasterExportResult appliedResult = pdf::PDFPageMasterExport::run(std::move(appliedJob));
    QVERIFY2(appliedResult.success, qPrintable(appliedResult.errorMessage));
    const QJsonObject appliedReport = appliedResult.manifest.value(QStringLiteral("outputs")).toArray().first().toObject().value(QStringLiteral("bleed_report")).toObject();
    QVERIFY(appliedReport.value(QStringLiteral("eligible")).toBool());
    QVERIFY(appliedReport.value(QStringLiteral("applied")).toBool());
    QCOMPARE(appliedReport.value(QStringLiteral("status")).toString(), QStringLiteral("applied"));

    const QString sufficientPath = tempDir.filePath(QStringLiteral("sufficient.pdf"));
    pdf::PDFDocument sufficientSource = buildFilledPage(QRectF(0.0, 0.0, 200.0, 200.0));
    pdf::PDFPageMasterExportJob sufficientJob;
    sufficientJob.assembledDocuments.push_back({ documentPage(0, sufficientSource) });
    sufficientJob.documents.emplace(0, std::move(sufficientSource));
    sufficientJob.outputFileNames.push_back(sufficientPath);
    sufficientJob.overwriteFiles = true;
    sufficientJob.hasBleedFixupSettings = true;
    sufficientJob.bleedConfirmationGranted = true;
    sufficientJob.bleedFixupSettings.dpi = 72;

    const pdf::PDFPageMasterExportResult sufficientResult = pdf::PDFPageMasterExport::run(std::move(sufficientJob));
    QVERIFY2(sufficientResult.success, qPrintable(sufficientResult.errorMessage));
    const QJsonObject sufficientReport = sufficientResult.manifest.value(QStringLiteral("outputs")).toArray().first().toObject().value(QStringLiteral("bleed_report")).toObject();
    QVERIFY(!sufficientReport.value(QStringLiteral("eligible")).toBool());
    QVERIFY(!sufficientReport.value(QStringLiteral("applied")).toBool());
    QCOMPARE(sufficientReport.value(QStringLiteral("status")).toString(), QStringLiteral("not-needed"));
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.value(1) == QStringLiteral("--pagemaster-crash-harness"))
    {
        return runCrashHarness(arguments);
    }

    PageMasterExportTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_pagemasterexporttest.moc"
