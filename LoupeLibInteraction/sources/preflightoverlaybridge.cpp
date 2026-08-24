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

#include "preflightoverlaybridge.h"

namespace pdfinteraction
{

PreflightOverlayBridge::PreflightOverlayBridge(QObject* parent) :
    QObject(parent)
{
}

void PreflightOverlayBridge::setFindingsModel(PreflightFindingsModel* model)
{
    if (m_findings == model)
    {
        return;
    }

    if (m_findings)
    {
        disconnect(m_findings, nullptr, this, nullptr);
    }

    m_findings = model;

    if (m_findings)
    {
        connect(m_findings, &PreflightFindingsModel::findingsReplaced, this, &PreflightOverlayBridge::onFindingsReplaced);
        connect(m_findings, &PreflightFindingsModel::selectedFindingIdChanged, this, &PreflightOverlayBridge::onSelectedFindingIdChanged);
    }
}

void PreflightOverlayBridge::setOverlayBuilder(OverlayBuilder* overlays)
{
    m_overlays = overlays;
}

void PreflightOverlayBridge::setInteractionController(InteractionController* interaction)
{
    m_interaction = interaction;
}

void PreflightOverlayBridge::applyFindings()
{
    if (!m_findings || !m_overlays)
    {
        return;
    }

    m_overlays->setFindings(m_findings->interactionTargets());
    m_overlays->setSeverities(m_findings->severityMap());
    m_overlays->setFocusedId(m_findings->selectedFindingId());

    if (m_interaction)
    {
        const QString selectedId = m_findings->selectedFindingId();
        if (!selectedId.isEmpty())
        {
            const PreflightFindingView* finding = m_findings->finding(selectedId);
            if (finding && finding->page > 0)
            {
                InteractionTarget target;
                target.kind = InteractionTargetKind::Finding;
                target.pageIndex = finding->page - 1;
                target.id = finding->id;
                target.pageBounds = finding->bbox.normalized();
                m_interaction->selectTarget(target);
                return;
            }
        }

        m_interaction->refreshOverlay();
    }
}

void PreflightOverlayBridge::onFindingsReplaced()
{
    applyFindings();
}

void PreflightOverlayBridge::onSelectedFindingIdChanged(const QString& findingId)
{
    Q_UNUSED(findingId);
    applyFindings();
}

}   // namespace pdfinteraction
