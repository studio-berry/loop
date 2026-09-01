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

#ifndef PREFLIGHTSERVICE_H
#define PREFLIGHTSERVICE_H

#include "interactionglobal.h"
#include "interactiontrace.h"
#include "jobrelay.h"
#include "jobsubmitter.h"

#include "pdfdocument.h"

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <memory>

namespace pdfinteraction
{

class PreflightController;

/// Everything a preflight run needs, captured at the moment it starts.
///
/// The document is held by shared pointer, not observed. A preflight run
/// outlives the click that started it, and a worker reading a document the GUI
/// thread has since replaced is the crash this field exists to prevent.
struct PreflightRunRequest
{
    QString documentKey;
    QString documentRevision;
    QString profileDigest;

    /// The resolved profile, as PreflightEngine already accepts it.
    QJsonObject profile;

    pdf::PDFDocumentPointer document;

    bool isValid() const { return !documentKey.isEmpty() && !document.isNull(); }
};

/// Starts and cancels preflight runs.
///
/// An interface rather than a concrete class because the interesting test is
/// "did the UI say Running before anything ran", and that is only observable if
/// the thing that runs the work can be replaced.
class IPreflightService
{
public:
    virtual ~IPreflightService() = default;

    /// Starts a run and returns its job id, or an empty string when the request
    /// is not runnable.
    virtual QString beginRun(PreflightRunRequest request) = 0;

    virtual bool cancel(const QString& jobId) = 0;
};

/// Runs preflight on pdf::PDFJobScheduler and reports through
/// PreflightController.
///
/// This is the producer PreflightController never had. The controller has
/// always been a complete state machine -- beginRun, updateProgress,
/// acceptResult, cancelRun, with revision and job-id fences on each -- with
/// nothing in the product calling it. Preflight in the editor could not be
/// started at all.
///
/// The ordering matters and is the point of issue #144 AC3:
///
///   1. `PreflightController::beginRun` runs synchronously, on the caller's
///      thread, before anything is submitted. The pane says Running on the same
///      frame as the click.
///   2. The work is submitted with the document's revision as the scheduler's
///      fence and `Discard` as the stale policy.
///   3. Progress and the result come back through JobRelay, so a completion
///      that arrives after this service is gone finds a detached relay.
///   4. The controller's own job-id and revision checks decide whether to
///      accept. A result for a superseded revision is dropped there, which is
///      why this class does not need a second staleness rule of its own.
///
/// The result crosses threads as a copy of `pdf::PreflightResult` -- a value,
/// not a handle into the worker's engine or session, both of which are
/// destroyed before the relay posts.
class SchedulerPreflightService final : public IPreflightService
{
public:
    SchedulerPreflightService(IJobSubmitter& submitter, PreflightController* controller);
    ~SchedulerPreflightService() override;

    SchedulerPreflightService(const SchedulerPreflightService&) = delete;
    SchedulerPreflightService& operator=(const SchedulerPreflightService&) = delete;

    QString beginRun(PreflightRunRequest request) override;
    bool cancel(const QString& jobId) override;

    /// Optional. When set, the run is reported to the trace as OCR-free
    /// Preflight work, so issue #144 AC7 can say whether a slow frame overlapped
    /// a preflight run.
    void setTraceRecorder(InteractionTraceRecorder* recorder);

    /// The job id of the run this service last started.
    QString activeJobId() const { return m_activeJobId; }

private:
    IJobSubmitter* m_submitter = nullptr;
    PreflightController* m_controller = nullptr;
    InteractionTraceRecorder* m_trace = nullptr;
    std::shared_ptr<JobRelay> m_relay;
    QString m_activeJobId;

    /// Set once the active run's "no longer running" has been reported to the
    /// trace, by whichever of the worker's completion and a cancel gets there
    /// first.
    ///
    /// Both can happen: cancelling a job the scheduler has not started yet
    /// means the work body never runs, so the completion path cannot be relied
    /// on to balance the start -- but it does run when the cancel lands mid-job.
    /// An unbalanced count would make every later frame look like it overlapped
    /// a preflight that finished long ago.
    std::shared_ptr<std::atomic_bool> m_runReported;

    quint64 m_sequence = 0;
};

}   // namespace pdfinteraction

#endif   // PREFLIGHTSERVICE_H
