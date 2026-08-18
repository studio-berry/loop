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

#include "ocrsidecarclient.h"

#include "pdftoolabstractapplication.h"
#include "pdfutils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>

namespace pdftool
{

namespace
{

bool isScript(const QFileInfo& info)
{
    const QString suffix = info.suffix();
#ifdef Q_OS_WIN
    return suffix.compare(QStringLiteral("cmd"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("bat"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("py"), Qt::CaseInsensitive) == 0;
#else
    return suffix.compare(QStringLiteral("sh"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("py"), Qt::CaseInsensitive) == 0;
#endif
}

}   // namespace

bool OcrSidecarClient::isRunnable(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
    {
        return false;
    }
#ifdef Q_OS_WIN
    return true;
#else
    // Repo checkout often lacks +x on scripts, and .gitattributes forces CRLF
    // which breaks shebang exec. Treat .sh/.py as runnable and launch via an
    // interpreter in OcrSidecarClient::start.
    return info.isExecutable() || isScript(info);
#endif
}

bool OcrSidecarClient::start(const QString& sidecarPath, QString& errorMessage)
{
    if (!isRunnable(sidecarPath))
    {
        errorMessage = PDFToolTranslationContext::tr("OCR sidecar not found or not runnable: %1").arg(sidecarPath);
        return false;
    }

    pdf::PDFSysUtils::configureScriptOrProgramProcess(m_process, sidecarPath);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.start();
    if (!m_process.waitForStarted(30000))
    {
        errorMessage = PDFToolTranslationContext::tr("Failed to start OCR sidecar: %1").arg(m_process.errorString());
        return false;
    }
    return true;
}

bool OcrSidecarClient::sendRequest(const QJsonObject& request,
                                   QJsonObject& response,
                                   QString& errorMessage)
{
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (m_process.write(line) != line.size())
    {
        errorMessage = PDFToolTranslationContext::tr("Failed to write OCR request to sidecar.");
        return false;
    }
    if (!m_process.waitForBytesWritten(30000))
    {
        errorMessage = PDFToolTranslationContext::tr("Timed out writing OCR request to sidecar.");
        return false;
    }

    // EasyOCR startup can take a while and native libs may emit blank stdout
    // lines; keep reading until a JSON object line arrives.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 120000)
    {
        if (!m_process.canReadLine())
        {
            if (!m_process.waitForReadyRead(qMax(1, 120000 - int(timer.elapsed()))))
            {
                break;
            }
            continue;
        }

        const QByteArray outputLine = m_process.readLine().trimmed();
        if (outputLine.isEmpty())
        {
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(outputLine, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            errorMessage = PDFToolTranslationContext::tr("Invalid OCR sidecar JSON: %1").arg(parseError.errorString());
            return false;
        }

        response = document.object();
        return true;
    }

    if (m_process.state() == QProcess::NotRunning)
    {
        errorMessage = PDFToolTranslationContext::tr("OCR sidecar process exited unexpectedly (exit code %1).").arg(m_process.exitCode());
    }
    else
    {
        errorMessage = PDFToolTranslationContext::tr("Timed out waiting for OCR sidecar response.");
    }
    return false;
}

void OcrSidecarClient::stop()
{
    if (m_process.state() != QProcess::NotRunning)
    {
        m_process.closeWriteChannel();
        m_process.waitForFinished(5000);
        if (m_process.state() != QProcess::NotRunning)
        {
            m_process.kill();
            m_process.waitForFinished(3000);
        }
    }
}

}   // namespace pdftool
