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

#include "ocrplugin.h"
#include "ocrreportdockwidget.h"
#include "../pdftoolenvelopeutils.h"

#include "pdfdocumentwriter.h"
#include "pdfdrawspacecontroller.h"
#include "pdfdrawwidget.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QTemporaryDir>

namespace pdfplugin
{

OcrPlugin::OcrPlugin() :
    pdf::PDFPlugin(nullptr)
{
}

OcrPlugin::~OcrPlugin()
{
    cancelOcrRun(true);
}

void OcrPlugin::setWidget(pdf::PDFWidget* widget)
{
    Q_ASSERT(!m_widget);
    BaseClass::setWidget(widget);

    m_actionRunOcr = new QAction(tr("&Run OCR"), this);
    m_actionRunOcr->setObjectName("loupeocr_Run");
    m_actionRunOcr->setEnabled(false);
    m_actionRunOcr->setToolTip(tr("Runs PdfTool ocr via QProcess."));
    connect(m_actionRunOcr, &QAction::triggered, this, &OcrPlugin::onRunOcrTriggered);

    m_actionShowPanel = new QAction(tr("Show &OCR Panel"), this);
    m_actionShowPanel->setObjectName("loupeocr_ShowPanel");
    m_actionShowPanel->setCheckable(true);
    connect(m_actionShowPanel, &QAction::toggled, this, &OcrPlugin::onShowPanelTriggered);

    updateActions();
}

void OcrPlugin::setDocument(const pdf::PDFModifiedDocument& document)
{
    BaseClass::setDocument(document);

    if (document.hasReset())
    {
        cancelOcrRun(true);
        if (m_reportDockWidget)
        {
            m_reportDockWidget->clearReport();
        }
        ++m_documentRevision;
    }
    else if (document.hasPageContentsChanged()
             || document.hasFlag(pdf::PDFModifiedDocument::Annotation)
             || document.hasFlag(pdf::PDFModifiedDocument::FormField))
    {
        ++m_documentRevision;
        if (m_ocrProcess)
        {
            cancelOcrRun(true);
        }
    }

    updateActions();
}

std::vector<QAction*> OcrPlugin::getActions() const
{
    return { m_actionRunOcr, m_actionShowPanel };
}

QString OcrPlugin::getPluginMenuName() const
{
    return tr("Loupe &OCR");
}

void OcrPlugin::ensureDockWidget()
{
    if (m_reportDockWidget)
    {
        return;
    }

    QMainWindow* mainWindow = m_dataExchangeInterface->getMainWindow();
    m_reportDockWidget = new OcrReportDockWidget(mainWindow);
    m_reportDockWidget->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_reportDockWidget, Qt::Vertical);

    connect(m_reportDockWidget, &QDockWidget::visibilityChanged, this, [this](bool visible)
    {
        if (m_actionShowPanel && m_actionShowPanel->isChecked() != visible)
        {
            m_actionShowPanel->setChecked(visible);
        }
    });

    connect(m_reportDockWidget, &OcrReportDockWidget::pageActivated, this, &OcrPlugin::onPageActivated);
}

void OcrPlugin::updateActions()
{
    const bool hasDocument = m_document != nullptr;
    if (m_actionRunOcr)
    {
        m_actionRunOcr->setEnabled(hasDocument && !m_ocrProcess);
    }
}

bool OcrPlugin::resolvePdfToolPath(QString* pdfToolPath) const
{
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QString candidate = ocr::resolveBundlePath(applicationDirectory, QStringLiteral("../") + ocr::getPdfToolFileName());
    if (!QFileInfo::exists(candidate))
    {
        QMessageBox::critical(m_widget, tr("Loupe OCR"),
                              tr("Could not find PdfTool at %1.").arg(candidate));
        return false;
    }
    *pdfToolPath = candidate;
    return true;
}

