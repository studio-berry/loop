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

#ifndef PDFTHREADAFFINITY_H
#define PDFTHREADAFFINITY_H

#include "pdfglobal.h"

#include <functional>

namespace pdf
{

/// Names the interactive thread, so expensive work can refuse to run on it.
///
/// Issue #144's rule is that nothing unpredictable happens on the thread that
/// answers input. Before this, the rule was a review question: a reviewer had
/// to notice that a new call reached preflight from a pointer handler. This
/// makes it a test failure instead.
///
/// It is a check, not a mechanism. Calling `requireNotInteractive` does not move
/// work anywhere -- it reports that work is in the wrong place. The fix for a
/// violation is always to submit the caller through pdf::PDFJobScheduler, never
/// to delete the guard.
///
/// Lives in Core because the blocking services do. A guard in the interaction
/// layer could only watch the calls that already go through it, which are the
/// ones that were never the problem.
class LOUPELIBCORESHARED_EXPORT PDFThreadAffinity
{
public:
    /// Called once, from the thread that will own input and frames. In the
    /// editor that is `main()` immediately after the application object exists.
    ///
    /// A process that never calls this has no interactive thread, and every
    /// `requireNotInteractive` passes. That is deliberate: PdfTool, the fuzzers
    /// and most unit tests have no interactive thread, and a guard that fired
    /// in them would be reporting the absence of a UI as a defect.
    static void markInteractiveThread();

    static bool isInteractiveThread() noexcept;

    /// Reports a violation when called on the interactive thread.
    ///
    /// `serviceName` is a short stable identifier -- "preflight", "ocr",
    /// "file-io" -- and appears in the report. It is a literal at the call
    /// site, never a path or anything derived from a document.
    static void requireNotInteractive(const char* serviceName);

    using ViolationHandler = std::function<void(const char* serviceName)>;

    /// Replaces what a violation does.
    ///
    /// The default reports through qCritical: loud in a log, fatal to no one.
    /// A crash would turn a latency defect into a data-loss defect, and the
    /// guard exists to protect responsiveness, not to punish it.
    ///
    /// A test installs a recording handler; that is what makes issue #144 AC5
    /// -- "thread-affinity tests fail when a blocking service is invoked on the
    /// interactive thread" -- an assertion rather than an aspiration.
    static void setViolationHandler(ViolationHandler handler);

    /// Clears the interactive thread and restores the default handler. For
    /// tests, which run several cases in one process and must not inherit the
    /// previous one's marking.
    static void resetForTesting();
};

}   // namespace pdf

#endif   // PDFTHREADAFFINITY_H
