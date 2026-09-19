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

#ifndef ACTIONLISTSTEPSMODEL_H
#define ACTIONLISTSTEPSMODEL_H

#include "interactionglobal.h"

#include "pdfactionlist.h"

#include <QAbstractListModel>
#include <QJsonArray>
#include <QVector>

namespace pdfinteraction
{

class ActionListStepsModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        StepIdRole = Qt::UserRole + 1,
        OperationIdRole,
        StatusRole,
        StatusNameRole,
        DurationMsRole,
        ResolvedParametersRole,
        AffectedScopeRole,
        DiagnosticsRole,
        FindingDeltaRole
    };
    Q_ENUM(Role)

    explicit ActionListStepsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(const QVector<pdf::PDFActionListStepResult>& steps);
    void clear();

private:
    QVector<pdf::PDFActionListStepResult> m_steps;
};

}   // namespace pdfinteraction

#endif   // ACTIONLISTSTEPSMODEL_H
