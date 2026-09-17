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

#include "actionlistcontroller.h"

namespace pdfinteraction
{

ActionListController::ActionListController(pdf::PDFJobScheduler* scheduler, QObject* parent) :
    QObject(parent),
    m_steps(this),
    m_scheduler(scheduler ? scheduler : &pdf::PDFJobScheduler::global())
{
    qRegisterMetaType<State>();
}

void ActionListController::setState(State state)
{
    if (m_state == state)
    {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(m_state);
}

void ActionListController::cancelSchedulerJob()
{
    if (!m_scheduler || m_jobId.isEmpty())
    {
        return;
    }
    const pdf::PDFJobSnapshot snapshot = m_scheduler->snapshot(m_jobId);
    if (snapshot.jobId == m_jobId &&
        (snapshot.status == pdf::PDFJobStatus::Queued || snapshot.status == pdf::PDFJobStatus::Running))
    {
        m_scheduler->cancel(m_jobId);
    }
}

void ActionListController::setCurrentRevision(QString documentKey, QString documentRevision)
{
    const bool changed = documentKey != m_documentKey || documentRevision != m_documentRevision;
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    if (changed && (m_state == State::Validating || m_state == State::Planning || m_state == State::Running ||
                    m_state == State::Planned))
    {
        markRecipeStale();
    }
}

void ActionListController::markRecipeStale()
{
    if (m_state == State::Idle && m_validatedRecipeHash.isEmpty())
    {
        return;
    }
    cancelSchedulerJob();
    m_cancelRequested = true;
    m_operatorSummary = QStringLiteral("Action List results are stale for the current document or recipe inputs.");
    setState(State::Idle);
    m_steps.clear();
    m_result = pdf::PDFActionListExecutionResult();
    m_validatedDocumentKey.clear();
    m_validatedDocumentRevision.clear();
    m_validatedRecipeHash.clear();
    m_validatedBindingsHash.clear();
    m_plannedDocumentKey.clear();
    m_plannedDocumentRevision.clear();
    m_plannedRecipeHash.clear();
    m_plannedBindingsHash.clear();
    Q_EMIT resultChanged();
}

void ActionListController::beginRun(State phase,
                                    QString documentKey,
                                    QString documentRevision,
                                    QString recipeId,
                                    QString recipeHash,
                                    QString bindingsHash,
                                    QString jobId)
{
    m_documentKey = std::move(documentKey);
    m_documentRevision = std::move(documentRevision);
    m_recipeId = std::move(recipeId);
    m_runRecipeHash = std::move(recipeHash);
    m_runBindingsHash = std::move(bindingsHash);
    m_jobId = std::move(jobId);
    m_cancelRequested = false;
    m_progress = 0;
    m_operatorSummary.clear();
    m_result = pdf::PDFActionListExecutionResult();
    m_steps.clear();
    setState(phase);
    Q_EMIT progressChanged(0);
    Q_EMIT resultChanged();
}

bool ActionListController::validationMatches(const QString& documentKey,
                                             const QString& documentRevision,
                                             const QString& recipeHash,
                                             const QString& bindingsHash) const
{
    return !m_validatedRecipeHash.isEmpty() && m_state == State::Idle &&
           documentKey == m_validatedDocumentKey && documentRevision == m_validatedDocumentRevision &&
           recipeHash == m_validatedRecipeHash && bindingsHash == m_validatedBindingsHash;
}

bool ActionListController::planMatches(const QString& documentKey,
                                       const QString& documentRevision,
                                       const QString& recipeHash,
                                       const QString& bindingsHash) const
{
    return m_state == State::Planned && documentKey == m_plannedDocumentKey &&
           documentRevision == m_plannedDocumentRevision && recipeHash == m_plannedRecipeHash &&
           bindingsHash == m_plannedBindingsHash;
}

bool ActionListController::updateProgress(const QString& jobId, const QString& documentRevision, int progress)
{
    if (jobId != m_jobId || documentRevision != m_documentRevision ||
        (m_state != State::Validating && m_state != State::Planning && m_state != State::Running))
    {
        return false;
    }
    m_progress = qBound(0, progress, 100);
    Q_EMIT progressChanged(m_progress);
    return true;
}

bool ActionListController::acceptValidation(const QString& jobId,
                                            const QString& documentRevision,
                                            const QStringList& errors,
                                            const QVector<pdf::PDFActionListStepResult>& validationSteps)
{
    if (jobId != m_jobId || documentRevision != m_documentRevision || m_state != State::Validating || m_cancelRequested)
    {
        return false;
    }

    m_progress = 100;
    Q_EMIT progressChanged(m_progress);
    if (!errors.isEmpty())
    {
        m_validatedDocumentKey.clear();
        m_validatedDocumentRevision.clear();
        m_validatedRecipeHash.clear();
        m_validatedBindingsHash.clear();
        m_plannedDocumentKey.clear();
        m_plannedDocumentRevision.clear();
        m_plannedRecipeHash.clear();
        m_plannedBindingsHash.clear();
        m_steps.replace(validationSteps);
        m_operatorSummary = errors.join(QLatin1Char('\n'));
        setState(State::Failed);
        return true;
    }

    m_validatedDocumentKey = m_documentKey;
    m_validatedDocumentRevision = documentRevision;
    m_validatedRecipeHash = m_runRecipeHash;
    m_validatedBindingsHash = m_runBindingsHash;
    m_operatorSummary = QStringLiteral("Action List recipe validated.");
    setState(State::Idle);
    Q_EMIT resultChanged();
    return true;
}

bool ActionListController::acceptPlan(const QString& jobId,
                                      const QString& documentRevision,
                                      const pdf::PDFActionListExecutionResult& result)
{
    if (jobId != m_jobId || documentRevision != m_documentRevision || m_state != State::Planning || m_cancelRequested)
    {
        return false;
    }

    m_result = result;
    m_plannedDocumentKey = m_documentKey;
    m_plannedDocumentRevision = documentRevision;
    m_plannedRecipeHash = m_runRecipeHash;
    m_plannedBindingsHash = m_runBindingsHash;
    m_steps.replace(result.steps);
    m_progress = 100;
    Q_EMIT progressChanged(m_progress);
    Q_EMIT resultChanged();
    if (result.status == QStringLiteral("planned"))
    {
        m_operatorSummary = QStringLiteral("Action List plan is ready for confirmation.");
        setState(State::Planned);
    }
    else
    {
        m_operatorSummary = QStringLiteral("Action List planning failed.");
        setState(State::Failed);
    }
    return true;
}

bool ActionListController::acceptExecution(const QString& jobId,
                                           const QString& documentRevision,
                                           const pdf::PDFActionListExecutionResult& result)
{
    if (jobId != m_jobId || documentRevision != m_documentRevision || m_state != State::Running || m_cancelRequested)
    {
        return false;
    }

    m_result = result;
    m_steps.replace(result.steps);
    m_progress = 100;
    Q_EMIT progressChanged(m_progress);
    Q_EMIT resultChanged();
    if (result.status == QStringLiteral("succeeded"))
    {
        m_operatorSummary = QStringLiteral("Action List completed successfully.");
        setState(State::Succeeded);
    }
    else if (result.status == QStringLiteral("cancelled"))
    {
        m_operatorSummary = QStringLiteral("Action List was cancelled.");
        setState(State::Cancelled);
    }
    else
    {
        m_operatorSummary = QStringLiteral("Action List execution failed.");
        setState(State::Failed);
    }
    m_plannedDocumentKey.clear();
    m_plannedDocumentRevision.clear();
    m_plannedRecipeHash.clear();
    m_plannedBindingsHash.clear();
    return true;
}

bool ActionListController::failRun(const QString& jobId, const QString& documentRevision, QString errorMessage)
{
    if (jobId != m_jobId || documentRevision != m_documentRevision)
    {
        return false;
    }
    m_operatorSummary = std::move(errorMessage);
    m_progress = 100;
    Q_EMIT progressChanged(m_progress);
    setState(State::Failed);
    return true;
}

bool ActionListController::cancelRun(const QString& jobId)
{
    if (jobId != m_jobId ||
        (m_state != State::Validating && m_state != State::Planning && m_state != State::Running))
    {
        return false;
    }
    m_cancelRequested = true;
    cancelSchedulerJob();
    setState(State::Cancelled);
    m_operatorSummary = QStringLiteral("Action List was cancelled.");
    return true;
}

void ActionListController::discardPlan()
{
    if (m_state != State::Planned)
    {
        return;
    }
    m_result = pdf::PDFActionListExecutionResult();
    m_steps.clear();
    m_operatorSummary.clear();
    setState(State::Idle);
    Q_EMIT resultChanged();
}

void ActionListController::clear()
{
    m_jobId.clear();
    m_recipeId.clear();
    m_runRecipeHash.clear();
    m_runBindingsHash.clear();
    m_validatedDocumentKey.clear();
    m_validatedDocumentRevision.clear();
    m_validatedRecipeHash.clear();
    m_validatedBindingsHash.clear();
    m_plannedDocumentKey.clear();
    m_plannedDocumentRevision.clear();
    m_plannedRecipeHash.clear();
    m_plannedBindingsHash.clear();
    m_progress = 0;
    m_cancelRequested = false;
    m_operatorSummary.clear();
    m_result = pdf::PDFActionListExecutionResult();
    m_steps.clear();
    setState(State::Idle);
    Q_EMIT progressChanged(0);
    Q_EMIT resultChanged();
}

}   // namespace pdfinteraction