void OcrPlugin::startOcrRun()
{
    if (!m_document || m_ocrProcess)
    {
        return;
    }

    QString pdfToolPath;
    if (!resolvePdfToolPath(&pdfToolPath))
    {
        return;
    }

    m_ocrTemporaryDirectory = std::make_unique<QTemporaryDir>();
    if (!m_ocrTemporaryDirectory->isValid())
    {
        QMessageBox::critical(m_widget, tr("Loupe OCR"), tr("Could not create a temporary directory for the OCR snapshot."));
        return;
    }

    const QString snapshotPath = m_ocrTemporaryDirectory->filePath(QStringLiteral("ocr-snapshot.pdf"));
    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult writeResult = writer.write(snapshotPath, m_document, false);
    if (!writeResult)
    {
        QMessageBox::critical(m_widget, tr("Loupe OCR"),
                              tr("Could not create an OCR snapshot: %1").arg(writeResult.getErrorMessage()));
        m_ocrTemporaryDirectory.reset();
        return;
    }

    m_ocrStdoutBuffer.clear();
    m_ocrStderrBuffer.clear();
    m_ocrProcess = new QProcess(this);
    m_ocrProcess->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_ocrProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus)
            {
                onOcrProcessFinished(exitCode, static_cast<int>(exitStatus));
            });
    connect(m_ocrProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
    {
        if (error == QProcess::FailedToStart)
        {
            onOcrProcessErrorOccurred();
        }
    });
    connect(m_ocrProcess, &QProcess::readyReadStandardOutput, this, &OcrPlugin::onOcrStdoutReady);
    connect(m_ocrProcess, &QProcess::readyReadStandardError, this, &OcrPlugin::onOcrStderrReady);

    updateActions();
    m_ocrProcess->start(pdfToolPath,
                        { QStringLiteral("ocr"),
                          snapshotPath,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") });
}

void OcrPlugin::onRunOcrTriggered()
{
    ensureDockWidget();
    m_reportDockWidget->show();
    m_actionShowPanel->setChecked(true);
    startOcrRun();
}

void OcrPlugin::onShowPanelTriggered(bool checked)
{
    ensureDockWidget();
    m_reportDockWidget->setVisible(checked);
}

void OcrPlugin::onOcrStdoutReady()
{
    if (!m_ocrProcess)
    {
        return;
    }
    m_ocrStdoutBuffer.append(m_ocrProcess->readAllStandardOutput());
}

void OcrPlugin::onOcrStderrReady()
{
    if (!m_ocrProcess)
    {
        return;
    }
    m_ocrStderrBuffer.append(m_ocrProcess->readAllStandardError());
}

void OcrPlugin::onOcrProcessFinished(int exitCode, int exitStatus)
{
    const QByteArray stdoutData = m_ocrStdoutBuffer;
    const QByteArray stderrData = m_ocrStderrBuffer;
    finishOcrRun();

    if (exitStatus != static_cast<int>(QProcess::NormalExit) || !ocr::isExpectedOcrExitCode(exitCode))
    {
        const QString detail = pdfplugin::pdftool::failureDetailFromStdout(
            stdoutData,
            QString::fromUtf8(stderrData).trimmed(),
            exitCode,
            tr("PdfTool ocr failed (exit %1).").arg(exitCode));

        QMessageBox::critical(m_widget,
                              tr("Loupe OCR"),
                              detail);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdoutData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        QMessageBox::critical(m_widget,
                              tr("Loupe OCR"),
                              tr("Invalid OCR JSON: %1").arg(parseError.errorString()));
        return;
    }

    const QJsonObject envelope = document.object();
    // #region agent log
    {
        QFile logFile(QStringLiteral("debug-1f0a11.log"));
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Append))
        {
            const QJsonObject payload{
                { QStringLiteral("sessionId"), QStringLiteral("1f0a11") },
                { QStringLiteral("hypothesisId"), QStringLiteral("A") },
                { QStringLiteral("location"), QStringLiteral("ocrplugin.cpp:onOcrProcessFinished") },
                { QStringLiteral("message"), QStringLiteral("parsed OCR envelope") },
                { QStringLiteral("data"), QJsonObject{
                      { QStringLiteral("isEnvelope"), pdfplugin::pdftool::isResultEnvelope(envelope, QStringLiteral("ocr")) },
                      { QStringLiteral("reportEmpty"), pdfplugin::pdftool::reportFromEnvelope(envelope).isEmpty() }
                  } },
                { QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch() }
            };
            logFile.write(QJsonDocument(payload).toJson(QJsonDocument::Compact));
            logFile.write("\n");
        }
    }
    // #endregion

    if (!pdfplugin::pdftool::isResultEnvelope(envelope, QStringLiteral("ocr")))
    {
        QMessageBox::critical(m_widget,
                              tr("Loupe OCR"),
                              tr("PdfTool returned an invalid result envelope."));
        return;
    }

    const QJsonObject report = pdfplugin::pdftool::reportFromEnvelope(envelope);
    if (report.isEmpty())
    {
        QMessageBox::critical(m_widget,
                              tr("Loupe OCR"),
                              tr("PdfTool returned a result without an OCR report."));
        return;
    }

    QString validationError;
    if (!applyReportJson(report, &validationError))
    {
        QMessageBox::critical(m_widget, tr("Loupe OCR"), validationError);
    }
}

void OcrPlugin::onOcrProcessErrorOccurred()
{
    QMessageBox::critical(m_widget, tr("Loupe OCR"), tr("Failed to start PdfTool ocr."));
    finishOcrRun();
}

void OcrPlugin::finishOcrRun()
{
    if (m_ocrProcess)
    {
        m_ocrProcess->deleteLater();
        m_ocrProcess = nullptr;
    }
    m_ocrTemporaryDirectory.reset();
    m_ocrStdoutBuffer.clear();
    m_ocrStderrBuffer.clear();
    updateActions();
}

void OcrPlugin::cancelOcrRun(bool silent)
{
    if (!m_ocrProcess)
    {
        return;
    }

    if (m_ocrProcess->state() != QProcess::NotRunning)
    {
        m_ocrProcess->kill();
        m_ocrProcess->waitForFinished(3000);
    }

    if (!silent)
    {
        QMessageBox::information(m_widget, tr("Loupe OCR"), tr("OCR run cancelled."));
    }

    finishOcrRun();
}

bool OcrPlugin::applyReportJson(const QJsonObject& report, QString* errorMessage)
{
    QString validationError;
    if (!ocr::validateOcrReport(report, &validationError))
    {
        if (errorMessage)
        {
            *errorMessage = validationError;
        }
        return false;
    }

    ensureDockWidget();
    m_reportDockWidget->setReport(report);
    m_reportDockWidget->show();
    m_actionShowPanel->setChecked(true);
    return true;
}

void OcrPlugin::onPageActivated(int pageIndex)
{
    if (!m_widget)
    {
        return;
    }
    m_widget->getDrawWidgetProxy()->goToPage(pageIndex);
}

}   // namespace pdfplugin
