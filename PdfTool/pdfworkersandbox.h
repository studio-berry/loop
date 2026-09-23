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

#ifndef PDFWORKERSANDBOX_H
#define PDFWORKERSANDBOX_H

#include <QJsonObject>
#include <QString>

namespace pdftool::worker
{

struct WorkerSandboxPaths
{
    QString inputPath;
    QString tempDir;
    QString outputDir;
};

struct WorkerSandboxLimits
{
    qint64 rssBytes = 0;
    qint64 cpuSeconds = 0;
};

/// Applies the Linux release-worker sandbox: no network (seccomp), Landlock FS
/// restriction to input/temp/output, and RLIMIT CPU/RSS. On non-Linux hosts this
/// returns false (Windows job-object hardening is a follow-on).
/// A missing sandbox for LOOP_PDF_WORKER_REQUIRE_SANDBOX builds is fatal.
bool applyWorkerSandbox(const WorkerSandboxPaths& paths,
                        const WorkerSandboxLimits& limits,
                        QString* errorMessage);

QJsonObject sandboxStatusJson(bool applied, const QString& detail);

}   // namespace pdftool::worker

#endif   // PDFWORKERSANDBOX_H
