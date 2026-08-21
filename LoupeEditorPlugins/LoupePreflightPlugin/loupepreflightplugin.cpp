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

#include "loupepreflightplugin.h"
#include "preflightreportdockwidget.h"
#include "preflightsidecarutils.h"
#include "repairpreviewdialog.h"
#include "../pdftoolenvelopeutils.h"

#include "pdfbleedfixup.h"
#include "pdfdocumentwriter.h"
#include "pdfdocumentreader.h"
#include "pdfrepairdiff.h"
#include "pdfrepairoperation.h"
#include "pdfoperationimpact.h"
#include "preflightengine.h"
#include "pdfsafefilewriter.h"
#include "pdfdrawspacecontroller.h"
#include "pdfdrawwidget.h"
#include "pdfjobscheduler.h"
#include "pdfuitheme.h"
#include "pdfwidgetutils.h"
#include "pdfwidgetutils.h"

#include <QAction>
#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMutexLocker>
#include <QPainter>
#include <QPen>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#ifndef LOUPE_PREFLIGHT_PROFILES_RELATIVE_PATH
#define LOUPE_PREFLIGHT_PROFILES_RELATIVE_PATH "../share/loupe/profiles"
#endif

namespace pdfplugin
{

namespace
{

constexpr qreal POINTS_PER_MM = 72.0 / 25.4;

pdf::PDFBleedFixupMode bleedFixupModeFromString(const QString& text)
{
    if (text == QStringLiteral("pixel-repeat"))
    {
        return pdf::PDFBleedFixupMode::PixelRepeat;
    }
    if (text == QStringLiteral("stretch"))
    {
        return pdf::PDFBleedFixupMode::Stretch;
    }
    return pdf::PDFBleedFixupMode::Mirror;
}

QString defaultBleedOutputPath(const QString& sourcePath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile())
    {
        return QString();
    }

    return sourceInfo.absolutePath() + QDir::separator() + sourceInfo.completeBaseName() + QStringLiteral("_bleed.") + sourceInfo.suffix();
}

QString defaultRgbToCmykOutputPath(const QString& sourcePath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile())
    {
        return QString();
    }

    return sourceInfo.absolutePath() + QDir::separator() + sourceInfo.completeBaseName() + QStringLiteral("_cmyk.") + sourceInfo.suffix();
}

QString defaultDownsampleOutputPath(const QString& sourcePath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile())
    {
        return QString();
    }

    return sourceInfo.absolutePath() + QDir::separator() + sourceInfo.completeBaseName() + QStringLiteral("_downsampled.") + sourceInfo.suffix();
}

QStringList enabledChecksForProfile(const QString& profilePath)
{
    QJsonObject profile;
    QString errorMessage;
    if (!pdf::PreflightEngine::loadProfile(profilePath, profile, errorMessage))
    {
        return {};
    }

    pdf::PreflightProfileData profileData;
    if (!pdf::PreflightEngine::parseProfile(profile, profileData, errorMessage))
    {
        return {};
    }

    QStringList enabledCheckIds;
    for (const pdf::PreflightCheckConfig& check : profileData.checks)
    {
        if (check.enabled)
        {
            enabledCheckIds.append(check.id);
        }
    }
    return enabledCheckIds;
}

QStringList targetedChecksForRepair(const pdf::PDFRepairOperation* operation,
                                    const QJsonObject& parameters,
                                    const QString& profilePath)
{
    if (!operation)
    {
        return {};
    }

    const pdf::PDFRevalidationPlan plan = pdf::planRevalidation(operation->impact(nullptr, parameters),
                                                                enabledChecksForProfile(profilePath));
    return plan.full ? QStringList{} : plan.checkIds;
}

bool writeReviewedRepairCandidate(pdf::PDFRepairTransaction& transaction,
                                  const QString& outputPath,
                                  QWidget* parent,
                                  const QString& title)
{
    QTemporaryDir previewDirectory;
    if (!previewDirectory.isValid())
    {
        QMessageBox::critical(parent, title, QObject::tr("Unable to create a private repair-preview directory."));
        return false;
    }

    const QString previewPath = QDir(previewDirectory.path()).filePath(QStringLiteral("candidate.pdf"));
    pdf::PDFRepairDiffOptions diffOptions;
    diffOptions.renderDirectory = previewDirectory.path();
    pdf::PDFRepairDiffReport diffReport;
    const pdf::PDFOperationResult candidateResult = transaction.compareCandidate(previewPath, diffOptions, &diffReport);
    if (!candidateResult)
    {
        QMessageBox::critical(parent, title, candidateResult.getErrorMessage());
        return false;
    }

    QFile previewFile(previewPath);
    if (!previewFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(parent, title, QObject::tr("The repair candidate could not be read after serialization."));
        return false;
    }
    const QByteArray previewData = previewFile.readAll();
    previewFile.close();
    const QByteArray previewHash = QCryptographicHash::hash(previewData, QCryptographicHash::Sha256);

    RepairPreviewDialog previewDialog(parent);
    previewDialog.setReport(diffReport, previewDirectory.path());
    if (previewDialog.exec() != QDialog::Accepted)
    {
        return false;
    }

    QFile candidateFile(previewPath);
    if (!candidateFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(parent, title, QObject::tr("The reviewed repair candidate is no longer available."));
        return false;
    }
    const QByteArray candidateData = candidateFile.readAll();
    candidateFile.close();
    if (QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256) != previewHash)
    {
        QMessageBox::critical(parent, title, QObject::tr("The repair candidate changed after preview. Review it again."));
        return false;
    }

    const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeData(
        outputPath, candidateData, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!writeResult)
    {
        QMessageBox::critical(parent, title, writeResult.getErrorMessage());
        return false;
    }

    QFile finalFile(outputPath);
    if (!finalFile.open(QIODevice::ReadOnly) ||
        QCryptographicHash::hash(finalFile.readAll(), QCryptographicHash::Sha256) != previewHash)
    {
        QMessageBox::critical(parent, title, QObject::tr("The final output does not match the approved repair candidate."));
        return false;
    }
    finalFile.close();

    pdf::PDFDocumentReader finalReader(nullptr, [](bool*)
                                       { return QString(); }, false, false);
    finalReader.readFromFile(outputPath);
    if (finalReader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        QMessageBox::critical(parent, title, QObject::tr("The final serialized output could not be reopened for verification."));
        return false;
    }
    return true;
}

}   // namespace

