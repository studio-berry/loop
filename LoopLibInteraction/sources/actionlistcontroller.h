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

#ifndef ACTIONLISTCONTROLLER_H
#define ACTIONLISTCONTROLLER_H

#include "actionliststepsmodel.h"

#include "pdfactionlist.h"
#include "pdfjobscheduler.h"

#include <QObject>
#include <QStringList>

namespace pdfinteraction
{

class ActionListController final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(ActionListStepsModel* stepsModel READ stepsModel CONSTANT)
    Q_PROPERTY(QString operatorSummary READ operatorSummary NOTIFY stateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString recipeHash READ recipeHash NOTIFY resultChanged)
    Q_PROPERTY(QString planDigest READ planDigest NOTIFY resultChanged)
    Q_PROPERTY(QString governedStatus READ governedStatus NOTIFY resultChanged)
    Q_PROPERTY(QString resultStatus READ resultStatus NOTIFY resultChanged)
    Q_PROPERTY(bool validationReady READ validationReady NOTIFY resultChanged)

public:
    enum class State
    {
        Idle,
        Validating,
        Planning,
        Running,
        Planned,
        Succeeded,
        Failed,
        Cancelled
    };
    Q_ENUM(State)

    explicit ActionListController(pdf::PDFJobScheduler* scheduler = nullptr, QObject* parent = nullptr);

    ActionListStepsModel* stepsModel() { return &m_steps; }
    const ActionListStepsModel* stepsModel() const { return &m_steps; }
    State state() const { return m_state; }
    QString operatorSummary() const { return m_operatorSummary; }
    QString documentKey() const { return m_documentKey; }
    QString documentRevision() const { return m_documentRevision; }
    QString recipeId() const { return m_recipeId; }
    QString jobId() const { return m_jobId; }
    int progress() const noexcept { return m_progress; }
    QString recipeHash() const { return m_result.recipeHash; }
    QString planDigest() const { return m_result.planDigest; }
    QString governedStatus() const
    {
        if (m_result.governed.value(QStringLiteral("sign_off")).toObject().value(QStringLiteral("schema")).toString() == QStringLiteral("loop.governed-sign-off"))
        {
            return QStringLiteral("signed-off");
        }
        if (m_result.status == QStringLiteral("planned"))
        {
            return QStringLiteral("pending");
        }
        return m_result.governed.isEmpty() ? QStringLiteral("not-run") : QStringLiteral("not-certified");
    }
    QString resultStatus() const { return m_result.status; }
    bool validationReady() const noexcept { return !m_validatedRecipeHash.isEmpty() && m_state == State::Idle; }
    const pdf::PDFActionListExecutionResult& result() const noexcept { return m_result; }
    bool hasPlannedResult() const noexcept { return m_state == State::Planned; }

    void setCurrentRevision(QString documentKey, QString documentRevision);
    void markRecipeStale();
    void beginRun(State phase,
                  QString documentKey,
                  QString documentRevision,
                  QString recipeId,
                  QString recipeHash,
                  QString bindingsHash,
                  QString jobId);
    bool validationMatches(const QString& documentKey,
                           const QString& documentRevision,
                           const QString& recipeHash,
                           const QString& bindingsHash) const;
    bool planMatches(const QString& documentKey,
                     const QString& documentRevision,
                     const QString& recipeHash,
                     const QString& bindingsHash) const;
    bool updateProgress(const QString& jobId, const QString& documentRevision, int progress);
    bool acceptValidation(const QString& jobId,
                          const QString& documentRevision,
                          const QStringList& errors,
                          const QVector<pdf::PDFActionListStepResult>& validationSteps);
    bool acceptPlan(const QString& jobId, const QString& documentRevision, const pdf::PDFActionListExecutionResult& result);
    bool acceptExecution(const QString& jobId, const QString& documentRevision, const pdf::PDFActionListExecutionResult& result);
    bool failRun(const QString& jobId, const QString& documentRevision, QString errorMessage);
    bool cancelRun(const QString& jobId);
    void discardPlan();
    void clear();

signals:
    void stateChanged(pdfinteraction::ActionListController::State state);
    void progressChanged(int progress);
    void resultChanged();

private:
    void setState(State state);
    void cancelSchedulerJob();

    ActionListStepsModel m_steps;
    State m_state = State::Idle;
    QString m_operatorSummary;
    QString m_documentKey;
    QString m_documentRevision;
    QString m_recipeId;
    QString m_jobId;
    QString m_runRecipeHash;
    QString m_runBindingsHash;
    QString m_validatedDocumentKey;
    QString m_validatedDocumentRevision;
    QString m_validatedRecipeHash;
    QString m_validatedBindingsHash;
    QString m_plannedDocumentKey;
    QString m_plannedDocumentRevision;
    QString m_plannedRecipeHash;
    QString m_plannedBindingsHash;
    int m_progress = 0;
    bool m_cancelRequested = false;
    pdf::PDFActionListExecutionResult m_result;
    pdf::PDFJobScheduler* m_scheduler = nullptr;
};

}   // namespace pdfinteraction

Q_DECLARE_METATYPE(pdfinteraction::ActionListController::State)

#endif   // ACTIONLISTCONTROLLER_H
