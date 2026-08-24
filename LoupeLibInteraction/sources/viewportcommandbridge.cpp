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

#include "viewportcommandbridge.h"

#include "pagesurfacecoordinator.h"

#include "pdfpage.h"

namespace pdfinteraction
{

const CommandId ViewportCommandBridge::GoToNextPageCommandId = QStringLiteral("actionGoToNextPage");
const CommandId ViewportCommandBridge::GoToPreviousPageCommandId = QStringLiteral("actionGoToPreviousPage");
const CommandId ViewportCommandBridge::GoToDocumentStartCommandId = QStringLiteral("actionGoToDocumentStart");
const CommandId ViewportCommandBridge::GoToDocumentEndCommandId = QStringLiteral("actionGoToDocumentEnd");
const CommandId ViewportCommandBridge::ZoomInCommandId = QStringLiteral("actionZoom_In");
const CommandId ViewportCommandBridge::ZoomOutCommandId = QStringLiteral("actionZoom_Out");
const CommandId ViewportCommandBridge::FitPageCommandId = QStringLiteral("actionFitPage");
const CommandId ViewportCommandBridge::FitWidthCommandId = QStringLiteral("actionFitWidth");
const CommandId ViewportCommandBridge::FitHeightCommandId = QStringLiteral("actionFitHeight");
const CommandId ViewportCommandBridge::RotateLeftCommandId = QStringLiteral("actionRotateLeft");
const CommandId ViewportCommandBridge::RotateRightCommandId = QStringLiteral("actionRotateRight");

namespace
{

const CommandId kHandledCommands[] = {
    ViewportCommandBridge::GoToNextPageCommandId,
    ViewportCommandBridge::GoToPreviousPageCommandId,
    ViewportCommandBridge::GoToDocumentStartCommandId,
    ViewportCommandBridge::GoToDocumentEndCommandId,
    ViewportCommandBridge::ZoomInCommandId,
    ViewportCommandBridge::ZoomOutCommandId,
    ViewportCommandBridge::FitPageCommandId,
    ViewportCommandBridge::FitWidthCommandId,
    ViewportCommandBridge::FitHeightCommandId,
    ViewportCommandBridge::RotateLeftCommandId,
    ViewportCommandBridge::RotateRightCommandId,
};

}   // namespace

ViewportCommandBridge::ViewportCommandBridge(CommandCatalog& catalog,
                                             DocumentFacade& facade,
                                             ViewportController& viewport,
                                             QObject* parent) :
    QObject(parent),
    m_catalog(&catalog),
    m_facade(&facade),
    m_viewport(&viewport)
{
    m_handlersRegistered = registerHandlers();

    connect(m_facade, &DocumentFacade::stateChanged, this, [this](DocumentState)
            { refreshAvailability(); });
    connect(m_facade, &DocumentFacade::documentReplaced, this, &ViewportCommandBridge::onDocumentReplaced);
    connect(m_facade, &DocumentFacade::documentClosed, this, &ViewportCommandBridge::onDocumentClosed);
    connect(m_viewport, &ViewportController::demandChanged, this, [this](quint64)
            { refreshAvailability(); });
    connect(m_viewport, &ViewportController::placementsChanged, this, &ViewportCommandBridge::refreshAvailability);

    refreshAvailability();
}

ViewportCommandBridge::~ViewportCommandBridge()
{
    clearHandlers();
}

void ViewportCommandBridge::setCoordinator(PageSurfaceCoordinator* coordinator)
{
    m_coordinator = coordinator;
}

bool ViewportCommandBridge::registerHandlers()
{
    if (!m_catalog)
    {
        return false;
    }

    auto bindAction = [this](const CommandId& id, const std::function<void()>& action)
    {
        CommandCatalog::Handler handler;
        handler.invoke = [this, action](CommandInvocationId invocation, const QVariantMap&)
        {
            action();
            finish(invocation);
        };
        return m_catalog->setHandler(id, std::move(handler));
    };

    bool registered = true;
    registered = bindAction(GoToNextPageCommandId, [this]()
                            { goToPage(resolvedPageIndex() + 1); }) &&
                 registered;
    registered = bindAction(GoToPreviousPageCommandId, [this]()
                            { goToPage(resolvedPageIndex() - 1); }) &&
                 registered;
    registered = bindAction(GoToDocumentStartCommandId, [this]()
                            { goToPage(0); }) &&
                 registered;
    registered = bindAction(GoToDocumentEndCommandId, [this]()
                            { goToPage(lastPageIndex()); }) &&
                 registered;
    registered = bindAction(ZoomInCommandId, [this]()
                            { zoomByStep(ViewportController::ZoomStep); }) &&
                 registered;
    registered = bindAction(ZoomOutCommandId, [this]()
                            { zoomByStep(1.0 / ViewportController::ZoomStep); }) &&
                 registered;
    registered = bindAction(FitPageCommandId, [this]()
                            { applyZoomHint(ZoomHint::Fit); }) &&
                 registered;
    registered = bindAction(FitWidthCommandId, [this]()
                            { applyZoomHint(ZoomHint::FitWidth); }) &&
                 registered;
    registered = bindAction(FitHeightCommandId, [this]()
                            { applyZoomHint(ZoomHint::FitHeight); }) &&
                 registered;
    registered = bindAction(RotateLeftCommandId, [this]()
                            { rotateBy(-1); }) &&
                 registered;
    registered = bindAction(RotateRightCommandId, [this]()
                            { rotateBy(1); }) &&
                 registered;

    if (!registered)
    {
        clearHandlers();
        return false;
    }

    return true;
}

void ViewportCommandBridge::clearHandlers()
{
    if (!m_catalog)
    {
        return;
    }

    for (const CommandId& id : kHandledCommands)
    {
        m_catalog->clearHandler(id);
        m_catalog->setEnabled(id, false);
    }

    m_handlersRegistered = false;
}

void ViewportCommandBridge::finish(CommandInvocationId invocation)
{
    if (m_catalog)
    {
        m_catalog->finishInvocation(invocation, CommandTerminalState::Completed);
    }
}

void ViewportCommandBridge::refreshAvailability()
{
    if (!m_catalog || !m_handlersRegistered)
    {
        return;
    }

    const bool ready = m_facade && m_facade->state() == DocumentState::Ready;
    const int count = m_viewport ? m_viewport->pageCount() : 0;
    const int page = resolvedPageIndex();
    const bool hasPages = ready && count > 0;
    const qreal zoom = m_viewport ? m_viewport->zoom() : 1.0;

    m_catalog->setEnabled(GoToNextPageCommandId, hasPages && page >= 0 && page < count - 1);
    m_catalog->setEnabled(GoToPreviousPageCommandId, hasPages && page > 0);
    m_catalog->setEnabled(GoToDocumentStartCommandId, hasPages && page > 0);
    m_catalog->setEnabled(GoToDocumentEndCommandId, hasPages && page >= 0 && page < count - 1);
    m_catalog->setEnabled(ZoomInCommandId, hasPages && zoom < ViewportController::MaximumZoom - 1e-9);
    m_catalog->setEnabled(ZoomOutCommandId, hasPages && zoom > ViewportController::MinimumZoom + 1e-9);
    m_catalog->setEnabled(FitPageCommandId, hasPages);
    m_catalog->setEnabled(FitWidthCommandId, hasPages);
    m_catalog->setEnabled(FitHeightCommandId, hasPages);
    m_catalog->setEnabled(RotateLeftCommandId, hasPages);
    m_catalog->setEnabled(RotateRightCommandId, hasPages);
}

void ViewportCommandBridge::onDocumentReplaced(quint64)
{
    invalidateSurfaces();
    refreshAvailability();
}

void ViewportCommandBridge::onDocumentClosed(quint64)
{
    invalidateSurfaces();
    refreshAvailability();
}

void ViewportCommandBridge::invalidateSurfaces()
{
    if (!m_coordinator)
    {
        return;
    }

    const pdf::PDFRevisionIdentity revision =
        m_facade ? m_facade->currentRevision() : pdf::PDFRevisionIdentity();
    m_coordinator->invalidate(revision);
}

int ViewportCommandBridge::resolvedPageIndex() const
{
    if (!m_viewport)
    {
        return -1;
    }

    const int page = m_viewport->currentPage();
    if (page >= 0)
    {
        return page;
    }

    return m_viewport->pageCount() > 0 ? 0 : -1;
}

int ViewportCommandBridge::lastPageIndex() const
{
    const int count = m_viewport ? m_viewport->pageCount() : 0;
    return count > 0 ? count - 1 : -1;
}

int ViewportCommandBridge::blockIndexForPage(int pageIndex) const
{
    if (!m_viewport)
    {
        return 0;
    }

    switch (m_viewport->pageLayout())
    {
        case PageLayout::TwoPagesLeft:
        case PageLayout::TwoPagesRight:
        case PageLayout::TwoColumnLeft:
        case PageLayout::TwoColumnRight:
            return pageIndex / 2;

        case PageLayout::SinglePage:
        case PageLayout::OneColumn:
            break;
    }

    return pageIndex;
}

void ViewportCommandBridge::goToPage(int pageIndex)
{
    if (!m_viewport)
    {
        return;
    }

    const int count = m_viewport->pageCount();
    if (count <= 0)
    {
        return;
    }

    const int bounded = qBound(0, pageIndex, count - 1);

    if (m_viewport->isBlockMode())
    {
        m_viewport->setBlockIndex(blockIndexForPage(bounded));
        return;
    }

    const QRect rect = m_viewport->placedPageRect(bounded);
    if (rect.isEmpty())
    {
        return;
    }

    m_viewport->setOffset(QPoint(m_viewport->offset().x(), m_viewport->offset().y() - rect.top()));
}

void ViewportCommandBridge::zoomByStep(qreal factor)
{
    if (!m_viewport || factor <= 0.0)
    {
        return;
    }

    m_viewport->setZoom(m_viewport->zoom() * factor);
}

void ViewportCommandBridge::applyZoomHint(ZoomHint hint)
{
    if (!m_viewport)
    {
        return;
    }

    m_viewport->setZoom(m_viewport->zoomHint(hint));
}

void ViewportCommandBridge::rotateBy(int quarterTurns)
{
    if (!m_viewport)
    {
        return;
    }

    pdf::PageRotation rotation = m_viewport->rotation();
    if (quarterTurns > 0)
    {
        for (int step = 0; step < quarterTurns; ++step)
        {
            rotation = pdf::getPageRotationRotatedRight(rotation);
        }
    }
    else
    {
        for (int step = 0; step < -quarterTurns; ++step)
        {
            rotation = pdf::getPageRotationRotatedLeft(rotation);
        }
    }

    m_viewport->setRotation(rotation);
}

}   // namespace pdfinteraction