LoupePreflightPlugin::LoupePreflightPlugin() :
    pdf::PDFPlugin(nullptr)
{
}

LoupePreflightPlugin::~LoupePreflightPlugin()
{
    cancelPreflightRun(true);
}

void LoupePreflightPlugin::setWidget(pdf::PDFWidget* widget)
{
    Q_ASSERT(!m_widget);

    BaseClass::setWidget(widget);

    m_actionRunPreflight = new QAction(QIcon(":/pdfplugins/loupepreflight/preflight.svg"), tr("&Run Preflight"), this);
    m_actionRunPreflight->setObjectName("loupepreflight_Run");
    m_actionRunPreflight->setEnabled(false);
    m_actionRunPreflight->setToolTip(tr("Runs PdfTool preflight via QProcess (MIC-136)."));
    connect(m_actionRunPreflight, &QAction::triggered, this, &LoupePreflightPlugin::onRunPreflightTriggered);
    connect(&pdf::PDFJobScheduler::global(), &pdf::PDFJobScheduler::jobFinished, this, [this](const pdf::PDFJobSnapshot& snapshot)
            { onPreflightJobFinished(snapshot); });

    m_actionShowPanel = new QAction(QIcon(":/pdfplugins/loupepreflight/preflight.svg"), tr("Show &Report Panel"), this);
    m_actionShowPanel->setObjectName("loupepreflight_ShowPanel");
    m_actionShowPanel->setCheckable(true);
    connect(m_actionShowPanel, &QAction::toggled, this, &LoupePreflightPlugin::onShowPanelTriggered);

    m_actionLoadExample = new QAction(tr("Load &Example Report"), this);
    m_actionLoadExample->setObjectName("loupepreflight_LoadExample");
    connect(m_actionLoadExample, &QAction::triggered, this, &LoupePreflightPlugin::onLoadExampleReportTriggered);

    m_widget->getDrawWidgetProxy()->registerDrawInterface(this);

    updateActions();
}

void LoupePreflightPlugin::setDocument(const pdf::PDFModifiedDocument& document)
{
    BaseClass::setDocument(document);

    const bool documentChanged = documentModificationInvalidatesReport(document);
    if (documentChanged)
    {
        ++m_documentRevision;
    }

    if (document.hasReset())
    {
        cancelPreflightRun(true);
        invalidateReport();
    }
    else if (documentChanged)
    {
        if (isPreflightRunning())
        {
            cancelPreflightRun(true);
        }
        invalidateReportIfStale();
    }

    updateActions();
}

std::vector<QAction*> LoupePreflightPlugin::getActions() const
{
    return { m_actionRunPreflight, m_actionShowPanel, m_actionLoadExample };
}

QString LoupePreflightPlugin::getPluginMenuName() const
{
    return tr("Loupe &Preflight");
}

void LoupePreflightPlugin::ensureDockWidget()
{
    if (m_reportDockWidget)
    {
        return;
    }

    QMainWindow* mainWindow = m_dataExchangeInterface->getMainWindow();
    m_reportDockWidget = new PreflightReportDockWidget(mainWindow);
    m_reportDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_reportDockWidget, Qt::Vertical);
    m_reportDockWidget->setFloating(false);

    connect(m_reportDockWidget, &QDockWidget::visibilityChanged, this, [this](bool visible)
            {
        if (m_actionShowPanel && m_actionShowPanel->isChecked() != visible)
        {
            m_actionShowPanel->setChecked(visible);
        } });

    connect(m_reportDockWidget, &PreflightReportDockWidget::findingSelectionChanged,
            this, &LoupePreflightPlugin::onFindingSelectionChanged);

    connect(m_reportDockWidget, &PreflightReportDockWidget::applyFixupRequested,
            this, &LoupePreflightPlugin::onApplyFixupRequested);
}

void LoupePreflightPlugin::updateActions()
{
    const bool hasDocument = m_widget && m_document;

    if (m_actionShowPanel)
    {
        m_actionShowPanel->setEnabled(hasDocument);
    }

    if (m_actionLoadExample)
    {
        m_actionLoadExample->setEnabled(hasDocument);
    }

    if (m_actionRunPreflight)
    {
        m_actionRunPreflight->setEnabled(hasDocument && !isPreflightRunning());
    }
}

bool LoupePreflightPlugin::applyReportJson(const QJsonObject& report, QString* errorMessage, const QString& sourceLabel)
{
    const QJsonObject filteredReport = preflight::filterAdvertisedFixups(report);
    if (!preflight::validateNormalizedReport(filteredReport, errorMessage))
    {
        return false;
    }

    ensureDockWidget();
    m_reportDockWidget->setReport(filteredReport, sourceLabel);
    m_reportDocumentRevision = m_documentRevision;
    m_actionShowPanel->setChecked(true);
    m_reportDockWidget->show();
    return true;
}

bool LoupePreflightPlugin::resolvePreflightPaths(QString* pdfToolPath, QString* profilePath) const
{
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    *pdfToolPath = preflight::resolveBundlePath(applicationDirectory, preflight::getPdfToolFileName());
    *profilePath = preflight::resolveBundlePath(applicationDirectory, QStringLiteral(LOUPE_PREFLIGHT_PROFILES_RELATIVE_PATH "/loupe-default.json"));

    if (!QFileInfo(*pdfToolPath).isExecutable())
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"),
                              tr("Could not find the PdfTool preflight sidecar at %1.").arg(QDir::toNativeSeparators(*pdfToolPath)));
        return false;
    }

    if (!QFileInfo(*profilePath).isFile())
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"),
                              tr("Could not find the bundled Loupe Default profile at %1.").arg(QDir::toNativeSeparators(*profilePath)));
        return false;
    }

    return true;
}

