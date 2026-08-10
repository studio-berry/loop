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

#include "actionlistplugin.h"
#include "actionlistdialog.h"

#include "pdfdrawwidget.h"

#include <QAction>

namespace pdfplugin
{

ActionListPlugin::ActionListPlugin() :
    pdf::PDFPlugin(nullptr)
{
}

void ActionListPlugin::setWidget(pdf::PDFWidget* widget)
{
    Q_ASSERT(!m_widget);
    BaseClass::setWidget(widget);

    m_action = new QAction(tr("&Action Lists..."), this);
    m_action->setObjectName(QStringLiteral("actionList_Open"));
    m_action->setToolTip(tr("Import, validate, preview, and run reusable Action Lists."));
    connect(m_action, &QAction::triggered, this, &ActionListPlugin::onActionListTriggered);
    updateActions();
}

void ActionListPlugin::setDocument(const pdf::PDFModifiedDocument& document)
{
    BaseClass::setDocument(document);
    updateActions();
}

std::vector<QAction*> ActionListPlugin::getActions() const
{
    return {m_action};
}

QString ActionListPlugin::getPluginMenuName() const
{
    return tr("Action Lists");
}

void ActionListPlugin::onActionListTriggered()
{
    if (!m_document || !m_widget)
    {
        return;
    }

    ActionListDialog dialog(m_document, m_dataExchangeInterface->getMainWindow());
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    pdf::PDFDocumentPointer candidate = dialog.takeCandidate();
    if (!candidate)
    {
        return;
    }

    const auto flags = pdf::PDFModifiedDocument::ModificationFlags(
        pdf::PDFModifiedDocument::Reset | pdf::PDFModifiedDocument::PreserveUndoRedo);
    Q_EMIT m_widget->getToolManager()->documentModified(
        pdf::PDFModifiedDocument(std::move(candidate), nullptr, flags));
}

void ActionListPlugin::updateActions()
{
    if (m_action)
    {
        m_action->setEnabled(m_widget && m_document);
    }
}

} // namespace pdfplugin
