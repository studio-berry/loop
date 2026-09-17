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

#ifndef ACTIONLISTRUNSUBMITTER_H
#define ACTIONLISTRUNSUBMITTER_H

#include "interactionglobal.h"

#include "pdfactionlist.h"
#include "pdfdocument.h"
#include "pdfjobscheduler.h"

#include <QJsonObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace pdfinteraction
{

enum class ActionListRunPhase
{
    Validate,
    Plan,
    Execute
};

struct ActionListWorkerOutcome
{
    ActionListRunPhase phase = ActionListRunPhase::Validate;
    bool ok = false;
    QString errorMessage;
    QStringList validationErrors;
    QVector<pdf::PDFActionListStepResult> validationSteps;
    pdf::PDFActionListExecutionResult executionResult;
    pdf::PDFDocumentPointer candidate;
};

using ActionListRunWorker = std::function<void(pdf::PDFJobContext&)>;

ActionListRunWorker makeActionListRunWorker(ActionListRunPhase phase,
                                            pdf::PDFActionList actionList,
                                            pdf::PDFDocumentPointer document,
                                            QJsonObject bindings,
                                            std::shared_ptr<ActionListWorkerOutcome> outcome);

}   // namespace pdfinteraction

#endif   // ACTIONLISTRUNSUBMITTER_H