void LoupePreflightPlugin::startPreflightOnFile(const QString& filePath,
                                                const QString& profilePath,
                                                quint64 revisionToMatch,
                                                bool ignoreRevisionMatch,
                                                const QString& reportSourceLabel,
                                                const QStringList& checkFilter)
{
    if (isPreflightRunning())
    {
        return;
    }

    m_preflightTemporaryDirectory = std::make_unique<QTemporaryDir>();
    if (!m_preflightTemporaryDirectory->isValid())
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"), tr("Could not create a temporary directory for preflight."));
        return;
    }

    const QString stagedPath = m_preflightTemporaryDirectory->filePath(QStringLiteral("preflight-input.pdf"));
    if (!QFile::copy(filePath, stagedPath))
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"),
                              tr("Could not stage the preflight input file at %1.").arg(QDir::toNativeSeparators(filePath)));
        m_preflightTemporaryDirectory.reset();
        return;
    }

    QString pdfToolPath;
    QString bundledProfilePath;
    if (!resolvePreflightPaths(&pdfToolPath, &bundledProfilePath))
    {
        m_preflightTemporaryDirectory.reset();
        return;
    }

    Q_UNUSED(bundledProfilePath);

    m_preflightStdoutBuffer.clear();
    m_preflightStderrBuffer.clear();
    m_preflightStdout.clear();
    m_preflightStderr.clear();
    m_preflightFailedToStart = false;
    m_preflightOutputOverflow = false;
    m_preflightExitCode = 0;
    m_preflightExitStatus = 0;
    m_preflightRunRevision = revisionToMatch;
    m_preflightIgnoreRevision = ignoreRevisionMatch;
    m_preflightReportSourceLabel = reportSourceLabel;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentRevision = QString::number(revisionToMatch);
    spec.operationId = QStringLiteral("preflight");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;
    pdf::PDFJobScheduler::global().setCurrentRevision(QStringLiteral("editor-preflight"), spec.documentRevision);
    spec.documentKey = QStringLiteral("editor-preflight");

    const QString workingDirectory = QCoreApplication::applicationDirPath();
    updateActions();
    m_preflightJobId = pdf::PDFJobScheduler::global().submit(spec, [this, pdfToolPath, stagedPath, profilePath, workingDirectory, checkFilter](pdf::PDFJobContext& context)
                                                             {
        QProcess process;
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.setWorkingDirectory(workingDirectory);
        QStringList arguments = { QStringLiteral("preflight"),
                                  stagedPath,
                                  QStringLiteral("--profile"),
                                  profilePath,
                                  QStringLiteral("--console-format"),
                                  QStringLiteral("json") };
        if (!checkFilter.isEmpty())
        {
            arguments << QStringLiteral("--checks") << checkFilter.join(QLatin1Char(','));
        }
        process.start(pdfToolPath, arguments);
        if (!process.waitForStarted(5000))
        {
            QMutexLocker locker(&m_preflightResultMutex);
            m_preflightFailedToStart = true;
            m_preflightStderr = process.errorString().toLocal8Bit();
            return;
        }

        preflight::PreflightSidecarStreamBuffer stdoutBuffer(preflight::PREFLIGHT_SIDECAR_STDOUT_MAX_BYTES);
        preflight::PreflightSidecarStreamBuffer stderrBuffer(preflight::PREFLIGHT_SIDECAR_STDERR_MAX_BYTES);
        bool outputOverflow = false;
        const auto drainChannels = [&]()
        {
            const QByteArray stdoutChunk = process.readAllStandardOutput();
            if (!stdoutChunk.isEmpty()
                && stdoutBuffer.append(stdoutChunk) == preflight::PreflightSidecarStreamBuffer::AppendResult::Overflow)
            {
                outputOverflow = true;
            }
            const QByteArray stderrChunk = process.readAllStandardError();
            if (!stderrChunk.isEmpty()
                && stderrBuffer.append(stderrChunk) == preflight::PreflightSidecarStreamBuffer::AppendResult::Overflow)
            {
                outputOverflow = true;
            }
        };

        while (!process.waitForFinished(100))
        {
            if (context.isCancellationRequested())
            {
                process.kill();
                process.waitForFinished(3000);
                return;
            }
            drainChannels();
            if (outputOverflow)
            {
                process.kill();
                process.waitForFinished(3000);
                break;
            }
        }
        drainChannels();

        QMutexLocker locker(&m_preflightResultMutex);
        m_preflightOutputOverflow = outputOverflow;
        if (outputOverflow)
        {
            m_preflightStdout.clear();
            m_preflightStderr = tr("Preflight output exceeded the maximum allowed size.").toLocal8Bit();
            m_preflightExitCode = 1;
            m_preflightExitStatus = static_cast<int>(QProcess::NormalExit);
            return;
        }
        m_preflightExitCode = process.exitCode();
        m_preflightExitStatus = static_cast<int>(process.exitStatus());
        m_preflightStdout = stdoutBuffer.takeData();
        m_preflightStderr = stderrBuffer.takeData(); });
}

void LoupePreflightPlugin::onRunPreflightTriggered()
{
    if (!m_document || isPreflightRunning())
    {
        return;
    }

    QString pdfToolPath;
    QString profilePath;
    if (!resolvePreflightPaths(&pdfToolPath, &profilePath))
    {
        return;
    }

    auto temporaryDirectory = std::make_unique<QTemporaryDir>();
    if (!temporaryDirectory->isValid())
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"), tr("Could not create a temporary directory for the preflight snapshot."));
        return;
    }

    const QString snapshotPath = temporaryDirectory->filePath(QStringLiteral("preflight-snapshot.pdf"));
    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult writeResult = writer.write(snapshotPath, m_document, false);
    if (!writeResult)
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"),
                              tr("Could not create a preflight snapshot: %1").arg(writeResult.getErrorMessage()));
        return;
    }

    const QString snapshot = snapshotPath;
    startPreflightOnFile(snapshot, profilePath, m_documentRevision, false, QString());
}

