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

#include "preflightreportdockwidget.h"

#include "pdfuitheme.h"
#include "pdfwidgetutils.h"
#include "preflightsidecarutils.h"

#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>

namespace pdfplugin
{

PreflightReportDockWidget::PreflightReportDockWidget(QWidget* parent) :
    QDockWidget(tr("Loupe Preflight Report"), parent)
{
    QWidget* container = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(container);
    layout->setContentsMargins(pdf::PDFUITheme::kDialogMarginPx,
                                 pdf::PDFUITheme::kDialogMarginPx,
                                 pdf::PDFUITheme::kDialogMarginPx,
                                 pdf::PDFUITheme::kDialogMarginPx);
    layout->setSpacing(pdf::PDFUITheme::kDialogMarginPx);

    m_headerLabel = new QLabel(tr("No preflight report loaded."), container);
    m_headerLabel->setObjectName(QStringLiteral("preflightReportHeader"));
    m_headerLabel->setAccessibleName(tr("Preflight report status"));
    m_headerLabel->setWordWrap(true);
    layout->addWidget(m_headerLabel);

    m_summaryLabel = new QLabel(container);
    m_summaryLabel->setObjectName(QStringLiteral("preflightReportSummary"));
    m_summaryLabel->setAccessibleName(tr("Preflight finding summary"));
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    m_contentStack = new QStackedWidget(container);

    m_emptyLabel = new QLabel(tr("Run preflight or load an example report to see findings here."), container);
    m_emptyLabel->setAccessibleName(tr("Preflight report instructions"));
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    {
        QPalette emptyPalette = m_emptyLabel->palette();
        emptyPalette.setColor(QPalette::WindowText, emptyPalette.color(QPalette::PlaceholderText));
        m_emptyLabel->setPalette(emptyPalette);
    }
    m_contentStack->addWidget(m_emptyLabel);

    QWidget* reportPage = new QWidget(container);
    QVBoxLayout* reportLayout = new QVBoxLayout(reportPage);
    reportLayout->setContentsMargins(0, 0, 0, 0);

    m_findingsView = new QTableView(reportPage);
    m_findingsView->setObjectName(QStringLiteral("preflightFindingsView"));
    m_findingsView->setAccessibleName(tr("Preflight findings"));
    m_findingsView->setAccessibleDescription(tr("Select a finding to inspect its evidence and available fixups."));
    m_findingsView->setModel(&m_model);
    m_findingsView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_findingsView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_findingsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_findingsView->setAlternatingRowColors(true);
    m_findingsView->setShowGrid(false);
    m_findingsView->horizontalHeader()->setStretchLastSection(true);
    m_findingsView->verticalHeader()->setVisible(false);
    reportLayout->addWidget(m_findingsView, 1);

    connect(m_findingsView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex& previous)
    {
        Q_UNUSED(previous);
        Q_EMIT findingSelectionChanged(current.isValid() ? current.row() : -1);
    });

    m_fixupsList = new QListWidget(reportPage);
    m_fixupsList->setObjectName(QStringLiteral("preflightFixupsList"));
    m_fixupsList->setAccessibleName(tr("Available fixups"));
    m_fixupsList->setAccessibleDescription(tr("Safe and bounded operations available for the current report."));
    m_fixupsList->setMaximumHeight(120);
    reportLayout->addWidget(m_fixupsList);

    m_applyFixupButton = new QPushButton(tr("Apply Fixup..."), reportPage);
    m_applyFixupButton->setObjectName(QStringLiteral("applyPreflightFixupButton"));
    m_applyFixupButton->setAccessibleName(tr("Apply selected preflight fixup"));
    m_applyFixupButton->setAccessibleDescription(tr("Apply the selected bounded fixup after reviewing its scope."));
    m_applyFixupButton->setEnabled(false);
    connect(m_applyFixupButton, &QPushButton::clicked, this, [this]
    {
        QListWidgetItem* item = m_fixupsList ? m_fixupsList->currentItem() : nullptr;
        if (!item && m_fixupsList && m_fixupsList->count() > 0)
        {
            item = m_fixupsList->item(0);
        }
        if (item)
        {
            const QString id = item->data(Qt::UserRole).toString();
            if (!id.isEmpty())
            {
                Q_EMIT applyFixupRequested(id);
            }
        }
    });
    reportLayout->addWidget(m_applyFixupButton);

