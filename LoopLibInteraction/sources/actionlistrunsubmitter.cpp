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

#include "actionlistrunsubmitter.h"

#include <stdexcept>

namespace pdfinteraction
{

ActionListRunWorker makeActionListRunWorker(ActionListRunPhase phase,
                                            pdf::PDFActionList actionList,
                                            pdf::PDFDocumentPointer document,
                                            QJsonObject bindings,
                                            std::shared_ptr<ActionListWorkerOutcome> outcome)
{
    return [phase, actionList = std::move(actionList), document = std::move(document), bindings = std::move(bindings), outcome = std::move(outcome)](pdf::PDFJobContext& context)
    {
        if (!outcome || !document)
        {
            throw std::runtime_error("Action List worker inputs are unavailable.");
        }
        if (context.isCancellationRequested())
        {
            return;
        }

        outcome->phase = phase;
        pdf::PDFActionListExecutionOptions options;
        options.bindings = bindings;
        options.operationControl = context.operationControl();
        pdf::PDFActionListExecutor executor;

        context.reportProgress(10);
        if (phase == ActionListRunPhase::Validate)
        {
            const pdf::PDFOperationResult validation = executor.validate(actionList, options, &outcome->validationErrors);
            outcome->ok = bool(validation);
            context.reportProgress(95);
            context.setResultSummary(outcome->ok ? QStringLiteral("Action List validated.")
                                                 : QStringLiteral("Action List validation failed."));
            return;
        }

        if (phase == ActionListRunPhase::Plan)
        {
            const pdf::PDFOperationResult planResult = executor.plan(actionList, *document, options, &outcome->executionResult);
            outcome->ok = bool(planResult);
            context.reportProgress(95);
            context.setResultSummary(outcome->ok ? QStringLiteral("Action List planned.")
                                                 : QStringLiteral("Action List planning failed."));
            return;
        }

        pdf::PDFDocument candidate;
        const pdf::PDFOperationResult executeResult = executor.execute(actionList, *document, options, &candidate, &outcome->executionResult);
        outcome->ok = bool(executeResult);
        if (context.isCancellationRequested())
        {
            return;
        }
        if (outcome->ok && candidate != pdf::PDFDocument())
        {
            outcome->candidate = pdf::PDFDocumentPointer(new pdf::PDFDocument(std::move(candidate)));
        }
        context.reportProgress(95);
        context.setResultSummary(outcome->ok ? QStringLiteral("Action List executed.")
                                             : QStringLiteral("Action List execution finished."));
    };
}

}   // namespace pdfinteraction
