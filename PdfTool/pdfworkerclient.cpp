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

#include "pdfworkerclient.h"

#include "pdfworkerprotocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

namespace pdftool
{

namespace
{

WorkerClientOutcome outcomeFromStatus(const QString& status)
{
    if (status == QLatin1String("success") || status == QLatin1String("findings"))
    {
        return WorkerClientOutcome::Success;
    }
    if (status == QLatin1String("incomplete") || status == QLatin1String("preflight-incomplete"))
    {
        return WorkerClientOutcome::Incomplete;
    }
    if (status == QLatin1String("cancelled"))
    {
        return WorkerClientOutcome::Cancelled;
    }
    if (status == QLatin1String("invalid-invocation"))
    {
        return WorkerClientOutcome::InvalidInvocation;
    }
    if (status == QLatin1String("input-error"))
    {
        return WorkerClientOutcome::InputError;
    }
    if (status == QLatin1String("unavailable") || status == QLatin1String("preflight-error"))
    {
        return status == QLatin1String("unavailable") ? WorkerClientOutcome::Unavailable
                                                      : WorkerClientOutcome::Incomplete;
    }
    return WorkerClientOutcome::Unavailable;
}

}   // namespace

PdfWorkerClient::PdfWorkerClient()
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
}

PdfWorkerClient::~PdfWorkerClient()
{
    killWorker();
}

QString PdfWorkerClient::defaultWorkerExecutable()
{
    const QDir dir(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    return dir.filePath(QStringLiteral("loop-pdf-worker.exe"));
#else
    return dir.filePath(QStringLiteral("loop-pdf-worker"));
#endif
}

bool PdfWorkerClient::start(const QString& workerExecutable,
                            const QString& sandboxInput,
                            const QString& sandboxTemp,
                            const QString& sandboxOutput,
                            QString* errorMessage)
{
    killWorker();
    m_workerExecutable = workerExecutable;
    m_sandboxInput = sandboxInput;
    m_sandboxTemp = sandboxTemp;
    m_sandboxOutput = sandboxOutput;

    if (!QFileInfo::exists(workerExecutable))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Worker executable not found: %1").arg(workerExecutable);
        }
        return false;
    }

    m_process.setProgram(workerExecutable);
    m_process.setArguments({
        QStringLiteral("--sandbox-input"),
        sandboxInput,
        QStringLiteral("--sandbox-temp"),
        sandboxTemp,
        QStringLiteral("--sandbox-output"),
        sandboxOutput,
    });
    m_process.start();
    if (!m_process.waitForStarted(30000))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Failed to start worker: %1").arg(m_process.errorString());
        }
        return false;
    }
    return true;
}

bool PdfWorkerClient::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

qint64 PdfWorkerClient::workerPid() const
{
    return m_process.processId();
}

void PdfWorkerClient::killWorker()
{
    if (m_process.state() == QProcess::NotRunning)
    {
        return;
    }
    m_process.kill();
    m_process.waitForFinished(5000);
}

bool PdfWorkerClient::replaceWorker(QString* errorMessage)
{
    return start(m_workerExecutable, m_sandboxInput, m_sandboxTemp, m_sandboxOutput, errorMessage);
}

WorkerClientResult PdfWorkerClient::fromWorkerFailure(const QString& op, const QString& reason)
{
    WorkerClientResult result;
    result.outcome = WorkerClientOutcome::Unavailable;
    result.code = QStringLiteral("worker.unavailable");
    result.reason = reason;
    result.response = worker::makeErrorResponse(QString(), op, QStringLiteral("unavailable"),
                                                result.code, reason);
    return result;
}

