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

#include "jobsubmitter.h"

namespace pdfinteraction
{

PDFJobSchedulerSubmitter::PDFJobSchedulerSubmitter(pdf::PDFJobScheduler& scheduler) :
    m_scheduler(&scheduler)
{
}

QString PDFJobSchedulerSubmitter::submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work)
{
    return m_scheduler->submit(std::move(spec), std::move(work));
}

bool PDFJobSchedulerSubmitter::cancel(const QString& jobId)
{
    return m_scheduler->cancel(jobId);
}

pdf::PDFJobSnapshot PDFJobSchedulerSubmitter::snapshot(const QString& jobId) const
{
    return m_scheduler->snapshot(jobId);
}

void PDFJobSchedulerSubmitter::publishCurrentRevision(const QString& documentKey,
                                                      const pdf::PDFRevisionIdentity& revision)
{
    m_scheduler->setCurrentRevision(documentKey, revision.toString());
}

void PDFJobSchedulerSubmitter::clearCurrentRevision(const QString& documentKey)
{
    m_scheduler->clearCurrentRevision(documentKey);
}

}   // namespace pdfinteraction
