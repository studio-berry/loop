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

#include "actionlistdialog.h"

#include "pdfrepairoperation.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QAbstractItemView>
#include <QColor>
#include <QCryptographicHash>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>

#include <utility>

namespace pdfplugin
{

namespace
{

QJsonValue editorValue(QWidget* editor)
{
    if (auto* combo = qobject_cast<QComboBox*>(editor))
    {
        return combo->currentData().isValid() ? QJsonValue::fromVariant(combo->currentData())
                                              : QJsonValue(combo->currentText());
    }
    if (auto* checkBox = qobject_cast<QCheckBox*>(editor))
    {
        return checkBox->isChecked();
    }
    if (auto* doubleSpin = qobject_cast<QDoubleSpinBox*>(editor))
    {
        return doubleSpin->value();
    }
    if (auto* spin = qobject_cast<QSpinBox*>(editor))
    {
        return spin->value();
    }
    if (auto* lineEdit = qobject_cast<QLineEdit*>(editor))
    {
        return lineEdit->text();
    }
    return {};
}

QString diagnosticText(const QJsonArray& diagnostics)
{
    QStringList messages;
    for (const QJsonValue& value : diagnostics)
    {
        const QJsonObject diagnostic = value.toObject();
        messages.append(diagnostic.value(QStringLiteral("message")).toString(
            diagnostic.value(QStringLiteral("code")).toString()));
    }
    return messages.join(QStringLiteral("\n"));
}

QString recipeHash(const pdf::PDFActionList& actionList)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(actionList.toJson()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

QColor statusColor(pdf::PDFActionListStepStatus status)
{
    switch (status)
    {
        case pdf::PDFActionListStepStatus::Pending: return QColor(80, 110, 150);
        case pdf::PDFActionListStepStatus::Running: return QColor(40, 100, 190);
        case pdf::PDFActionListStepStatus::Succeeded: return QColor(30, 125, 65);
        case pdf::PDFActionListStepStatus::Skipped: return QColor(170, 105, 15);
        case pdf::PDFActionListStepStatus::Failed: return QColor(175, 35, 35);
        case pdf::PDFActionListStepStatus::Cancelled: return QColor(125, 55, 145);
    }
    return {};
}

} // namespace

ActionListDialog::ActionListDialog(pdf::PDFDocument* document, QWidget* parent) :
    QDialog(parent),
    m_document(document)
{
    setWindowTitle(tr("Action Lists"));
    resize(1050, 700);
    m_actionList.id = QStringLiteral("action-list");
    m_actionList.name = tr("New Action List");
    addDefaultStep();
    createUi();
    refreshStepList();
}

ActionListDialog::~ActionListDialog()
{
    cancelRun();
    if (m_watcher && !m_watcher->isFinished())
    {
        m_watcher->waitForFinished();
    }
}

pdf::PDFDocumentPointer ActionListDialog::takeCandidate()
{
    return std::move(m_candidate);
}

void ActionListDialog::createUi()
{
    auto* outerLayout = new QVBoxLayout(this);

    auto* fileButtons = new QHBoxLayout();
    auto* importButton = new QPushButton(tr("Import..."), this);
    auto* exportButton = new QPushButton(tr("Export..."), this);
    auto* addButton = new QPushButton(tr("Add step"), this);
    auto* removeButton = new QPushButton(tr("Remove step"), this);
    auto* upButton = new QPushButton(tr("Move up"), this);
    auto* downButton = new QPushButton(tr("Move down"), this);
    fileButtons->addWidget(importButton);
    fileButtons->addWidget(exportButton);
    fileButtons->addSpacing(12);
    fileButtons->addWidget(addButton);
    fileButtons->addWidget(removeButton);
    fileButtons->addWidget(upButton);
    fileButtons->addWidget(downButton);
    fileButtons->addStretch(1);
    outerLayout->addLayout(fileButtons);

    auto* headerForm = new QFormLayout();
    m_idEdit = new QLineEdit(this);
    m_nameEdit = new QLineEdit(this);
    m_failurePolicyCombo = new QComboBox(this);
    m_failurePolicyCombo->addItem(tr("Stop on failure"), QStringLiteral("stop"));
    m_failurePolicyCombo->addItem(tr("Continue after failure"), QStringLiteral("continue"));
    headerForm->addRow(tr("Recipe id"), m_idEdit);
    headerForm->addRow(tr("Recipe name"), m_nameEdit);
    headerForm->addRow(tr("Failure policy"), m_failurePolicyCombo);
    outerLayout->addLayout(headerForm);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* stepsPanel = new QWidget(splitter);
    auto* stepsLayout = new QVBoxLayout(stepsPanel);
    stepsLayout->setContentsMargins(0, 0, 0, 0);
    stepsLayout->addWidget(new QLabel(tr("Steps"), stepsPanel));
    m_stepsList = new QListWidget(stepsPanel);
    stepsLayout->addWidget(m_stepsList);

    auto* editorPanel = new QWidget(splitter);
    auto* editorLayout = new QVBoxLayout(editorPanel);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    auto* operationForm = new QFormLayout();
    m_operationCombo = new QComboBox(editorPanel);
    operationForm->addRow(tr("Operation"), m_operationCombo);
    editorLayout->addLayout(operationForm);
    auto* scroll = new QScrollArea(editorPanel);
    scroll->setWidgetResizable(true);
    auto* parameterWidget = new QWidget(scroll);
    m_parameterLayout = new QVBoxLayout(parameterWidget);
    m_parameterLayout->setAlignment(Qt::AlignTop);
    scroll->setWidget(parameterWidget);
    editorLayout->addWidget(scroll, 1);

    splitter->addWidget(stepsPanel);
    splitter->addWidget(editorPanel);
    splitter->setStretchFactor(1, 1);
    outerLayout->addWidget(splitter, 2);

    outerLayout->addWidget(new QLabel(tr("Plan and execution diagnostics"), this));
    m_resultTable = new QTableWidget(this);
    m_resultTable->setColumnCount(6);
    m_resultTable->setHorizontalHeaderLabels({tr("Step"), tr("Operation"), tr("Status"),
                                               tr("Duration"), tr("Affected scope"), tr("Diagnostics")});
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->horizontalHeader()->setStretchLastSection(true);
    outerLayout->addWidget(m_resultTable, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    outerLayout->addWidget(m_statusLabel);

    auto* buttons = new QHBoxLayout();
    m_planButton = new QPushButton(tr("Validate && Plan"), this);
    m_executeButton = new QPushButton(tr("Commit planned run"), this);
    m_cancelButton = new QPushButton(tr("Cancel run"), this);
    auto* closeButton = new QPushButton(tr("Close"), this);
    m_executeButton->setEnabled(false);
    m_cancelButton->setEnabled(false);
    buttons->addWidget(m_planButton);
    buttons->addWidget(m_executeButton);
    buttons->addWidget(m_cancelButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);
    outerLayout->addLayout(buttons);

    connect(importButton, &QPushButton::clicked, this, &ActionListDialog::importRecipe);
    connect(exportButton, &QPushButton::clicked, this, &ActionListDialog::exportRecipe);
    connect(addButton, &QPushButton::clicked, this, [this]
    {
        syncCurrentStep();
        addDefaultStep();
        refreshStepList();
        m_stepsList->setCurrentRow(m_actionList.steps.size() - 1);
    });
    connect(removeButton, &QPushButton::clicked, this, [this]
    {
        if (m_actionList.steps.size() <= 1)
        {
            return;
        }
        syncCurrentStep();
        const int row = m_stepsList->currentRow();
        if (row >= 0 && row < m_actionList.steps.size())
        {
            m_actionList.steps.removeAt(row);
            refreshStepList();
            m_stepsList->setCurrentRow(qMin(row, m_actionList.steps.size() - 1));
        }
    });
    connect(upButton, &QPushButton::clicked, this, [this]
    {
        syncCurrentStep();
        const int row = m_stepsList->currentRow();
        if (row > 0)
        {
            m_actionList.steps.swapItemsAt(row, row - 1);
            refreshStepList();
            m_stepsList->setCurrentRow(row - 1);
        }
    });
    connect(downButton, &QPushButton::clicked, this, [this]
    {
        syncCurrentStep();
        const int row = m_stepsList->currentRow();
        if (row >= 0 && row + 1 < m_actionList.steps.size())
        {
            m_actionList.steps.swapItemsAt(row, row + 1);
            refreshStepList();
            m_stepsList->setCurrentRow(row + 1);
        }
    });
    connect(m_stepsList, &QListWidget::currentRowChanged, this, [this](int)
    {
        if (!m_updatingForm)
        {
            syncStepToForm(m_currentStepRow);
            refreshOperationForm();
        }
    });
    connect(m_operationCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int)
    {
        if (!m_updatingForm && m_stepsList->currentRow() >= 0)
        {
            syncCurrentStep();
            m_actionList.steps[m_stepsList->currentRow()].operationId = m_operationCombo->currentData().toString();
            refreshStepList();
            refreshOperationForm();
        }
    });
    connect(m_planButton, &QPushButton::clicked, this, &ActionListDialog::planRecipe);
    connect(m_executeButton, &QPushButton::clicked, this, &ActionListDialog::executeRecipe);
    connect(m_cancelButton, &QPushButton::clicked, this, &ActionListDialog::cancelRun);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    m_idEdit->setText(m_actionList.id);
    m_nameEdit->setText(m_actionList.name);
    refreshOperationForm();
}

void ActionListDialog::addDefaultStep()
{
    const QStringList operationIds = pdf::PDFRepairRegistry::instance().operationIds();
    if (operationIds.isEmpty())
    {
        return;
    }
    pdf::PDFActionListStep step;
    step.id = QStringLiteral("step-%1").arg(m_actionList.steps.size() + 1);
    step.operationId = operationIds.front();
    m_actionList.steps.append(std::move(step));
}

void ActionListDialog::refreshStepList()
{
    m_updatingForm = true;
    const int currentRow = m_stepsList ? m_stepsList->currentRow() : 0;
    m_stepsList->clear();
    for (const pdf::PDFActionListStep& step : m_actionList.steps)
    {
        m_stepsList->addItem(QStringLiteral("%1 — %2").arg(step.id, step.operationId));
    }
    if (!m_actionList.steps.isEmpty())
    {
        m_stepsList->setCurrentRow(qBound(0, currentRow, m_actionList.steps.size() - 1));
    }
    m_updatingForm = false;
    refreshOperationForm();
}

void ActionListDialog::refreshOperationForm()
{
    if (!m_stepsList || m_stepsList->currentRow() < 0 || m_stepsList->currentRow() >= m_actionList.steps.size())
    {
        return;
    }

    m_updatingForm = true;
    const int row = m_stepsList->currentRow();
    m_currentStepRow = row;
    const pdf::PDFActionListStep& step = m_actionList.steps.at(row);
    m_operationCombo->clear();
    const QStringList operationIds = pdf::PDFRepairRegistry::instance().operationIds();
    for (const QString& operationId : operationIds)
    {
        m_operationCombo->addItem(operationId, operationId);
    }
    m_operationCombo->setCurrentIndex(qMax(0, m_operationCombo->findData(step.operationId)));

    while (QLayoutItem* item = m_parameterLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    m_parameterEditors.clear();
    const pdf::PDFRepairOperation* operation = pdf::PDFRepairRegistry::instance().find(step.operationId);
    if (operation)
    {
        const QJsonObject properties = operation->parameterSchema().value(QStringLiteral("properties")).toObject();
        for (auto it = properties.begin(); it != properties.end(); ++it)
        {
            addParameterEditor(it.key(), it.value().toObject(), step.parameters.value(it.key()));
        }
    }
    auto* hint = new QLabel(tr("Parameter editors are generated from the operation's parameterSchema()."), this);
    hint->setWordWrap(true);
    m_parameterLayout->addWidget(hint);
    m_updatingForm = false;
}

void ActionListDialog::addParameterEditor(const QString& name,
                                          const QJsonObject& schema, const QJsonValue& value)
{
    QWidget* editor = nullptr;
    const QString type = schema.value(QStringLiteral("type")).toString();
    const QJsonArray enumValues = schema.value(QStringLiteral("enum")).toArray();
    if (!enumValues.isEmpty())
    {
        auto* combo = new QComboBox(this);
        for (const QJsonValue& enumValue : enumValues)
        {
            combo->addItem(enumValue.toString(), enumValue.toString());
        }
        combo->setCurrentIndex(qMax(0, combo->findData(value.toString())));
        editor = combo;
    }
    else if (type == QStringLiteral("boolean"))
    {
        auto* checkBox = new QCheckBox(this);
        checkBox->setChecked(value.toBool(false));
        editor = checkBox;
    }
    else if (type == QStringLiteral("integer"))
    {
        auto* spin = new QSpinBox(this);
        spin->setRange(schema.value(QStringLiteral("minimum")).toInt(-1000000),
                       schema.value(QStringLiteral("maximum")).toInt(1000000));
        spin->setValue(value.toInt(schema.value(QStringLiteral("default")).toInt(0)));
        editor = spin;
    }
    else if (type == QStringLiteral("number"))
    {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(schema.value(QStringLiteral("minimum")).toDouble(-1000000.0),
                       schema.value(QStringLiteral("maximum")).toDouble(1000000.0));
        spin->setDecimals(3);
        spin->setValue(value.toDouble(schema.value(QStringLiteral("default")).toDouble(0.0)));
        editor = spin;
    }
    else
    {
        auto* lineEdit = new QLineEdit(this);
        lineEdit->setText(value.toString());
        editor = lineEdit;
    }
    m_parameterEditors.insert(name, editor);
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(name, this));
    row->addWidget(editor, 1);
    m_parameterLayout->addLayout(row);
}

void ActionListDialog::syncCurrentStep()
{
    if (m_updatingForm || !m_stepsList || m_stepsList->currentRow() < 0 || m_stepsList->currentRow() >= m_actionList.steps.size())
    {
        return;
    }
    syncStepToForm(m_stepsList->currentRow());
}

void ActionListDialog::syncStepToForm(int row)
{
    if (m_updatingForm || row < 0 || row >= m_actionList.steps.size())
    {
        return;
    }
    syncHeaderToRecipe();
    pdf::PDFActionListStep& step = m_actionList.steps[row];
    step.operationId = m_operationCombo->currentData().toString();
    QJsonObject parameters;
    for (auto it = m_parameterEditors.cbegin(); it != m_parameterEditors.cend(); ++it)
    {
        parameters.insert(it.key(), editorValue(it.value()));
    }
    step.parameters = parameters;
}

void ActionListDialog::syncHeaderToRecipe()
{
    if (m_idEdit)
    {
        m_actionList.id = m_idEdit->text().trimmed();
        m_actionList.name = m_nameEdit->text().trimmed();
        m_actionList.failurePolicy = m_failurePolicyCombo->currentData().toString() == QStringLiteral("continue")
            ? pdf::PDFActionListFailurePolicy::Continue : pdf::PDFActionListFailurePolicy::Stop;
    }
}

void ActionListDialog::setRecipe(const pdf::PDFActionList& actionList)
{
    m_actionList = actionList;
    m_idEdit->setText(m_actionList.id);
    m_nameEdit->setText(m_actionList.name);
    m_failurePolicyCombo->setCurrentIndex(m_actionList.failurePolicy == pdf::PDFActionListFailurePolicy::Continue ? 1 : 0);
    refreshStepList();
    m_executeButton->setEnabled(false);
}

void ActionListDialog::importRecipe()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Action List"), QString(), tr("JSON files (*.json);;All files (*.*)"));
    if (path.isEmpty())
    {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("Import Action List"), file.errorString());
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        QMessageBox::warning(this, tr("Import Action List"), parseError.errorString());
        return;
    }
    pdf::PDFActionList actionList;
    if (!pdf::PDFActionList::fromJson(document.object(), &actionList))
    {
        QMessageBox::warning(this, tr("Import Action List"), tr("The file is not a valid loupe-action-list/1 recipe."));
        return;
    }
    setRecipe(actionList);
    validateRecipe();
}

