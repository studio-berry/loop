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

#include "pdftoolworker.h"

#include "pdfworkerclient.h"
#include "pdftoolresult.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace pdftool
{

namespace
{

PDFToolExitCode mapWorkerOutcome(WorkerClientOutcome outcome)
{
    switch (outcome)
    {
        case WorkerClientOutcome::Success:
            return PDFToolExitCode::Success;
        case WorkerClientOutcome::Incomplete:
            return PDFToolExitCode::PreflightIncomplete;
        case WorkerClientOutcome::Unavailable:
            return PDFToolExitCode::ProcessingFailure;
        case WorkerClientOutcome::Cancelled:
            return PDFToolExitCode::Cancelled;
        case WorkerClientOutcome::InvalidInvocation:
            return PDFToolExitCode::InvalidInvocation;
        case WorkerClientOutcome::InputError:
            return PDFToolExitCode::InputError;
    }
    return PDFToolExitCode::ProcessingFailure;
}

QString resolveWorkerExecutable()
{
    const QByteArray overridePath = qgetenv("LOOP_PDF_WORKER_PATH");
    if (!overridePath.isEmpty())
    {
        return QString::fromLocal8Bit(overridePath);
    }
    return PdfWorkerClient::defaultWorkerExecutable();
}

bool stageProfile(const QString& profilePath, const QString& tempDir, QString* stagedPath, QString* error)
{
    const QFileInfo info(profilePath);
    if (!info.exists() || !info.isFile())
    {
        if (error)
        {
            *error = PDFToolTranslationContext::tr("Profile not found: %1").arg(profilePath);
        }
        return false;
    }
    const QString target = QDir(tempDir).filePath(info.fileName());
    if (QFile::exists(target))
    {
        QFile::remove(target);
    }
    if (!QFile::copy(profilePath, target))
    {
        if (error)
        {
            *error = PDFToolTranslationContext::tr("Failed to stage profile into worker temp.");
        }
        return false;
    }
    *stagedPath = target;
    return true;
}

void publishWorkerResult(const PDFToolOptions& options, const WorkerClientResult& result)
{
    if (options.executionContext)
    {
        QJsonObject data = result.response;
        data.insert(QStringLiteral("worker_code"), result.code);
        data.insert(QStringLiteral("worker_reason"), result.reason);
        options.executionContext->setData(data);
    }
}

}   // namespace

static PDFToolWorkerPingApplication s_workerPing;
static PDFToolWorkerOpenApplication s_workerOpen;
static PDFToolWorkerPreflightApplication s_workerPreflight;

QString PDFToolWorkerPingApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("worker-ping");
        case Name:
            return PDFToolTranslationContext::tr("Worker Ping");
        case Description:
            return PDFToolTranslationContext::tr("Ping the isolated loop-pdf-worker process.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolAbstractApplication::Options PDFToolWorkerPingApplication::getOptionsFlags() const
{
    return ConsoleFormat;
}

PDFToolExitCode PDFToolWorkerPingApplication::execute(const PDFToolOptions& options)
{
    QTemporaryDir tempDir;
    QTemporaryDir outputDir;
    if (!tempDir.isValid() || !outputDir.isValid())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.temp"),
                         PDFToolTranslationContext::tr("Failed to create worker sandbox directories."));
        return PDFToolExitCode::InternalError;
    }

    // Sandbox input root is the temp dir for ping (no document).
    const QString inputRoot = tempDir.path();
    PdfWorkerClient client;
    QString error;
    if (!client.start(resolveWorkerExecutable(), inputRoot, tempDir.path(), outputDir.path(), &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.unavailable"), error);
        return PDFToolExitCode::ProcessingFailure;
    }

    const WorkerClientResult result = client.ping();
    publishWorkerResult(options, result);
    if (result.outcome != WorkerClientOutcome::Success)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error,
                         result.code.isEmpty() ? QStringLiteral("worker.unavailable") : result.code,
                         result.reason);
    }
    else if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        PDFConsole::writeText(QStringLiteral("worker ping ok"), options.outputCodec);
    }
    return mapWorkerOutcome(result.outcome);
}

QString PDFToolWorkerOpenApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("worker-open");
        case Name:
            return PDFToolTranslationContext::tr("Worker Open");
        case Description:
            return PDFToolTranslationContext::tr("Open an untrusted PDF in loop-pdf-worker and return artifact identity.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolAbstractApplication::Options PDFToolWorkerOpenApplication::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument;
}

