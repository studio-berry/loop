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

#ifndef PDFWORKERRUNTIME_H
#define PDFWORKERRUNTIME_H

#include "pdfworkersandbox.h"

#include <QJsonObject>
#include <atomic>

namespace pdftool::worker
{

class WorkerRuntime
{
public:
    explicit WorkerRuntime(WorkerSandboxPaths sandboxPaths);

    QJsonObject handleRequest(const QJsonObject& request);
    void requestCancel();

private:
    QJsonObject handlePing(const QString& id);
    QJsonObject handleOpen(const QString& id, const QJsonObject& request);
    QJsonObject handlePreflight(const QString& id, const QJsonObject& request);
    QJsonObject handleCancel(const QString& id);

    bool pathIsInsideSandbox(const QString& candidate, const QString& root) const;

    WorkerSandboxPaths m_sandboxPaths;
    QJsonObject m_sandboxStatus;
    std::atomic_bool m_cancelRequested{ false };
};

}   // namespace pdftool::worker

#endif   // PDFWORKERRUNTIME_H
