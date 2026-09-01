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

#include "pdfthreadaffinity.h"

#include <QLoggingCategory>
#include <QThread>

#include <atomic>
#include <mutex>

namespace pdf
{

namespace
{

/// The interactive thread, or null when the process has none.
///
/// Atomic because `requireNotInteractive` is called from worker threads on
/// every expensive operation, and it must not take a lock to answer a question
/// whose answer almost never changes.
std::atomic<QThread*> s_interactiveThread{ nullptr };

/// Guards the handler only. A std::function cannot be read atomically, and the
/// handler is replaced at most a handful of times per process -- in a test's
/// init and cleanup -- so the mutex is uncontended in every real path.
std::mutex s_handlerMutex;
PDFThreadAffinity::ViolationHandler s_violationHandler;

void reportViolation(const char* serviceName)
{
    // Not qFatal. A responsiveness guard that aborts the process turns a
    // stuttering drag into lost work, which is a strictly worse outcome than
    // the defect it is reporting.
    qCritical("Thread affinity violation: '%s' was invoked on the interactive thread. "
              "Submit it through pdf::PDFJobScheduler instead.",
              serviceName ? serviceName : "<unnamed>");
}

}   // namespace

void PDFThreadAffinity::markInteractiveThread()
{
    s_interactiveThread.store(QThread::currentThread(), std::memory_order_release);
}

bool PDFThreadAffinity::isInteractiveThread() noexcept
{
    QThread* const interactive = s_interactiveThread.load(std::memory_order_acquire);
    return interactive != nullptr && interactive == QThread::currentThread();
}

void PDFThreadAffinity::requireNotInteractive(const char* serviceName)
{
    if (!isInteractiveThread())
    {
        return;
    }

    ViolationHandler handler;

    {
        const std::lock_guard<std::mutex> lock(s_handlerMutex);
        handler = s_violationHandler;
    }

    if (handler)
    {
        handler(serviceName);
        return;
    }

    reportViolation(serviceName);
}

void PDFThreadAffinity::setViolationHandler(ViolationHandler handler)
{
    const std::lock_guard<std::mutex> lock(s_handlerMutex);
    s_violationHandler = std::move(handler);
}

void PDFThreadAffinity::resetForTesting()
{
    s_interactiveThread.store(nullptr, std::memory_order_release);

    const std::lock_guard<std::mutex> lock(s_handlerMutex);
    s_violationHandler = nullptr;
}

}   // namespace pdf
