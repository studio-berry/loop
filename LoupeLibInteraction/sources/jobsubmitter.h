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

#ifndef JOBSUBMITTER_H
#define JOBSUBMITTER_H

#include "interactionglobal.h"

#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"

#include <QString>

namespace pdfinteraction
{

/// Submission seam over the one Core scheduler.
///
/// This is deliberately a narrow pass-through rather than a queue. Loupe has one
/// scheduler, pdf::PDFJobScheduler, which already owns priority classes, worker
/// capacity, cancellation tokens, and revision-based staleness. An implementation
/// of this interface adapts that scheduler or fakes it for a test; it never adds
/// a second queue, thread pool, or priority scheme.
class IJobSubmitter
{
public:
    virtual ~IJobSubmitter() = default;

    /// Submits work and returns the job id. The scheduler generates an id when
    /// spec.jobId is empty.
    virtual QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) = 0;

    /// Requests cancellation. Cancellation is terminal and is not success.
    virtual bool cancel(const QString& jobId) = 0;

    /// Returns the current snapshot for a job id.
    virtual pdf::PDFJobSnapshot snapshot(const QString& jobId) const = 0;

    /// Publishes the revision fence a document's queued work is measured against.
    ///
    /// The key is explicit on purpose. pdf::PDFJobScheduler keeps one
    /// current-revision entry per document key and the last writer wins, so two
    /// contexts sharing a key silently fight over one entry. Callers name the key
    /// they own rather than inheriting an ambient global.
    virtual void publishCurrentRevision(const QString& documentKey,
                                        const pdf::PDFRevisionIdentity& revision) = 0;

    /// Drops the fence entry for a document key. A key with no entry is never
    /// stale, so this belongs at document close, not between submissions.
    virtual void clearCurrentRevision(const QString& documentKey) = 0;
};

/// Adapts IJobSubmitter to an existing pdf::PDFJobScheduler.
///
/// The scheduler is referenced, never owned. Construct with
/// pdf::PDFJobScheduler::global() in product code, or with a locally owned
/// scheduler in a test that needs deterministic worker capacity.
class PDFJobSchedulerSubmitter final : public IJobSubmitter
{
public:
    explicit PDFJobSchedulerSubmitter(pdf::PDFJobScheduler& scheduler);

    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override;
    bool cancel(const QString& jobId) override;
    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override;
    void publishCurrentRevision(const QString& documentKey,
                                const pdf::PDFRevisionIdentity& revision) override;
    void clearCurrentRevision(const QString& documentKey) override;

    /// Returns the adapted scheduler.
    pdf::PDFJobScheduler& scheduler() const noexcept { return *m_scheduler; }

private:
    pdf::PDFJobScheduler* m_scheduler = nullptr;
};

}   // namespace pdfinteraction

#endif   // JOBSUBMITTER_H