PDFToolExitCode PDFToolWorkerOpenApplication::execute(const PDFToolOptions& options)
{
    const QString inputPath = QFileInfo(options.document).absoluteFilePath();
    if (!QFileInfo::exists(inputPath))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.input"),
                         PDFToolTranslationContext::tr("Document not found: %1").arg(options.document));
        return PDFToolExitCode::InputError;
    }

    QTemporaryDir tempDir;
    QTemporaryDir outputDir;
    if (!tempDir.isValid() || !outputDir.isValid())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.temp"),
                         PDFToolTranslationContext::tr("Failed to create worker sandbox directories."));
        return PDFToolExitCode::InternalError;
    }

    PdfWorkerClient client;
    QString error;
    if (!client.start(resolveWorkerExecutable(), inputPath, tempDir.path(), outputDir.path(), &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.unavailable"), error);
        return PDFToolExitCode::ProcessingFailure;
    }

    const qint64 firstPid = client.workerPid();
    const WorkerClientResult result = client.openDocument(inputPath, options.password, options.permissiveReading);
    publishWorkerResult(options, result);

    if (result.outcome != WorkerClientOutcome::Success)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error,
                         result.code.isEmpty() ? QStringLiteral("worker.unavailable") : result.code,
                         result.reason.isEmpty() ? PDFToolTranslationContext::tr("Worker open failed.") : result.reason);
        // Prove the supervisor can replace a dead worker without restarting.
        if (!client.isRunning())
        {
            QString replaceError;
            if (client.replaceWorker(&replaceError) && options.executionContext)
            {
                const WorkerClientResult ping = client.ping();
                QJsonObject data = result.response;
                data.insert(QStringLiteral("replaced_worker"), true);
                data.insert(QStringLiteral("previous_pid"), firstPid);
                data.insert(QStringLiteral("replacement_pid"), client.workerPid());
                data.insert(QStringLiteral("replacement_ping_ok"), ping.outcome == WorkerClientOutcome::Success);
                options.executionContext->setData(data);
            }
        }
        return mapWorkerOutcome(result.outcome);
    }

    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        const QJsonObject artifact = result.response.value(QStringLiteral("artifact")).toObject();
        PDFConsole::writeText(QStringLiteral("worker-open ok sha256=%1 pages=%2")
                                  .arg(artifact.value(QStringLiteral("sha256")).toString())
                                  .arg(result.response.value(QStringLiteral("page_count")).toInt()),
                              options.outputCodec);
    }
    return PDFToolExitCode::Success;
}

QString PDFToolWorkerPreflightApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("worker-preflight");
        case Name:
            return PDFToolTranslationContext::tr("Worker Preflight");
        case Description:
            return PDFToolTranslationContext::tr("Run preflight inside loop-pdf-worker for an untrusted PDF.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolAbstractApplication::Options PDFToolWorkerPreflightApplication::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PreflightProfile;
}

PDFToolExitCode PDFToolWorkerPreflightApplication::execute(const PDFToolOptions& options)
{
    const QString inputPath = QFileInfo(options.document).absoluteFilePath();
    if (!QFileInfo::exists(inputPath))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.input"),
                         PDFToolTranslationContext::tr("Document not found: %1").arg(options.document));
        return PDFToolExitCode::InputError;
    }
    if (options.preflightProfilePath.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.profile"),
                         PDFToolTranslationContext::tr("A preflight profile is required."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QTemporaryDir tempDir;
    QTemporaryDir outputDir;
    if (!tempDir.isValid() || !outputDir.isValid())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.temp"),
                         PDFToolTranslationContext::tr("Failed to create worker sandbox directories."));
        return PDFToolExitCode::InternalError;
    }

    QString stagedProfile;
    QString stageError;
    if (!stageProfile(options.preflightProfilePath, tempDir.path(), &stagedProfile, &stageError))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.profile"), stageError);
        return PDFToolExitCode::InputError;
    }

    PdfWorkerClient client;
    QString error;
    if (!client.start(resolveWorkerExecutable(), inputPath, tempDir.path(), outputDir.path(), &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("worker.unavailable"), error);
        return PDFToolExitCode::ProcessingFailure;
    }

    const WorkerClientResult result = client.preflight(inputPath, stagedProfile, outputDir.path(),
                                                       options.password, options.permissiveReading);
    publishWorkerResult(options, result);

    if (result.outcome == WorkerClientOutcome::Success)
    {
        if (options.outputStyle != PDFOutputFormatter::Style::Json)
        {
            PDFConsole::writeText(QStringLiteral("worker-preflight ok status=%1")
                                      .arg(result.response.value(QStringLiteral("status")).toString()),
                                  options.outputCodec);
        }
        // Findings are still a successful isolated run (not PASS-as-clean when
        // status is findings); map findings to Findings exit when reported.
        if (result.response.value(QStringLiteral("status")).toString() == QLatin1String("findings"))
        {
            return PDFToolExitCode::Findings;
        }
        return PDFToolExitCode::Success;
    }

    reportDiagnostic(options, PDFToolDiagnosticSeverity::Error,
                     result.code.isEmpty() ? QStringLiteral("worker.unavailable") : result.code,
                     result.reason.isEmpty() ? PDFToolTranslationContext::tr("Worker preflight failed.") : result.reason);
    return mapWorkerOutcome(result.outcome);
}

}   // namespace pdftool