void ActionListDialog::exportRecipe()
{
    syncCurrentStep();
    syncHeaderToRecipe();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Action List"),
                                                      m_actionList.name + QStringLiteral(".json"),
                                                      tr("JSON files (*.json);;All files (*.*)"));
    if (path.isEmpty())
    {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, tr("Export Action List"), file.errorString());
        return;
    }
    file.write(QJsonDocument(m_actionList.toJson()).toJson(QJsonDocument::Indented));
    m_statusLabel->setText(tr("Saved recipe to %1.").arg(path));
}

void ActionListDialog::showValidationErrors(const QStringList& errors)
{
    m_resultTable->setRowCount(0);
    for (const QString& error : errors)
    {
        const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("Step '([^']+)'" )).match(error);
        const int row = m_resultTable->rowCount();
        m_resultTable->insertRow(row);
        m_resultTable->setItem(row, 0, new QTableWidgetItem(match.hasMatch() ? match.captured(1) : tr("Recipe")));
        m_resultTable->setItem(row, 1, new QTableWidgetItem());
        auto* status = new QTableWidgetItem(tr("Failed"));
        status->setForeground(statusColor(pdf::PDFActionListStepStatus::Failed));
        m_resultTable->setItem(row, 2, status);
        m_resultTable->setItem(row, 3, new QTableWidgetItem());
        m_resultTable->setItem(row, 4, new QTableWidgetItem());
        m_resultTable->setItem(row, 5, new QTableWidgetItem(error));
    }
    m_executeButton->setEnabled(false);
    m_statusLabel->setText(tr("Recipe validation failed with %1 diagnostic(s). Each step error is shown separately.").arg(errors.size()));
}

