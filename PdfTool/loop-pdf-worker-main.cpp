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

#include "pdfworkerruntime.h"
#include "pdfworkerprotocol.h"
#include "pdfworkersandbox.h"
#include "pdfapplicationidentity.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <cstdio>

namespace
{

int writeLine(const QJsonObject& object)
{
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout) != static_cast<size_t>(line.size()))
    {
        return 1;
    }
    fflush(stdout);
    return 0;
}

}   // namespace

int main(int argc, char* argv[])
{
    // loop-pdf-worker never initializes Sentry or an out-of-process crash
    // reporter. A hostile PDF that crashes this process must not produce a
    // customer-content minidump (R-008).
    QCoreApplication application(argc, argv);
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopPdfWorker);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Isolated LoopLibCore worker for untrusted open/preflight"));
    parser.addHelpOption();
    QCommandLineOption inputOption(QStringList{ QStringLiteral("sandbox-input") },
                                   QStringLiteral("Input file or directory the worker may read."),
                                   QStringLiteral("path"));
    QCommandLineOption tempOption(QStringList{ QStringLiteral("sandbox-temp") },
                                  QStringLiteral("Private temporary directory."),
                                  QStringLiteral("path"));
    QCommandLineOption outputOption(QStringList{ QStringLiteral("sandbox-output") },
                                    QStringLiteral("Output directory for published artifacts."),
                                    QStringLiteral("path"));
    QCommandLineOption rssOption(QStringList{ QStringLiteral("rss-limit-bytes") },
                                 QStringLiteral("Address-space RLIMIT in bytes."),
                                 QStringLiteral("bytes"));
    QCommandLineOption cpuOption(QStringList{ QStringLiteral("cpu-limit-seconds") },
                                 QStringLiteral("CPU RLIMIT in seconds."),
                                 QStringLiteral("seconds"));
    parser.addOption(inputOption);
    parser.addOption(tempOption);
    parser.addOption(outputOption);
    parser.addOption(rssOption);
    parser.addOption(cpuOption);
    parser.process(application);

    pdftool::worker::WorkerSandboxPaths paths;
    paths.inputPath = parser.value(inputOption);
    paths.tempDir = parser.value(tempOption);
    paths.outputDir = parser.value(outputOption);
    if (paths.inputPath.isEmpty() || paths.tempDir.isEmpty() || paths.outputDir.isEmpty())
    {
        QTextStream(stderr) << "loop-pdf-worker: --sandbox-input, --sandbox-temp, and --sandbox-output are required.\n";
        return 2;
    }

    pdftool::worker::WorkerSandboxLimits limits;
    if (parser.isSet(rssOption))
    {
        limits.rssBytes = parser.value(rssOption).toLongLong();
    }
    if (parser.isSet(cpuOption))
    {
        limits.cpuSeconds = parser.value(cpuOption).toLongLong();
    }

    QString sandboxError;
    const bool sandboxApplied = pdftool::worker::applyWorkerSandbox(paths, limits, &sandboxError);
#if defined(LOOP_PDF_WORKER_REQUIRE_SANDBOX) && LOOP_PDF_WORKER_REQUIRE_SANDBOX
    if (!sandboxApplied)
    {
        QTextStream(stderr) << "loop-pdf-worker: sandbox required but failed: " << sandboxError << '\n';
        return 3;
    }
#else
    if (!sandboxApplied)
    {
        QTextStream(stderr) << "loop-pdf-worker: sandbox unavailable: " << sandboxError << '\n';
    }
#endif

    pdftool::worker::WorkerRuntime runtime(paths);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
    {
        QTextStream(stderr) << "loop-pdf-worker: failed to open stdin.\n";
        return 4;
    }

    while (true)
    {
        const QByteArray line = input.readLine();
        if (line.isNull())
        {
            break;
        }
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty())
        {
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(trimmed, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            writeLine(pdftool::worker::makeErrorResponse(QString(), QStringLiteral("unknown"),
                                                         QStringLiteral("invalid-invocation"),
                                                         QStringLiteral("worker.bad-json"),
                                                         parseError.errorString()));
            continue;
        }

        const QJsonObject response = runtime.handleRequest(document.object());
        if (writeLine(response) != 0)
        {
            return 5;
        }
    }

    return 0;
}
