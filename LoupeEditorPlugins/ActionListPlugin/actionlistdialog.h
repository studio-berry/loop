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

#ifndef ACTIONLISTDIALOG_H
#define ACTIONLISTDIALOG_H

#include "pdfactionlist.h"
#include "pdfdocument.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QHash>

#include <atomic>
#include <memory>

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

namespace pdfplugin
{

class ActionListDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit ActionListDialog(pdf::PDFDocument* document, QWidget* parent);
    ~ActionListDialog() override;

    pdf::PDFDocumentPointer takeCandidate();

private:
    struct WorkerResult
    {
        bool ok = false;
        QString error;
        pdf::PDFActionListExecutionResult result;
        pdf::PDFDocument candidate;
    };

    class OperationControl final : public pdf::PDFOperationControl
    {
    public:
        bool isOperationCancelled() const override
        {
            return m_cancelled.load(std::memory_order_acquire);
        }

        void cancel()
        {
            m_cancelled.store(true, std::memory_order_release);
        }

    private:
        std::atomic_bool m_cancelled{false};
    };

    enum class RunMode
    {
        None,
        Plan,
        Execute
    };

    void createUi();
    void addDefaultStep();
    void refreshStepList();
    void refreshOperationForm();
    void syncCurrentStep();
    void syncStepToForm(int row);
    void syncHeaderToRecipe();
    void importRecipe();
    void exportRecipe();
    void validateRecipe();
    void planRecipe();
    void executeRecipe();
    void cancelRun();
    void startWorker(RunMode mode);
    void onWorkerFinished();
    void showResult(const pdf::PDFActionListExecutionResult& result);
    void showValidationErrors(const QStringList& errors);
    void setBusy(bool busy);
    void addParameterEditor(const QString& name, const QJsonObject& schema,
                            const QJsonValue& value);
    void setRecipe(const pdf::PDFActionList& actionList);

    pdf::PDFDocument* m_document = nullptr;
    pdf::PDFActionList m_actionList;
    pdf::PDFActionListExecutionResult m_planResult;
    pdf::PDFDocumentPointer m_candidate;
    std::shared_ptr<OperationControl> m_operationControl;
    QFutureWatcher<WorkerResult>* m_watcher = nullptr;
    RunMode m_runMode = RunMode::None;
    int m_currentStepRow = -1;
    QHash<QString, QWidget*> m_parameterEditors;
    bool m_updatingForm = false;

    QLineEdit* m_idEdit = nullptr;
    QLineEdit* m_nameEdit = nullptr;
    QComboBox* m_failurePolicyCombo = nullptr;
    QListWidget* m_stepsList = nullptr;
    QComboBox* m_operationCombo = nullptr;
    QVBoxLayout* m_parameterLayout = nullptr;
    QTableWidget* m_resultTable = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_planButton = nullptr;
    QPushButton* m_executeButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
};

} // namespace pdfplugin

#endif // ACTIONLISTDIALOG_H
