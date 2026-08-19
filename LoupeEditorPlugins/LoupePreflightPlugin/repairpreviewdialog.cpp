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

#include "repairpreviewdialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pdfplugin
{

RepairPreviewDialog::RepairPreviewDialog(QWidget* parent) :
    QDialog(parent)
{
    setWindowTitle(tr("Repair Preview"));
    resize(900, 700);

    auto* layout = new QVBoxLayout(this);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    m_previewTabs = new QTabWidget(this);
    layout->addWidget(m_previewTabs, 1);

    m_structuralChanges = new QTreeWidget(this);
    m_structuralChanges->setHeaderLabels({ tr("Path"), tr("Kind"), tr("Classification"), tr("Before"), tr("After") });
    m_structuralChanges->setRootIsDecorated(false);
    layout->addWidget(m_structuralChanges, 1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply Repair"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);
}

void RepairPreviewDialog::setPreviewImage(QLabel* label, const QString& path)
{
    const QPixmap pixmap(path);
    if (pixmap.isNull())
    {
        label->setText(tr("Preview artifact is unavailable."));
        return;
    }
    label->setPixmap(pixmap);
    label->setAlignment(Qt::AlignCenter);
    label->setScaledContents(false);
}

void RepairPreviewDialog::setReport(const pdf::PDFRepairDiffReport& report, const QString& artifactDirectory)
{
    m_previewTabs->clear();
    m_structuralChanges->clear();

    int expected = 0;
    int unexpected = 0;
    for (const pdf::PDFRepairStructuralChange& change : report.structuralChanges)
    {
        expected += change.classification == pdf::PDFRepairChangeClass::Expected;
        unexpected += change.classification == pdf::PDFRepairChangeClass::Unexpected;
        auto* item = new QTreeWidgetItem(m_structuralChanges);
        item->setText(0, change.path);
        item->setText(1, change.kind);
        item->setText(2, pdf::pdfRepairChangeClassName(change.classification));
        item->setText(3, change.beforeValue);
        item->setText(4, change.afterValue);
    }

    int changedPages = 0;
    for (const pdf::PDFRepairPageVisualDiff& page : report.pages)
    {
        changedPages += page.changedPixelCount > 0;
        auto addPreview = [this, &artifactDirectory](const QString& title, const QString& artifact)
        {
            auto* label = new QLabel;
            label->setMinimumSize(320, 240);
            label->setWordWrap(true);
            setPreviewImage(label, artifact.isEmpty() ? QString() : QDir(artifactDirectory).filePath(artifact));
            m_previewTabs->addTab(label, title);
        };
        addPreview(tr("Before — page %1").arg(page.pageIndex + 1), page.beforeImagePath);
        addPreview(tr("After — page %1").arg(page.pageIndex + 1), page.afterImagePath);
        addPreview(tr("Difference — page %1").arg(page.pageIndex + 1), page.diffImagePath);
    }

    const QString summary = tr("Pages compared: %1 | Visually changed: %2 | Expected structural changes: %3 | Unexpected: %4")
        .arg(report.pages.size()).arg(changedPages).arg(expected).arg(unexpected);
    m_summaryLabel->setText(summary);

    const QString status = tr("Comparison status: %1").arg(pdf::pdfRepairDiffStatusName(report.status));
    QStringList limitations = report.incompleteReasons;
    limitations.append(report.warnings);
    m_statusLabel->setText(limitations.isEmpty() ? status : status + tr("\nLimitations: %1").arg(limitations.join(QStringLiteral("; "))));

    const bool canApply = report.status != pdf::PDFRepairDiffStatus::Incomplete &&
                          report.status != pdf::PDFRepairDiffStatus::Failed &&
                          unexpected == 0;
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(canApply);
    m_buttons->button(QDialogButtonBox::Ok)->setToolTip(canApply
                                                        ? tr("Apply the reviewed candidate.")
                                                        : tr("Approval is disabled until comparison limitations or unexpected changes are resolved."));
}

} // namespace pdfplugin