void LoupePreflightPlugin::onPreflightJobFinished(const pdf::PDFJobSnapshot& snapshot)
{
    if (snapshot.jobId != m_preflightJobId)
    {
        return;
    }

    int exitCode = 0;
    int exitStatus = 0;
    bool failedToStart = false;
    bool outputOverflow = false;
    QByteArray standardOutput;
    QByteArray standardErrorBytes;
    {
        QMutexLocker locker(&m_preflightResultMutex);
        exitCode = m_preflightExitCode;
        exitStatus = m_preflightExitStatus;
        failedToStart = m_preflightFailedToStart;
        outputOverflow = m_preflightOutputOverflow;
        standardOutput = m_preflightStdout;
        standardErrorBytes = m_preflightStderr;
    }

    const quint64 runRevision = m_preflightRunRevision;
    const bool ignoreRevision = m_preflightIgnoreRevision;
    const QString reportSourceLabel = m_preflightReportSourceLabel;
    finishPreflightRun();

    if (snapshot.status == pdf::PDFJobStatus::Cancelled || snapshot.status == pdf::PDFJobStatus::Stale)
    {
        return;
    }

    if (!ignoreRevision && runRevision != m_documentRevision)
    {
        return;
    }

    if (failedToStart || snapshot.status != pdf::PDFJobStatus::Succeeded)
    {
        const QString error = QString::fromLocal8Bit(standardErrorBytes).trimmed();
        QMessageBox::critical(m_widget, tr("Loupe Preflight"),
                              tr("Could not start PdfTool: %1").arg(error.isEmpty() ? snapshot.errorMessage : error));
        return;
    }

    if (outputOverflow)
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"), tr("Preflight output exceeded the maximum allowed size."));
        return;
    }

    const QString standardError = QString::fromLocal8Bit(standardErrorBytes).trimmed();

    if (exitStatus != static_cast<int>(QProcess::NormalExit) || !preflight::isExpectedPreflightExitCode(exitCode))
    {
        const QString detail = pdfplugin::pdftool::failureDetailFromStdout(
            standardOutput,
            standardError,
            exitCode,
            tr("PdfTool exited with code %1.").arg(exitCode));

        QMessageBox::critical(m_widget, tr("Loupe Preflight"), tr("Preflight did not complete successfully: %1").arg(detail));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument reportDocument = QJsonDocument::fromJson(standardOutput, &parseError);
    if (parseError.error != QJsonParseError::NoError || !reportDocument.isObject())
    {
        QString detail = parseError.error != QJsonParseError::NoError ? parseError.errorString() : tr("The output is not a JSON object.");
        QMessageBox::critical(m_widget, tr("Loupe Preflight"), tr("PdfTool returned an invalid preflight report: %1").arg(detail));
        return;
    }

    const QJsonObject envelope = reportDocument.object();
    if (!pdfplugin::pdftool::isResultEnvelope(envelope, QStringLiteral("preflight")) ||
        envelope.value(QStringLiteral("exit_code")).toInt(-1) != exitCode)
    {
        QMessageBox::critical(m_widget,
                              tr("Loupe Preflight"),
                              tr("PdfTool returned an invalid result envelope."));
        return;
    }

    const QJsonObject report = pdfplugin::pdftool::reportFromEnvelope(envelope);
    if (report.isEmpty())
    {
        QMessageBox::critical(m_widget,
                              tr("Loupe Preflight"),
                              tr("PdfTool returned a result without a preflight report."));
        return;
    }

    QString validationError;
    if (!applyReportJson(report, &validationError, reportSourceLabel))
    {
        QMessageBox::critical(m_widget, tr("Loupe Preflight"), tr("PdfTool returned an invalid preflight report: %1").arg(validationError));
    }
}

void LoupePreflightPlugin::finishPreflightRun()
{
    cancelPreflightRun(true);
    updateActions();
}

void LoupePreflightPlugin::cancelPreflightRun(bool silent)
{
    if (!m_preflightJobId.isEmpty())
    {
        pdf::PDFJobScheduler::global().cancel(m_preflightJobId);
        pdf::PDFJobScheduler::global().waitForFinished(m_preflightJobId, 5000);
        m_preflightJobId.clear();
    }

    m_preflightTemporaryDirectory.reset();
    m_preflightStdoutBuffer.clear();
    m_preflightStderrBuffer.clear();
    m_preflightStdout.clear();
    m_preflightStderr.clear();
    m_preflightFailedToStart = false;
    m_preflightOutputOverflow = false;
    m_preflightRunRevision = 0;
    m_preflightIgnoreRevision = false;
    m_preflightReportSourceLabel.clear();

    if (!silent)
    {
        updateActions();
    }
}

void LoupePreflightPlugin::abortPreflightRun(const QString& message)
{
    finishPreflightRun();
    QMessageBox::critical(m_widget, tr("Loupe Preflight"), message);
}

void LoupePreflightPlugin::invalidateReport()
{
    m_selectedFindingIndex = -1;

    if (m_reportDockWidget)
    {
        m_reportDockWidget->clearReport();
    }

    m_reportDocumentRevision = 0;
    updateOverlayGraphics();
}

void LoupePreflightPlugin::invalidateReportIfStale()
{
    if (m_reportDocumentRevision != 0 && m_reportDocumentRevision != m_documentRevision)
    {
        invalidateReport();
    }
}

bool LoupePreflightPlugin::documentModificationInvalidatesReport(const pdf::PDFModifiedDocument& document) const
{
    return document.hasReset() || document.hasPageContentsChanged() || document.hasFlag(pdf::PDFModifiedDocument::Annotation) || document.hasFlag(pdf::PDFModifiedDocument::FormField);
}

void LoupePreflightPlugin::onShowPanelTriggered(bool checked)
{
    if (!m_document)
    {
        return;
    }

    ensureDockWidget();

    if (checked)
    {
        m_reportDockWidget->show();
    }
    else
    {
        m_reportDockWidget->hide();
    }
}

void LoupePreflightPlugin::onLoadExampleReportTriggered()
{
    QFile file(QStringLiteral(":/pdfplugins/loupepreflight/report.example.json"));
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(m_widget, tr("Loupe Preflight"),
                             tr("Could not open the example report at %1.").arg(file.fileName()));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        QMessageBox::warning(m_widget, tr("Loupe Preflight"),
                             tr("Example report is not valid JSON: %1").arg(parseError.errorString()));
        return;
    }

    QString validationError;
    if (!applyReportJson(document.object(), &validationError))
    {
        QMessageBox::warning(m_widget, tr("Loupe Preflight"),
                             tr("Example report does not match the Loupe report contract: %1").arg(validationError));
    }
}

