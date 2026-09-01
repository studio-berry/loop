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

#include "ocrservice.h"

#include <utility>

namespace pdfinteraction
{

SchedulerOcrService::SchedulerOcrService(IJobSubmitter& submitter, Backend backend) :
    m_submitter(&submitter),
    m_backend(std::move(backend)),
    m_relay(std::make_shared<JobRelay>())
{
}

SchedulerOcrService::~SchedulerOcrService()
{
    m_relay->detach();
}

QString SchedulerOcrService::beginRecognize(QList<OcrPageRequest> pages, Completion completion)
{
    if (!m_submitter || !m_backend || pages.isEmpty())
    {
        return QString();
    }

    const QString jobId = QStringLiteral("ocr-%1").arg(++m_sequence);

    pdf::PDFJobSpec spec;
    spec.jobId = jobId;
    spec.kind = pdf::PDFJobKind::OCR;

    // Background, not Operator. OCR is long, and the scheduler reserves a
    // worker against the background class precisely so a run of it cannot
    // starve the interactive and visible-page work.
    spec.priority = pdf::PDFJobPriority::Background;
    spec.documentKey = pages.constFirst().documentKey;
    spec.documentRevision = pages.constFirst().documentRevision;
    spec.operationId = QStringLiteral("ocr");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;

    auto relay = m_relay;
    Backend backend = m_backend;

    auto work = [relay, backend, pages, completion = std::move(completion)](pdf::PDFJobContext& context)
    {
        QList<OcrPageResult> results;
        results.reserve(pages.size());

        for (const OcrPageRequest& page : pages)
        {
            if (context.isCancellationRequested())
            {
                // The pages already recognized are kept and the run is reported
                // as cancelled. Discarding them would throw away work the user
                // paid for; reporting it as complete would be a lie about
                // coverage.
                OcrPageResult cancelled;
                cancelled.pageIndex = page.pageIndex;
                cancelled.cancelled = true;
                results.push_back(cancelled);
                break;
            }

            results.push_back(backend(page, context));
        }

        if (!completion)
        {
            return;
        }

        relay->post([completion, results = std::move(results)]() { completion(results); });
    };

    return m_submitter->submit(spec, std::move(work));
}

bool SchedulerOcrService::cancel(const QString& jobId)
{
    if (!m_submitter || jobId.isEmpty())
    {
        return false;
    }

    return m_submitter->cancel(jobId);
}

}   // namespace pdfinteraction
