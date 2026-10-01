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

#ifndef PDFWORKERPROCESS_H
#define PDFWORKERPROCESS_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QStringList>
#include <atomic>
#include <memory>

namespace pdftool
{
class WorkerProcess
{
public:
    WorkerProcess();
    ~WorkerProcess();
    bool start(const QString& executable, const QStringList& arguments, const QString& inputDir,
               const QString& tempDir, const QString& outputDir, QString& error);
    void stop();
    bool running() const;
    qint64 pid() const;
    qint64 exitCode() const;
    bool write(const QByteArray& bytes, QElapsedTimer& timer, int timeoutMs, const std::atomic_bool& cancelled);
    QByteArray read(qint64 maximum);

private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
#endif
