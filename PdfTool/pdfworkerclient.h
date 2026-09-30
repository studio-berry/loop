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

#ifndef PDFWORKERCLIENT_H
#define PDFWORKERCLIENT_H

#include <QJsonObject>
#include <QProcess>
#include <QString>

namespace pdftool
{

enum class WorkerClientOutcome
{
    Success,
    Incomplete,
    Unavailable,
    Cancelled,
    InvalidInvocation,
    InputError
};

struct WorkerClientResult
{
    WorkerClientOutcome outcome = WorkerClientOutcome::Unavailable;
    QJsonObject response;
    QString code;
    QString reason;
};

/// Supervisor-side client for loop-pdf-worker. Owns the child process lifetime
/// so a hostile worker crash cannot take down PdfTool.
class PdfWorkerClient
{
public:
    PdfWorkerClient();
    ~PdfWorkerClient();

    PdfWorkerClient(const PdfWorkerClient&) = delete;
    PdfWorkerClient& operator=(const PdfWorkerClient&) = delete;

    static QString defaultWorkerExecutable();

    bool start(const QString& workerExecutable,
               const QString& sandboxInput,
               const QString& sandboxTemp,
               const QString& sandboxOutput,
               QString* errorMessage);

    bool isRunning() const;
    qint64 workerPid() const;

    /// Kill the worker without restarting the supervisor.
    void killWorker();

    /// Kill and start a replacement worker with the same sandbox roots.
    bool replaceWorker(QString* errorMessage);

    WorkerClientResult ping(int timeoutMs = 10000);
    WorkerClientResult openDocument(const QString& inputPath,
                                    const QString& password = QString(),
                                    bool permissive = true,
                                    int timeoutMs = 60000);
    WorkerClientResult preflight(const QString& inputPath,
                                 const QString& profilePath,
                                 const QString& outputDir,
                                 const QString& password = QString(),
                                 bool permissive = true,
                                 int timeoutMs = 120000);
    WorkerClientResult cancel(int timeoutMs = 10000);

private:
    WorkerClientResult call(const QJsonObject& request, int timeoutMs);
    WorkerClientResult fromWorkerFailure(const QString& op, const QString& reason);

    QProcess m_process;
    QString m_workerExecutable;
    QString m_sandboxInput;
    QString m_sandboxTemp;
    QString m_sandboxOutput;
};

}   // namespace pdftool

#endif   // PDFWORKERCLIENT_H