void LoupePreflightPlugin::drawOverlay(QPainter* painter, const pdf::PDFOverlayContext& context) const
{
    if (!context.renderable || !m_reportDockWidget || !m_reportDockWidget->hasReport())
    {
        return;
    }

    const QVector<PreflightFindingEntry>& findings = m_reportDockWidget->findings();
    if (findings.isEmpty())
    {
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal lineWidth = pdf::PDFWidgetUtils::scaleDPI_x(painter->device(), 1.0);
    const qreal selectedLineWidth = pdf::PDFWidgetUtils::scaleDPI_x(painter->device(), 2.5);

    for (int findingIndex = 0; findingIndex < findings.size(); ++findingIndex)
    {
        const PreflightFindingEntry& finding = findings.at(findingIndex);
        if (!finding.hasVisualOverlay || finding.page <= 0 || finding.page - 1 != context.pageIndex)
        {
            continue;
        }

        const QRectF deviceRect = context.pagePointToDevicePointMatrix.mapRect(finding.bbox).normalized();
        if (deviceRect.isEmpty())
        {
            continue;
        }

        const bool isSelected = findingIndex == m_selectedFindingIndex;
        QColor borderColor;
        QColor fillColor;

        if (finding.severity == QStringLiteral("error"))
        {
            borderColor = pdf::PDFUITheme::severityErrorColor();
            fillColor = QColor(borderColor.red(), borderColor.green(), borderColor.blue(), isSelected ? 90 : 55);
        }
        else if (finding.severity == QStringLiteral("warning"))
        {
            borderColor = pdf::PDFUITheme::severityWarningColor();
            fillColor = QColor(borderColor.red(), borderColor.green(), borderColor.blue(), isSelected ? 85 : 50);
        }
        else
        {
            borderColor = pdf::PDFUITheme::severityInfoColor();
            fillColor = QColor(borderColor.red(), borderColor.green(), borderColor.blue(), isSelected ? 80 : 45);
        }

        painter->setPen(QPen(borderColor, isSelected ? selectedLineWidth : lineWidth));
        painter->setBrush(fillColor);
        painter->drawRect(deviceRect);

        if (finding.severity == QStringLiteral("error"))
        {
            QPen hatchPen(borderColor, lineWidth * 0.75, Qt::SolidLine);
            painter->setPen(hatchPen);
            painter->setBrush(Qt::NoBrush);

            const qreal spacing = pdf::PDFWidgetUtils::scaleDPI_x(painter->device(), 6.0);
            for (qreal offset = deviceRect.left() - deviceRect.height(); offset < deviceRect.right(); offset += spacing)
            {
                painter->drawLine(QPointF(offset, deviceRect.bottom()),
                                  QPointF(offset + deviceRect.height(), deviceRect.top()));
            }
        }
    }
}

void LoupePreflightPlugin::updateOverlayGraphics()
{
    if (m_widget)
    {
        m_widget->getDrawWidget()->getWidget()->update();
    }
}

void LoupePreflightPlugin::onFindingSelectionChanged(int row)
{
    m_selectedFindingIndex = row;

    if (!m_reportDockWidget || !m_widget || row < 0)
    {
        updateOverlayGraphics();
        return;
    }

    const QVector<PreflightFindingEntry>& findings = m_reportDockWidget->findings();
    if (row >= findings.size())
    {
        updateOverlayGraphics();
        return;
    }

    const PreflightFindingEntry& finding = findings.at(row);
    if ((finding.scope == QStringLiteral("page") || finding.scope == QStringLiteral("object")) && finding.page > 0)
    {
        m_widget->getDrawWidgetProxy()->goToPage(finding.page - 1);
    }

    updateOverlayGraphics();
}

void LoupePreflightPlugin::onApplyFixupRequested(const QString& id)
{
    if (id == QStringLiteral("add-bleed"))
    {
        onApplyBleedFixupRequested();
    }
    else if (id == QStringLiteral("rgb-to-cmyk"))
    {
        onApplyRgbToCmykFixupRequested();
    }
    else if (id == QStringLiteral("downsample-images"))
    {
        onApplyDownsampleImagesRequested();
    }
}

void LoupePreflightPlugin::onApplyBleedFixupRequested()
{
    if (!m_document || !m_reportDockWidget)
    {
        return;
    }

    const PreflightFixupEntry* addBleedFixup = m_reportDockWidget->addBleedFixup();
    if (!addBleedFixup)
    {
        if (m_reportDockWidget->hasRgbToCmykFixup())
        {
            onApplyRgbToCmykFixupRequested();
        }
        return;
    }

    const QString originalPath = m_dataExchangeInterface->getOriginalFileName();
    const double defaultAmountPt = addBleedFixup->amountPt > 0.0 ? addBleedFixup->amountPt : 9.0;
    const qreal defaultBleedMm = defaultAmountPt / POINTS_PER_MM;
    const QString defaultMode = addBleedFixup->params.value(QStringLiteral("mode")).toString(QStringLiteral("mirror"));

    QDialog dialog(m_widget);
    dialog.setWindowTitle(tr("Apply Bleed Fix"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QFormLayout* form = new QFormLayout();

    QComboBox* modeCombo = new QComboBox(&dialog);
    modeCombo->addItem(tr("Mirror"), int(pdf::PDFBleedFixupMode::Mirror));
    modeCombo->addItem(tr("Pixel repeat"), int(pdf::PDFBleedFixupMode::PixelRepeat));
    modeCombo->addItem(tr("Stretch"), int(pdf::PDFBleedFixupMode::Stretch));
    const int defaultModeIndex = modeCombo->findData(int(bleedFixupModeFromString(defaultMode)));
    if (defaultModeIndex >= 0)
    {
        modeCombo->setCurrentIndex(defaultModeIndex);
    }
    form->addRow(tr("Mode"), modeCombo);

    QDoubleSpinBox* bleedSpin = new QDoubleSpinBox(&dialog);
    bleedSpin->setRange(0.1, 50.0);
    bleedSpin->setDecimals(2);
    bleedSpin->setSuffix(tr(" mm"));
    bleedSpin->setValue(defaultBleedMm);
    form->addRow(tr("Bleed amount"), bleedSpin);

    QLineEdit* outputPathEdit = new QLineEdit(defaultBleedOutputPath(originalPath), &dialog);
    QPushButton* browseButton = new QPushButton(tr("Browse..."), &dialog);
    QHBoxLayout* outputLayout = new QHBoxLayout();
    outputLayout->addWidget(outputPathEdit, 1);
    outputLayout->addWidget(browseButton);
    form->addRow(tr("Output file"), outputLayout);

    QCheckBox* rerunCheckBox = new QCheckBox(tr("Re-run preflight after fixing"), &dialog);
    rerunCheckBox->setChecked(true);
    layout->addLayout(form);
    layout->addWidget(rerunCheckBox);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, &dialog, [&dialog, outputPathEdit, originalPath]()
            {
        const QString selectedPath = QFileDialog::getSaveFileName(&dialog,
                                                                  tr("Save bleed-fixed PDF"),
                                                                  outputPathEdit->text().isEmpty() ? defaultBleedOutputPath(originalPath) : outputPathEdit->text(),
                                                                  tr("PDF files (*.pdf)"));
        if (!selectedPath.isEmpty())
        {
            outputPathEdit->setText(selectedPath);
        } });

    pdf::PDFWidgetUtils::style(&dialog);

    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const QString outputPath = outputPathEdit->text().trimmed();
    if (outputPath.isEmpty())
    {
        QMessageBox::warning(m_widget, tr("Apply Bleed Fix"), tr("Choose an output file path."));
        return;
    }

    if (QFile::exists(outputPath))
    {
        if (QMessageBox::warning(m_widget, tr("Apply Bleed Fix"),
                                 tr("'%1' already exists. Overwrite it?").arg(QDir::toNativeSeparators(outputPath)),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        {
            return;
        }
    }

    const qreal bleedMm = bleedSpin->value();
    const QString mode = modeCombo->currentData().toInt() == int(pdf::PDFBleedFixupMode::PixelRepeat)
                             ? QStringLiteral("pixel-repeat")
                         : modeCombo->currentData().toInt() == int(pdf::PDFBleedFixupMode::Stretch)
                             ? QStringLiteral("stretch")
                             : QStringLiteral("mirror");
    const QJsonObject repairParameters = QJsonObject{
        { QStringLiteral("mode"), mode },
        { QStringLiteral("bleed_mm"), bleedMm },
        { QStringLiteral("force"), true }
    };
    const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed"));
    if (!operation)
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), tr("The add-bleed repair operation is unavailable."));
        return;
    }

    pdf::PDFRepairTransaction transaction(*m_document);
    const pdf::PDFOperationResult addResult = transaction.add(operation, repairParameters);
    if (!addResult || !transaction.analyze() || !transaction.apply())
    {
        const QString message = !addResult ? addResult.getErrorMessage()
                                           : tr("The bleed repair could not be planned or applied.");
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), message);
        return;
    }

    // Serialize and reopen the candidate before showing it to the operator. The
    // dialog must review the bytes that will be committed, not only the copied
    // in-memory document.
    QTemporaryDir previewDirectory;
    if (!previewDirectory.isValid())
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), tr("Unable to create a private repair-preview directory."));
        return;
    }

    const QString previewPath = QDir(previewDirectory.path()).filePath(QStringLiteral("candidate.pdf"));
    pdf::PDFRepairDiffOptions diffOptions;
    diffOptions.renderDirectory = previewDirectory.path();
    pdf::PDFRepairDiffReport diffReport;
    const pdf::PDFOperationResult candidateResult = transaction.compareCandidate(previewPath, diffOptions, &diffReport);
    if (!candidateResult)
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), candidateResult.getErrorMessage());
        return;
    }

    QFile previewFile(previewPath);
    if (!previewFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), tr("The repair candidate could not be read after serialization."));
        return;
    }
    const QByteArray previewData = previewFile.readAll();
    previewFile.close();
    const QByteArray previewHash = QCryptographicHash::hash(previewData, QCryptographicHash::Sha256);

    RepairPreviewDialog previewDialog(m_widget);
    previewDialog.setReport(diffReport, previewDirectory.path());
    if (previewDialog.exec() != QDialog::Accepted)
    {
        return;
    }

    QFile candidateFile(previewPath);
    if (!candidateFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), tr("The reviewed repair candidate is no longer available."));
        return;
    }
    const QByteArray candidateData = candidateFile.readAll();
    candidateFile.close();
    if (QCryptographicHash::hash(candidateData, QCryptographicHash::Sha256) != previewHash)
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), tr("The repair candidate changed after preview. Review it again."));
        return;
    }

    const pdf::PDFOperationResult writeResult = pdf::PDFSafeFileWriter::writeData(
        outputPath, candidateData, pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!writeResult)
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), writeResult.getErrorMessage());
        return;
    }

    QFile finalFile(outputPath);
    if (!finalFile.open(QIODevice::ReadOnly) ||
        QCryptographicHash::hash(finalFile.readAll(), QCryptographicHash::Sha256) != previewHash)
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), tr("The final output does not match the approved repair candidate."));
        return;
    }
    finalFile.close();
    pdf::PDFDocumentReader finalReader(nullptr, [](bool*)
                                       { return QString(); }, false, false);
    finalReader.readFromFile(outputPath);
    if (finalReader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        QMessageBox::critical(m_widget, tr("Apply Bleed Fix"), tr("The final serialized output could not be reopened for verification."));
        return;
    }

    QMessageBox::information(m_widget, tr("Apply Bleed Fix"),
                             tr("Bleed fixup saved to %1. The open document was not modified.")
                                 .arg(QDir::toNativeSeparators(outputPath)));

    if (!rerunCheckBox->isChecked() || isPreflightRunning())
    {
        return;
    }

    QString pdfToolPath;
    QString profilePath;
    if (!resolvePreflightPaths(&pdfToolPath, &profilePath))
    {
        return;
    }

    startPreflightOnFile(outputPath,
                         profilePath,
                         m_documentRevision,
                         true,
                         tr("Post-fix results for: %1").arg(QDir::toNativeSeparators(outputPath)),
                         targetedChecksForRepair(operation, repairParameters, profilePath));
}

