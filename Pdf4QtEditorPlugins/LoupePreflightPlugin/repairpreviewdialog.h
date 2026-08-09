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

#ifndef REPAIRPREVIEWDIALOG_H
#define REPAIRPREVIEWDIALOG_H

#include "pdfrepairdiff.h"

#include <QDialog>

class QLabel;
class QDialogButtonBox;
class QTabWidget;
class QTreeWidget;

namespace pdfplugin
{

/// Reusable operator approval surface for serialized repair-diff reports.
/// Applying is deliberately disabled for incomplete comparisons or unexpected
/// changes; callers remain responsible for the final safe-write transaction.
class RepairPreviewDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RepairPreviewDialog(QWidget* parent = nullptr);

    void setReport(const pdf::PDFRepairDiffReport& report, const QString& artifactDirectory = QString());

private:
    void setPreviewImage(QLabel* label, const QString& path);

    QLabel* m_summaryLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTabWidget* m_previewTabs = nullptr;
    QTreeWidget* m_structuralChanges = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

} // namespace pdfplugin

#endif // REPAIRPREVIEWDIALOG_H
