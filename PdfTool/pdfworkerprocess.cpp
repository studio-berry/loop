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

#include "pdfworkerprocess.h"
#include "pdfworkerprotocol.h"

#ifndef Q_OS_WIN
#include <QProcess>
#include <QProcessEnvironment>

namespace pdftool
{
struct WorkerProcess::State
{
    QProcess process;
};

WorkerProcess::WorkerProcess() :
    m_state(std::make_unique<State>())
{
    auto* process = &m_state->process;
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process]
                     {
        if (process->bytesAvailable() > worker::MAX_RESPONSE_BYTES)
        {
            process->kill();
        } });
}
WorkerProcess::~WorkerProcess() { stop(); }

bool WorkerProcess::start(const QString& executable, const QStringList& arguments, const QString& inputDir,
                          const QString& tempDir, const QString& outputDir, QString& error)
{
    Q_UNUSED(inputDir);
    Q_UNUSED(outputDir);
    stop();
    auto& process = m_state->process;
    process.setStandardErrorFile(QProcess::nullDevice());
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    environment.insert(QStringLiteral("TMPDIR"), tempDir);
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(tempDir);
#if defined(Q_OS_UNIX)
    process.setUnixProcessParameters({ QProcess::UnixProcessFlag::CloseFileDescriptors |
                                       QProcess::UnixProcessFlag::DisableCoreDumps });
#endif
    process.start(executable, arguments);
    if (!process.waitForStarted(10000))
    {
        error = QStringLiteral("Isolated worker launch failed.");
        return false;
    }
    return true;
}

bool WorkerProcess::running() const { return m_state->process.state() != QProcess::NotRunning; }
qint64 WorkerProcess::pid() const { return m_state->process.processId(); }
qint64 WorkerProcess::exitCode() const { return m_state->process.exitCode(); }

void WorkerProcess::stop()
{
    if (running())
    {
        m_state->process.kill();
        m_state->process.waitForFinished(5000);
    }
    m_state->process.readAllStandardOutput();
}

bool WorkerProcess::write(const QByteArray& bytes, QElapsedTimer& timer, int timeoutMs, const std::atomic_bool& cancelled)
{
    auto& process = m_state->process;
    if (process.write(bytes) != bytes.size())
    {
        return false;
    }
    while (process.bytesToWrite() > 0 && timer.elapsed() < timeoutMs && !cancelled.load())
    {
        if (process.bytesAvailable() > worker::MAX_RESPONSE_BYTES)
            return false;
        process.waitForBytesWritten(qMin(10, timeoutMs - int(timer.elapsed())));
    }
    return process.bytesToWrite() == 0 && timer.elapsed() < timeoutMs && !cancelled.load();
}

QByteArray WorkerProcess::read(qint64 maximum)
{
    auto& process = m_state->process;
    process.waitForReadyRead(10);
    return process.read(qMin(maximum, qint64(65536)));
}
}
#endif