void ActionListDialog::validateRecipe()
{
    syncCurrentStep();
    syncHeaderToRecipe();
    QStringList errors;
    const pdf::PDFOperationResult result = pdf::PDFActionListExecutor().validate(m_actionList, {}, &errors);
    if (!result)
    {
        showValidationErrors(errors);
        return;
    }
    m_statusLabel->setText(tr("Recipe is valid. Plan it before committing any changes."));
    m_resultTable->setRowCount(0);
    m_executeButton->setEnabled(false);
}

void ActionListDialog::planRecipe()
{
    syncCurrentStep();
    syncHeaderToRecipe();
    QStringList errors;
    if (!pdf::PDFActionListExecutor().validate(m_actionList, {}, &errors))
    {
        showValidationErrors(errors);
        return;
    }
    m_planResult = {};
    m_executeButton->setEnabled(false);
    startWorker(RunMode::Plan);
}

void ActionListDialog::executeRecipe()
{
    if (m_planResult.status != QStringLiteral("planned"))
    {
        return;
    }
    if (QMessageBox::question(this, tr("Commit Action List"),
                              tr("The reviewed plan will now run against an isolated candidate. Commit the candidate to the open document when all steps succeed?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        return;
    }
    syncCurrentStep();
    syncHeaderToRecipe();
    if (recipeHash(m_actionList) != m_planResult.recipeHash)
    {
        m_planResult = {};
        m_executeButton->setEnabled(false);
        m_statusLabel->setText(tr("The recipe changed after planning. Validate and plan it again before committing."));
        return;
    }
    startWorker(RunMode::Execute);
}

void ActionListDialog::startWorker(RunMode mode)
{
    if (m_watcher && !m_watcher->isFinished())
    {
        return;
    }
    if (!m_document)
    {
        return;
    }
    m_runMode = mode;
    m_operationControl = std::make_shared<OperationControl>();
    const std::shared_ptr<OperationControl> control = m_operationControl;
    const pdf::PDFActionList actionList = m_actionList;
    const pdf::PDFDocument source = *m_document;
    m_watcher = new QFutureWatcher<WorkerResult>(this);
    connect(m_watcher, &QFutureWatcher<WorkerResult>::finished, this, &ActionListDialog::onWorkerFinished);
    setBusy(true);
    m_watcher->setFuture(QtConcurrent::run([actionList, source, control, mode]() mutable
    {
        WorkerResult workerResult;
        pdf::PDFActionListExecutor executor;
        pdf::PDFActionListExecutionOptions options;
        options.dryRun = mode == RunMode::Plan;
        options.operationControl = control.get();
        if (mode == RunMode::Plan)
        {
            workerResult.result = {};
            const pdf::PDFOperationResult result = executor.plan(actionList, source, options, &workerResult.result);
            workerResult.ok = static_cast<bool>(result);
            workerResult.error = result.getErrorMessage();
        }
        else
        {
            workerResult.result = {};
            const pdf::PDFOperationResult result = executor.execute(actionList, source, options,
                                                                    &workerResult.candidate, &workerResult.result);
            workerResult.ok = static_cast<bool>(result);
            workerResult.error = result.getErrorMessage();
        }
        return workerResult;
    }));
}

void ActionListDialog::cancelRun()
{
    if (m_operationControl)
    {
        m_operationControl->cancel();
    }
}

void ActionListDialog::onWorkerFinished()
{
    const WorkerResult workerResult = m_watcher->result();
    const RunMode mode = m_runMode;
    m_runMode = RunMode::None;
    setBusy(false);
    showResult(workerResult.result);
    if (mode == RunMode::Plan)
    {
        m_planResult = workerResult.result;
        m_executeButton->setEnabled(workerResult.ok && workerResult.result.status == QStringLiteral("planned"));
        if (!workerResult.ok)
        {
            m_statusLabel->setText(tr("Plan failed: %1").arg(workerResult.error));
        }
        return;
    }
    if (!workerResult.ok)
    {
        m_statusLabel->setText(workerResult.result.status == QStringLiteral("cancelled")
            ? tr("Action List execution cancelled; the source document was left untouched.")
            : tr("Action List execution failed: %1").arg(workerResult.error));
        return;
    }

    m_candidate = pdf::PDFDocumentPointer(new pdf::PDFDocument(std::move(workerResult.candidate)));
    m_statusLabel->setText(tr("Action List completed. The isolated candidate is ready to commit."));
    accept();
}

void ActionListDialog::showResult(const pdf::PDFActionListExecutionResult& result)
{
    m_resultTable->setRowCount(0);
    for (const pdf::PDFActionListStepResult& step : result.steps)
    {
        const int row = m_resultTable->rowCount();
        m_resultTable->insertRow(row);
        m_resultTable->setItem(row, 0, new QTableWidgetItem(step.stepId));
        m_resultTable->setItem(row, 1, new QTableWidgetItem(step.operationId));
        auto* status = new QTableWidgetItem(pdf::pdfActionListStepStatusName(step.status));
        status->setForeground(statusColor(step.status));
        m_resultTable->setItem(row, 2, status);
        m_resultTable->setItem(row, 3, new QTableWidgetItem(tr("%1 ms").arg(step.durationMs)));
        m_resultTable->setItem(row, 4, new QTableWidgetItem(QString::fromUtf8(QJsonDocument(step.affectedScope).toJson(QJsonDocument::Compact))));
        m_resultTable->setItem(row, 5, new QTableWidgetItem(diagnosticText(step.diagnostics)));
    }
    if (result.steps.isEmpty() && !result.diagnostics.isEmpty())
    {
        for (const QJsonValue& value : result.diagnostics)
        {
            const QJsonObject diagnostic = value.toObject();
            const int row = m_resultTable->rowCount();
            m_resultTable->insertRow(row);
            m_resultTable->setItem(row, 0, new QTableWidgetItem(tr("Recipe")));
            m_resultTable->setItem(row, 2, new QTableWidgetItem(diagnostic.value(QStringLiteral("severity")).toString()));
            m_resultTable->setItem(row, 5, new QTableWidgetItem(diagnostic.value(QStringLiteral("message")).toString()));
        }
    }
    m_statusLabel->setText(tr("Status: %1; recipe hash: %2; duration: %3 ms")
                           .arg(result.status, result.recipeHash).arg(result.durationMs));
}

void ActionListDialog::setBusy(bool busy)
{
    m_planButton->setEnabled(!busy);
    m_executeButton->setEnabled(!busy && m_planResult.status == QStringLiteral("planned"));
    m_cancelButton->setEnabled(busy);
    m_stepsList->setEnabled(!busy);
    m_operationCombo->setEnabled(!busy);
    m_statusLabel->setText(busy ? tr("Action List is running off the GUI thread...") : m_statusLabel->text());
}

} // namespace pdfplugin
