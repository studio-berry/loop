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

#ifndef OCRSERVICE_H
#define OCRSERVICE_H

#include "interactionglobal.h"
#include "jobrelay.h"
#include "jobsubmitter.h"

#include <QList>
#include <QString>

#include <functional>
#include <memory>

namespace pdfinteraction
{

/// One page to recognize.
struct OcrPageRequest
{
    int pageIndex = -1;
    QString documentKey;
    QString documentRevision;
};

/// What recognizing one page produced.
///
/// A value, not a handle. It crosses from a worker to the owner thread, so it
/// must not reference anything the worker owned.
struct OcrPageResult
{
    int pageIndex = -1;
    QString text;
    bool cancelled = false;
    QString error;

    bool isOk() const { return !cancelled && error.isEmpty(); }
};

/// Starts and cancels OCR runs.
class IOcrService
{
public:
    virtual ~IOcrService() = default;

    using Completion = std::function<void(QList<OcrPageResult>)>;

    /// Starts recognition and returns its job id, or an empty string when there
    /// is nothing to do. `completion` runs on the calling thread.
    virtual QString beginRecognize(QList<OcrPageRequest> pages, Completion completion) = 0;

    virtual bool cancel(const QString& jobId) = 0;
};

/// Puts OCR on the one scheduler.
///
/// This is a boundary, not a feature. There is no OCR surface in the editor and
/// this class does not add one; what it adds is that OCR work, whoever calls it,
/// is submitted as `PDFJobKind::OCR` at `Background` priority, carries a
/// cancellation token and a document revision, and returns through JobRelay.
/// Before this, OCR existed only as a `QProcess` sidecar driven synchronously
/// from PdfTool, reachable through no seam at all.
///
/// The backend is injected and this class knows nothing about `QProcess`. That
/// is what lets a test assert the submission -- kind, priority, revision fence,
/// cancellation -- without a Python sidecar, a model download, or a temporary
/// directory anywhere near it.
///
/// Note on scope: `PdfTool`'s existing CLI loop still drives the sidecar
/// directly. Hoisting it onto this service is a separate change; what protects
/// the interactive thread in the meantime is the `"ocr"` guard in
/// `OcrSidecarClient::sendRequest`. See `docs/ASYNC_BOUNDARY_DEFERRED.md`.
class SchedulerOcrService final : public IOcrService
{
public:
    /// Recognizes one page. Runs on a worker thread and must honour the
    /// context's cancellation.
    using Backend = std::function<OcrPageResult(const OcrPageRequest&, pdf::PDFJobContext&)>;

    SchedulerOcrService(IJobSubmitter& submitter, Backend backend);
    ~SchedulerOcrService() override;

    SchedulerOcrService(const SchedulerOcrService&) = delete;
    SchedulerOcrService& operator=(const SchedulerOcrService&) = delete;

    QString beginRecognize(QList<OcrPageRequest> pages, Completion completion) override;
    bool cancel(const QString& jobId) override;

private:
    IJobSubmitter* m_submitter = nullptr;
    Backend m_backend;
    std::shared_ptr<JobRelay> m_relay;
    quint64 m_sequence = 0;
};

}   // namespace pdfinteraction

#endif   // OCRSERVICE_H