    m_contentStack->addWidget(reportPage);
    layout->addWidget(m_contentStack, 1);

    setWidget(container);
    pdf::PDFWidgetUtils::style(container);
    refreshEmptyState();
}

void PreflightReportDockWidget::setReport(const QJsonObject& report, const QString& sourceLabel)
{
    clearFindingSelection();
    m_reportSourceLabel = sourceLabel;
    m_model.setReport(report);
    refreshHeader();
    refreshFixups();
    refreshApplyFixupButton();
    m_contentStack->setCurrentIndex(1);
}

void PreflightReportDockWidget::clearReport()
{
    clearFindingSelection();
    m_reportSourceLabel.clear();
    m_model.clear();
    refreshHeader();
    refreshFixups();
    refreshApplyFixupButton();
    refreshEmptyState();
}

void PreflightReportDockWidget::clearFindingSelection()
{
    if (m_findingsView && m_findingsView->selectionModel())
    {
        m_findingsView->selectionModel()->clearSelection();
    }
    Q_EMIT findingSelectionChanged(-1);
}

void PreflightReportDockWidget::refreshHeader()
{
    if (!m_model.hasReport())
    {
        m_headerLabel->setText(tr("No preflight report loaded."));
        m_summaryLabel->clear();
        return;
    }

    const QString statusText = m_model.pass() ? tr("Pass") : tr("Fail");
    const QColor statusColor = m_model.pass()
                                   ? (pdf::PDFWidgetUtils::isDarkTheme() ? QColor(134, 239, 172) : QColor(0, 102, 51))
                                   : pdf::PDFUITheme::severityTextColor(QStringLiteral("error"));
    if (m_reportSourceLabel.isEmpty())
    {
        m_headerLabel->setText(tr("%1 — profile: %2").arg(statusText, m_model.profileName()));
    }
    else
    {
        m_headerLabel->setText(tr("%1 — profile: %2 — %3").arg(statusText, m_model.profileName(), m_reportSourceLabel));
    }
    {
        QPalette headerPalette = m_headerLabel->palette();
        headerPalette.setColor(QPalette::WindowText, statusColor);
        m_headerLabel->setPalette(headerPalette);
    }
    QString summaryText = tr("%1 error(s), %2 warning(s).")
                              .arg(m_model.errorCount())
                              .arg(m_model.warningCount());

    summaryText += QStringLiteral(" ");
    summaryText += pdfplugin::preflight::overprintDisclosureText(m_model.hasWhiteOverprintFinding());

    m_summaryLabel->setText(summaryText);
    {
        QPalette summaryPalette = m_summaryLabel->palette();
        summaryPalette.setColor(QPalette::WindowText, pdf::PDFUITheme::mutedTextColor(summaryPalette));
        m_summaryLabel->setPalette(summaryPalette);
    }
}

void PreflightReportDockWidget::refreshFixups()
{
    m_fixupsList->clear();

    if (!m_model.hasReport())
    {
        return;
    }

    for (const PreflightFixupEntry& fixup : m_model.fixups())
    {
        const QString label = fixup.safe
                                  ? tr("%1 — %2 (safe)").arg(fixup.id, fixup.description)
                                  : tr("%1 — %2").arg(fixup.id, fixup.description);
        QListWidgetItem* item = new QListWidgetItem(label, m_fixupsList);
        item->setData(Qt::UserRole, fixup.id);
    }

    if (m_fixupsList->count() == 0)
    {
        m_fixupsList->addItem(tr("No fixups available."));
    }
}

void PreflightReportDockWidget::refreshApplyFixupButton()
{
    if (!m_applyFixupButton)
    {
        return;
    }

    m_applyFixupButton->setEnabled(m_model.hasReport() && !m_model.fixups().isEmpty());
}

void PreflightReportDockWidget::refreshEmptyState()
{
    m_contentStack->setCurrentIndex(0);
}

}   // namespace pdfplugin
