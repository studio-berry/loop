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

#ifndef PREFLIGHTOVERLAYBRIDGE_H
#define PREFLIGHTOVERLAYBRIDGE_H

#include "interactioncontroller.h"
#include "overlaybuilder.h"
#include "preflightfindingsmodel.h"

#include <QObject>

namespace pdfinteraction
{

/// Feeds preflight findings into OverlayBuilder and refreshes InteractionController.
///
/// One overlay provider stack: markers enter through OverlayBuilder::setFindings,
/// not a parallel Widgets draw path.
class PreflightOverlayBridge final : public QObject
{
    Q_OBJECT

public:
    explicit PreflightOverlayBridge(QObject* parent = nullptr);

    void setFindingsModel(PreflightFindingsModel* model);
    void setOverlayBuilder(OverlayBuilder* overlays);
    void setInteractionController(InteractionController* interaction);

    void applyFindings();

private:
    void onFindingsReplaced();
    void onSelectedFindingIdChanged(const QString& findingId);

    PreflightFindingsModel* m_findings = nullptr;
    OverlayBuilder* m_overlays = nullptr;
    InteractionController* m_interaction = nullptr;
};

}   // namespace pdfinteraction

#endif   // PREFLIGHTOVERLAYBRIDGE_H
