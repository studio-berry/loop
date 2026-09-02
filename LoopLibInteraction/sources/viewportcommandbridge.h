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

#ifndef VIEWPORTCOMMANDBRIDGE_H
#define VIEWPORTCOMMANDBRIDGE_H

#include "commandcatalog.h"
#include "documentfacade.h"
#include "pagesurfacecoordinator.h"
#include "viewportcontroller.h"

#include <QObject>
#include <QPointer>

namespace pdfinteraction
{

/// Catalog handlers for page/zoom/rotate presentation commands.
///
/// These commands mutate ViewportController only. They do not write PDF bytes,
/// and they do not invent a second command registry: the IDs are the Editor
/// action IDs already recorded in docs/loop-shell-actions.json. Enabled only
/// while DocumentFacade is Ready, so a closed or opening document cannot be
/// navigated.
class ViewportCommandBridge final : public QObject
{
    Q_OBJECT

public:
    static const CommandId GoToNextPageCommandId;
    static const CommandId GoToPreviousPageCommandId;
    static const CommandId GoToDocumentStartCommandId;
    static const CommandId GoToDocumentEndCommandId;
    static const CommandId ZoomInCommandId;
    static const CommandId ZoomOutCommandId;
    static const CommandId FitPageCommandId;
    static const CommandId FitWidthCommandId;
    static const CommandId FitHeightCommandId;
    static const CommandId RotateLeftCommandId;
    static const CommandId RotateRightCommandId;

    ViewportCommandBridge(CommandCatalog& catalog,
                          DocumentFacade& facade,
                          ViewportController& viewport,
                          QObject* parent = nullptr);
    ~ViewportCommandBridge() override;

    ViewportCommandBridge(const ViewportCommandBridge&) = delete;
    ViewportCommandBridge& operator=(const ViewportCommandBridge&) = delete;

    /// Observed, not owned. Used to drop admitted surfaces when the facade
    /// replaces or closes the document. May be nullptr in unit tests that only
    /// exercise viewport commands.
    void setCoordinator(PageSurfaceCoordinator* coordinator);
    PageSurfaceCoordinator* coordinator() const noexcept { return m_coordinator; }

    bool handlersRegistered() const noexcept { return m_handlersRegistered; }

    void goToPage(int pageIndex);
    void zoomByStep(qreal factor);
    void applyZoomHint(ZoomHint hint);
    void rotateBy(int quarterTurns);

private:
    bool registerHandlers();
    void clearHandlers();
    void finish(CommandInvocationId invocation);
    void refreshAvailability();
    void onDocumentReplaced(quint64 generation);
    void onDocumentClosed(quint64 generation);
    void invalidateSurfaces();

    int resolvedPageIndex() const;
    int lastPageIndex() const;
    int blockIndexForPage(int pageIndex) const;

    CommandCatalog* m_catalog = nullptr;
    DocumentFacade* m_facade = nullptr;
    ViewportController* m_viewport = nullptr;
    QPointer<PageSurfaceCoordinator> m_coordinator;

    bool m_handlersRegistered = false;
};

}   // namespace pdfinteraction

#endif   // VIEWPORTCOMMANDBRIDGE_H