WorkerClientResult PdfWorkerClient::call(const QJsonObject& request, int timeoutMs)
{
    const QString op = request.value(QStringLiteral("op")).toString();
    if (!isRunning())
    {
        return fromWorkerFailure(op, QStringLiteral("Worker process is not running."));
    }

    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (m_process.write(line) != line.size())
    {
        return fromWorkerFailure(op, QStringLiteral("Failed to write request to worker."));
    }
    if (!m_process.waitForBytesWritten(timeoutMs))
    {
        killWorker();
        return fromWorkerFailure(op, QStringLiteral("Timed out writing request to worker."));
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs)
    {
        if (m_process.state() == QProcess::NotRunning)
        {
            return fromWorkerFailure(op, QStringLiteral("Worker exited unexpectedly (exit %1, status %2).")
                                             .arg(m_process.exitCode())
                                             .arg(static_cast<int>(m_process.exitStatus())));
        }
        if (!m_process.canReadLine())
        {
            if (!m_process.waitForReadyRead(qMax(1, timeoutMs - int(timer.elapsed()))))
            {
                continue;
            }
        }
        if (!m_process.canReadLine())
        {
            continue;
        }

        const QByteArray responseLine = m_process.readLine().trimmed();
        if (responseLine.isEmpty())
        {
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(responseLine, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            killWorker();
            return fromWorkerFailure(op, QStringLiteral("Worker returned invalid JSON."));
        }

        WorkerClientResult result;
        result.response = document.object();
        result.code = result.response.value(QStringLiteral("code")).toString();
        result.reason = result.response.value(QStringLiteral("reason")).toString();
        const QString status = result.response.value(QStringLiteral("status")).toString();
        if (result.response.value(QStringLiteral("ok")).toBool())
        {
            result.outcome = outcomeFromStatus(status.isEmpty() ? QStringLiteral("success") : status);
        }
        else
        {
            result.outcome = outcomeFromStatus(status.isEmpty() ? QStringLiteral("unavailable") : status);
        }
        return result;
    }

    killWorker();
    WorkerClientResult timedOut = fromWorkerFailure(op, QStringLiteral("Timed out waiting for worker response."));
    timedOut.outcome = WorkerClientOutcome::Incomplete;
    timedOut.code = QStringLiteral("worker.incomplete");
    timedOut.response = worker::makeErrorResponse(request.value(QStringLiteral("id")).toString(), op,
                                                  QStringLiteral("incomplete"), timedOut.code, timedOut.reason);
    return timedOut;
}

WorkerClientResult PdfWorkerClient::ping(int timeoutMs)
{
    return call(QJsonObject{
                    { QStringLiteral("v"), worker::PROTOCOL_VERSION },
                    { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                    { QStringLiteral("op"), QStringLiteral("ping") },
                },
                timeoutMs);
}

WorkerClientResult PdfWorkerClient::openDocument(const QString& inputPath,
                                                 const QString& password,
                                                 bool permissive,
                                                 int timeoutMs)
{
    return call(QJsonObject{
                    { QStringLiteral("v"), worker::PROTOCOL_VERSION },
                    { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                    { QStringLiteral("op"), QStringLiteral("open") },
                    { QStringLiteral("input_path"), inputPath },
                    { QStringLiteral("password"), password },
                    { QStringLiteral("permissive"), permissive },
                },
                timeoutMs);
}

WorkerClientResult PdfWorkerClient::preflight(const QString& inputPath,
                                              const QString& profilePath,
                                              const QString& outputDir,
                                              const QString& password,
                                              bool permissive,
                                              int timeoutMs)
{
    return call(QJsonObject{
                    { QStringLiteral("v"), worker::PROTOCOL_VERSION },
                    { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                    { QStringLiteral("op"), QStringLiteral("preflight") },
                    { QStringLiteral("input_path"), inputPath },
                    { QStringLiteral("profile_path"), profilePath },
                    { QStringLiteral("output_dir"), outputDir },
                    { QStringLiteral("password"), password },
                    { QStringLiteral("permissive"), permissive },
                },
                timeoutMs);
}

WorkerClientResult PdfWorkerClient::cancel(int timeoutMs)
{
    return call(QJsonObject{
                    { QStringLiteral("v"), worker::PROTOCOL_VERSION },
                    { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
                    { QStringLiteral("op"), QStringLiteral("cancel") },
                },
                timeoutMs);
}

}   // namespace pdftool
