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

#include "preflightclirun.h"

#include "pdfdocumentsession.h"
#include "pdfoperationcontrol.h"

#include <cstdio>
#include <memory>

namespace pdf
{

namespace
{

void logPreflightStage(const char* stage)
{
    fprintf(stderr, "loop-pdftool preflight stage=%s\n", stage);
    fflush(stderr);
}

}   // namespace

PreflightFileInspectionOutcome inspectPreflightFile(const PreflightFileInspectionRequest& request)
{
    PreflightFileInspectionOutcome outcome;
    logPreflightStage("begin");

    bool isFirstPasswordAttempt = true;
    const auto passwordCallback = [&request, &isFirstPasswordAttempt](bool* ok) -> QString
    {
        *ok = isFirstPasswordAttempt;
        isFirstPasswordAttempt = false;
        return request.password;
    };

    PDFDocumentReader reader(nullptr, passwordCallback, request.permissiveReading, false);
    logPreflightStage("before_read");
    std::unique_ptr<PDFDocument, void (*)(PDFDocument*)> document(reader.readFromFileOnHeap(request.documentPath),
                                                                  &PDFDocumentReader::destroyDocument);

    outcome.readResult = reader.getReadingResult();
    outcome.readErrorMessage = reader.getErrorMessage();
    outcome.readWarnings = reader.getWarnings();
    outcome.documentReadOk = outcome.readResult == PDFDocumentReader::Result::OK;
    if (!outcome.documentReadOk)
    {
        logPreflightStage("read_failed");
        return outcome;
    }

    outcome.sourceData = reader.getSource();
    logPreflightStage("after_read");

    std::unique_ptr<PDFDocumentSession, void (*)(PDFDocumentSession*)> session(
        PDFDocumentSession::createForInspection(document.get()),
        &PDFDocumentSession::destroy);
    logPreflightStage("after_session");

    PreflightEngine engine(session.get());
    engine.setOperationControl(request.cancellation);

    logPreflightStage("before_run");
    if (!PDFOperationControl::isOperationCancelled(request.cancellation))
    {
        outcome.report = engine.run(request.profile, request.jobSpec, request.cliBindings, request.plan);
        outcome.inspectionRan = true;
    }
    logPreflightStage("after_run");

#if defined(Q_OS_WIN)
    document.release();
    session.release();
#endif

    logPreflightStage("end");
    return outcome;
}

}   // namespace pdf
