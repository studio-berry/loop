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

#include "actionliststepsmodel.h"

#include <QJsonDocument>

namespace pdfinteraction
{

ActionListStepsModel::ActionListStepsModel(QObject* parent) :
    QAbstractListModel(parent)
{
}

int ActionListStepsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_steps.size();
}

QVariant ActionListStepsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_steps.size())
    {
        return {};
    }

    const pdf::PDFActionListStepResult& step = m_steps.at(index.row());
    switch (role)
    {
        case StepIdRole:
            return step.stepId;
        case OperationIdRole:
            return step.operationId;
        case StatusRole:
            return static_cast<int>(step.status);
        case StatusNameRole:
            return pdf::pdfActionListStepStatusName(step.status);
        case DurationMsRole:
            return step.durationMs;
        case ResolvedParametersRole:
            return QString::fromUtf8(QJsonDocument(step.resolvedParameters).toJson(QJsonDocument::Compact));
        case AffectedScopeRole:
            return step.affectedScope;
        case DiagnosticsRole:
            return step.diagnostics;
        case FindingDeltaRole:
            return step.repairResult.value(QStringLiteral("finding_delta")).toObject().toVariantMap();
        case Qt::DisplayRole:
            return QStringLiteral("%1 — %2").arg(step.stepId, pdf::pdfActionListStepStatusName(step.status));
        default:
            return {};
    }
}

QHash<int, QByteArray> ActionListStepsModel::roleNames() const
{
    return {
        { StepIdRole, "stepId" },
        { OperationIdRole, "operationId" },
        { StatusRole, "status" },
        { StatusNameRole, "statusName" },
        { DurationMsRole, "durationMs" },
        { ResolvedParametersRole, "resolvedParameters" },
        { AffectedScopeRole, "affectedScope" },
        { DiagnosticsRole, "diagnostics" },
        { FindingDeltaRole, "findingDelta" }
    };
}

void ActionListStepsModel::replace(const QVector<pdf::PDFActionListStepResult>& steps)
{
    beginResetModel();
    m_steps = steps;
    endResetModel();
}

void ActionListStepsModel::clear()
{
    if (m_steps.isEmpty())
    {
        return;
    }
    beginResetModel();
    m_steps.clear();
    endResetModel();
}

}   // namespace pdfinteraction