void LoupePreflightPlugin::onApplyDownsampleImagesRequested()
{
    if (!m_document || !m_reportDockWidget || !m_widget)
    {
        return;
    }

    const PreflightFixupEntry* fixup = m_reportDockWidget->fixup(QStringLiteral("downsample-images"));
    if (!fixup)
    {
        return;
    }

    QDialog dialog(m_widget);
    dialog.setWindowTitle(tr("Downsample Images"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QFormLayout* form = new QFormLayout();

    QSpinBox* dpiSpin = new QSpinBox(&dialog);
    dpiSpin->setRange(72, 1200);
    dpiSpin->setSuffix(tr(" DPI"));
    dpiSpin->setValue(qBound(72, fixup->params.value(QStringLiteral("target_dpi")).toInt(300), 1200));
    form->addRow(tr("Target resolution"), dpiSpin);

    QSpinBox* qualitySpin = new QSpinBox(&dialog);
    qualitySpin->setRange(50, 100);
    qualitySpin->setSuffix(tr("%"));
    qualitySpin->setValue(qBound(50, fixup->params.value(QStringLiteral("quality")).toInt(90), 100));
    form->addRow(tr("JPEG quality"), qualitySpin);

    const int candidateCount = fixup->params.value(QStringLiteral("candidate_count")).toInt(0);
    if (candidateCount > 0)
    {
        QLabel* candidateLabel = new QLabel(
            tr("%1 image(s) are significantly above the target resolution.").arg(candidateCount), &dialog);
        candidateLabel->setWordWrap(true);
        layout->addWidget(candidateLabel);
    }

    QLabel* safetyLabel = new QLabel(
        tr("Only images significantly above the target resolution will be resampled. "
           "Color mode and transparency are preserved; larger re-encodings are discarded."),
        &dialog);
    safetyLabel->setWordWrap(true);
    layout->addWidget(safetyLabel);

    QLineEdit* outputPathEdit = new QLineEdit(
        defaultDownsampleOutputPath(m_dataExchangeInterface->getOriginalFileName()), &dialog);
    QPushButton* browseButton = new QPushButton(tr("Browse..."), &dialog);
    QHBoxLayout* outputLayout = new QHBoxLayout();
    outputLayout->addWidget(outputPathEdit, 1);
    outputLayout->addWidget(browseButton);
    form->addRow(tr("Output file"), outputLayout);

    QCheckBox* rerunCheckBox = new QCheckBox(tr("Re-run preflight after fixing"), &dialog);
    rerunCheckBox->setChecked(true);
    layout->addLayout(form);
    layout->addWidget(rerunCheckBox);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, &dialog, [&dialog, outputPathEdit]()
            {
        const QString selectedPath = QFileDialog::getSaveFileName(
            &dialog, QObject::tr("Save downsampled PDF"), outputPathEdit->text(), QObject::tr("PDF files (*.pdf)"));
        if (!selectedPath.isEmpty())
        {
            outputPathEdit->setText(selectedPath);
        } });

    pdf::PDFWidgetUtils::style(&dialog);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const QString outputPath = outputPathEdit->text().trimmed();
    if (outputPath.isEmpty())
    {
        QMessageBox::warning(m_widget, tr("Downsample Images"), tr("Choose an output file path."));
        return;
    }
    if (QFile::exists(outputPath) && QMessageBox::warning(m_widget, tr("Downsample Images"),
                                                          tr("'%1' already exists. Overwrite it?").arg(QDir::toNativeSeparators(outputPath)),
                                                          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        return;
    }

    const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(QStringLiteral("downsample-images"));
    if (!operation)
    {
        QMessageBox::critical(m_widget, tr("Downsample Images"), tr("The downsample-images repair operation is unavailable."));
        return;
    }

    const QJsonObject repairParameters = QJsonObject{
        { QStringLiteral("target_dpi"), dpiSpin->value() },
        { QStringLiteral("quality"), qualitySpin->value() }
    };
    pdf::PDFRepairTransaction transaction(*m_document);
    const pdf::PDFOperationResult addResult = transaction.add(operation, repairParameters);
    if (!addResult || !transaction.analyze() || !transaction.apply())
    {
        const QString message = !addResult ? addResult.getErrorMessage()
                                           : tr("The downsample repair could not be planned or applied.");
        QMessageBox::critical(m_widget, tr("Downsample Images"), message);
        return;
    }

    const int changedImages = transaction.results().isEmpty()
                                  ? 0
                                  : transaction.results().front().changes.size();
    if (!writeReviewedRepairCandidate(transaction, outputPath, m_widget, tr("Downsample Images")))
    {
        return;
    }

    QMessageBox::information(
        m_widget,
        tr("Downsample Images"),
        tr("%1 image(s) changed.\nSaved to %2. The open document was not modified.")
            .arg(changedImages)
            .arg(QDir::toNativeSeparators(outputPath)));

    if (!rerunCheckBox->isChecked() || isPreflightRunning())
    {
        return;
    }

    QString pdfToolPath;
    QString profilePath;
    if (resolvePreflightPaths(&pdfToolPath, &profilePath))
    {
        startPreflightOnFile(outputPath,
                             profilePath,
                             m_documentRevision,
                             true,
                             tr("Post-fix results for: %1").arg(QDir::toNativeSeparators(outputPath)),
                             targetedChecksForRepair(operation, repairParameters, profilePath));
    }
}

void LoupePreflightPlugin::onApplyRgbToCmykFixupRequested()
{
    if (!m_document || !m_reportDockWidget || !m_widget)
    {
        return;
    }

    const auto* cmsManager = m_widget->getDrawWidgetProxy()->getCMSManager();
    if (!cmsManager)
    {
        QMessageBox::warning(m_widget, tr("RGB to CMYK"), tr("No color-management system is available."));
        return;
    }

    const pdf::PDFColorProfileIdentifiers& profiles = cmsManager->getCMYKProfiles();
    if (profiles.empty())
    {
        QMessageBox::warning(m_widget, tr("RGB to CMYK"), tr("No CMYK ICC profiles are available."));
        return;
    }

    QDialog dialog(m_widget);
    dialog.setWindowTitle(tr("Convert RGB to CMYK"));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QFormLayout* form = new QFormLayout();

    QComboBox* profileCombo = new QComboBox(&dialog);
    for (const pdf::PDFColorProfileIdentifier& profile : profiles)
    {
        profileCombo->addItem(profile.name.isEmpty() ? profile.id : profile.name,
                              profile.id);
    }
    form->addRow(tr("Target CMYK profile"), profileCombo);

    QComboBox* intentCombo = new QComboBox(&dialog);
    intentCombo->addItem(tr("Relative colorimetric"), int(pdf::RenderingIntent::RelativeColorimetric));
    intentCombo->addItem(tr("Perceptual"), int(pdf::RenderingIntent::Perceptual));
    intentCombo->addItem(tr("Absolute colorimetric"), int(pdf::RenderingIntent::AbsoluteColorimetric));
    intentCombo->addItem(tr("Saturation"), int(pdf::RenderingIntent::Saturation));
    form->addRow(tr("Rendering intent"), intentCombo);

    QCheckBox* blackPointCheck = new QCheckBox(tr("Black-point compensation"), &dialog);
    blackPointCheck->setChecked(true);
    form->addRow(QString(), blackPointCheck);

    QLineEdit* outputPathEdit = new QLineEdit(
        defaultRgbToCmykOutputPath(m_dataExchangeInterface->getOriginalFileName()), &dialog);
    QPushButton* browseButton = new QPushButton(tr("Browse..."), &dialog);
    QHBoxLayout* outputLayout = new QHBoxLayout();
    outputLayout->addWidget(outputPathEdit, 1);
    outputLayout->addWidget(browseButton);
    form->addRow(tr("Output file"), outputLayout);

    QCheckBox* rerunCheckBox = new QCheckBox(tr("Re-run preflight after conversion"), &dialog);
    rerunCheckBox->setChecked(true);
    layout->addLayout(form);
    layout->addWidget(rerunCheckBox);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, &dialog, [&dialog, outputPathEdit]()
            {
        const QString selectedPath = QFileDialog::getSaveFileName(
            &dialog, tr("Save CMYK PDF"), outputPathEdit->text(), tr("PDF files (*.pdf)"));
        if (!selectedPath.isEmpty())
        {
            outputPathEdit->setText(selectedPath);
        } });

    pdf::PDFWidgetUtils::style(&dialog);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const QString outputPath = outputPathEdit->text().trimmed();
    if (outputPath.isEmpty())
    {
        QMessageBox::warning(m_widget, tr("RGB to CMYK"), tr("Choose an output file path."));
        return;
    }
    if (QFile::exists(outputPath) && QMessageBox::warning(m_widget, tr("RGB to CMYK"),
                                                          tr("'%1' already exists. Overwrite it?").arg(QDir::toNativeSeparators(outputPath)),
                                                          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        return;
    }

    const int profileIndex = profileCombo->currentIndex();
    if (profileIndex < 0 || profileIndex >= int(profiles.size()))
    {
        return;
    }
    const pdf::PDFColorProfileIdentifier& profile = profiles.at(size_t(profileIndex));
    QByteArray profileData = profile.profileMemoryData;
    if (profileData.isEmpty())
    {
        QFile profileFile(profile.id);
        if (!profileFile.open(QIODevice::ReadOnly))
        {
            QMessageBox::critical(m_widget, tr("RGB to CMYK"), tr("Unable to read the selected ICC profile."));
            return;
        }
        profileData = profileFile.readAll();
    }

    const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(QStringLiteral("rgb-to-cmyk"));
    if (!operation)
    {
        QMessageBox::critical(m_widget, tr("RGB to CMYK"), tr("The rgb-to-cmyk repair operation is unavailable."));
        return;
    }

    const QJsonObject repairParameters = QJsonObject{
        { QStringLiteral("target_icc_base64"), QString::fromLatin1(profileData.toBase64()) },
        { QStringLiteral("target_icc_id"), profile.id },
        { QStringLiteral("target_profile_name"), profile.name },
        { QStringLiteral("intent"), intentCombo->currentData().toInt() },
        { QStringLiteral("black_point_compensation"), blackPointCheck->isChecked() },
        { QStringLiteral("embed_output_intent"), true }
    };
    pdf::PDFRepairTransaction transaction(*m_document);
    const pdf::PDFOperationResult addResult = transaction.add(operation, repairParameters);
    if (!addResult || !transaction.analyze() || !transaction.apply())
    {
        const QString message = !addResult ? addResult.getErrorMessage()
                                           : tr("The RGB-to-CMYK repair could not be planned or applied.");
        QMessageBox::critical(m_widget, tr("RGB to CMYK"), message);
        return;
    }

    const int changedAreas = transaction.results().isEmpty()
                                 ? 0
                                 : transaction.results().front().changes.size();
    if (!writeReviewedRepairCandidate(transaction, outputPath, m_widget, tr("RGB to CMYK")))
    {
        return;
    }

    QMessageBox::information(m_widget, tr("RGB to CMYK"),
                             tr("Applied %1 RGB-to-CMYK change(s) and saved the candidate PDF to %2. The open document was not modified.")
                                 .arg(changedAreas)
                                 .arg(QDir::toNativeSeparators(outputPath)));

    if (!rerunCheckBox->isChecked() || isPreflightRunning())
    {
        return;
    }

    QString pdfToolPath;
    QString profilePath;
    if (resolvePreflightPaths(&pdfToolPath, &profilePath))
    {
        startPreflightOnFile(outputPath,
                             profilePath,
                             m_documentRevision,
                             true,
                             tr("Post-conversion results for: %1").arg(QDir::toNativeSeparators(outputPath)),
                             targetedChecksForRepair(operation, repairParameters, profilePath));
    }
}

}   // namespace pdfplugin
