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

#include "preflightservice.h"

#include "preflightcontroller.h"

#include "pdfdocumentsession.h"
#include "preflightengine.h"

namespace pdfinteraction
{

SchedulerPreflightService::SchedulerPreflightService(IJobSubmitter& submitter, PreflightController* controller) :
    m_submitter(&submitter),
    m_controller(controller),
    m_relay(std::make_shared<JobRelay>())
{
}

SchedulerPreflightService::~SchedulerPreflightService()
{
    // On this thread, before anything else is torn down. A completion already
    // queued behind this call finds the relay detached and does nothing.
    m_relay->detach();
}

void SchedulerPreflightService::setTraceRecorder(InteractionTraceRecorder* recorder)
{
    m_trace = recorder;
}

QString SchedulerPreflightService::beginRun(PreflightRunRequest request)
{
    if (!m_submitter || !m_controller || !request.isValid())
    {
        return QString();
    }

    // One run at a time. Starting a second while the first is in flight would
    // leave the controller reporting whichever finished last, and the id fence
    // would silently drop the other -- correct, but invisible.
    if (!m_activeJobId.isEmpty())
    {
        cancel(m_activeJobId);
    }

    const QString jobId = QStringLiteral("preflight-%1").arg(++m_sequence);

    // Issue #144 AC3. Before the submit, on this thread: the pane shows Running
    // on the same frame as the click, whatever the queue depth is.
    m_controller->beginRun(request.documentKey, request.documentRevision, request.profileDigest, jobId);
    m_activeJobId = jobId;

    auto runReported = std::make_shared<std::atomic_bool>(false);
    m_runReported = runReported;

    if (m_trace)
    {
        m_trace->recordJobStateChange(TraceJobKind::Preflight, true);
    }

    pdf::PDFJobSpec spec;
    spec.jobId = jobId;
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.priority = pdf::PDFJobPriority::Operator;
    spec.documentKey = request.documentKey;
    spec.documentRevision = request.documentRevision;
    spec.operationId = QStringLiteral("preflight");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    // Captured by value. The lambda runs on a worker after this function has
    // returned, so a reference to `request` -- or to anything else on this
    // stack -- would be a reference to a frame that no longer exists.
    auto relay = m_relay;
    PreflightController* controller = m_controller;
    InteractionTraceRecorder* trace = m_trace;
    const QString revision = request.documentRevision;
    const QJsonObject profile = request.profile;
    const pdf::PDFDocumentPointer document = request.document;

    auto work = [relay, controller, trace, runReported, jobId, revision, profile, document](pdf::PDFJobContext& context)
    {
        const auto finish = [relay, controller, trace, runReported, jobId, revision](pdf::PreflightResult result)
        {
            relay->post([controller, trace, runReported, jobId, revision, result = std::move(result)]()
                        {
                            if (trace && !runReported->exchange(true))
                            {
                                trace->recordJobStateChange(TraceJobKind::Preflight, false);
                            }

                            // The controller re-checks the job id and the
                            // revision. A result for a document state the user
                            // has already moved past is dropped there, which is
                            // why there is no second staleness rule here.
                            controller->acceptResult(jobId, revision, result); });
        };

        if (context.isCancellationRequested())
        {
            pdf::PreflightResult cancelled;
            cancelled.inspectionComplete = false;
            cancelled.errorCode = QStringLiteral("cancelled");
            finish(std::move(cancelled));
            return;
        }

        pdf::PDFDocumentSession session(document.data());
        pdf::PreflightEngine engine(&session);
        engine.setOperationControl(context.operationControl());

        pdf::PreflightResult result = engine.run(profile, QJsonObject(), QJsonObject());

        if (context.isCancellationRequested())
        {
            pdf::PreflightResult cancelled;
            cancelled.inspectionComplete = false;
            cancelled.errorCode = QStringLiteral("cancelled");
            finish(std::move(cancelled));
            return;
        }

        // The result is copied out before `session` and `engine` are destroyed
        // at the end of this scope. Posting a handle into either would be a
        // read of a destroyed object on the owner's thread.
        finish(std::move(result));
    };

    const QString submitted = m_submitter->submit(spec, std::move(work));

    if (submitted.isEmpty())
    {
        m_controller->cancelRun(jobId);
        m_activeJobId.clear();

        if (m_trace && !runReported->exchange(true))
        {
            m_trace->recordJobStateChange(TraceJobKind::Preflight, false);
        }

        m_runReported.reset();
        return QString();
    }

    return submitted;
}

bool SchedulerPreflightService::cancel(const QString& jobId)
{
    if (!m_submitter || !m_controller || jobId.isEmpty())
    {
        return false;
    }

    // The controller first: cancellation is a UI state the user asked for, and
    // it should not wait on the scheduler noticing.
    const bool acknowledged = m_controller->cancelRun(jobId);
    m_submitter->cancel(jobId);

    if (jobId == m_activeJobId)
    {
        // A job cancelled before a worker picked it up never runs its body, so
        // the completion path will not balance the start. Whichever of the two
        // arrives first reports it; the exchange makes sure only one does.
        if (m_trace && m_runReported && !m_runReported->exchange(true))
        {
            m_trace->recordJobStateChange(TraceJobKind::Preflight, false);
        }

        m_runReported.reset();
        m_activeJobId.clear();
    }

    return acknowledged;
}

}   // namespace pdfinteraction
