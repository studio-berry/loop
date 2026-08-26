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

enum class Operation
{
    Next,
    Previous,
    Start,
    End,
    ZoomIn,
    ZoomOut,
    FitPage,
    FitWidth,
    FitHeight,
    RotateLeft,
    RotateRight,
};

struct CommandSpec
{
    CommandId id;
    Operation operation;
};

const CommandSpec kCommandSpecs[] = {
    { ViewportCommandBridge::GoToNextPageCommandId, Operation::Next },
    { ViewportCommandBridge::GoToPreviousPageCommandId, Operation::Previous },
    { ViewportCommandBridge::GoToDocumentStartCommandId, Operation::Start },
    { ViewportCommandBridge::GoToDocumentEndCommandId, Operation::End },
    { ViewportCommandBridge::ZoomInCommandId, Operation::ZoomIn },
    { ViewportCommandBridge::ZoomOutCommandId, Operation::ZoomOut },
    { ViewportCommandBridge::FitPageCommandId, Operation::FitPage },
    { ViewportCommandBridge::FitWidthCommandId, Operation::FitWidth },
    { ViewportCommandBridge::FitHeightCommandId, Operation::FitHeight },
    { ViewportCommandBridge::RotateLeftCommandId, Operation::RotateLeft },
    { ViewportCommandBridge::RotateRightCommandId, Operation::RotateRight },
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

    auto bindAction = [this](const CommandSpec& spec)
    {
        CommandCatalog::Handler handler;
        handler.invoke = [this, spec](CommandInvocationId invocation, const QVariantMap&)
        {
            switch (spec.operation)
            {
                case Operation::Next:
                    goToPage(resolvedPageIndex() + 1);
                    break;
                case Operation::Previous:
                    goToPage(resolvedPageIndex() - 1);
                    break;
                case Operation::Start:
                    goToPage(0);
                    break;
                case Operation::End:
                    goToPage(lastPageIndex());
                    break;
                case Operation::ZoomIn:
                    zoomByStep(ViewportController::ZoomStep);
                    break;
                case Operation::ZoomOut:
                    zoomByStep(1.0 / ViewportController::ZoomStep);
                    break;
                case Operation::FitPage:
                    applyZoomHint(ZoomHint::Fit);
                    break;
                case Operation::FitWidth:
                    applyZoomHint(ZoomHint::FitWidth);
                    break;
                case Operation::FitHeight:
                    applyZoomHint(ZoomHint::FitHeight);
                    break;
                case Operation::RotateLeft:
                    rotateBy(-1);
                    break;
                case Operation::RotateRight:
                    rotateBy(1);
                    break;
            }
            finish(invocation);
        };
        return m_catalog->setHandler(spec.id, std::move(handler));
    };

    bool registered = true;
    for (const CommandSpec& spec : kCommandSpecs)
    {
        registered = bindAction(spec) && registered;
    }

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

    QHash<CommandId, bool> availability;
    for (const CommandSpec& spec : kCommandSpecs)
    {
        m_catalog->clearHandler(spec.id);
        availability.insert(spec.id, false);
    }
    m_catalog->setEnabledBatch(availability);

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

    QHash<CommandId, bool> availability;
    for (const CommandSpec& spec : kCommandSpecs)
    {
        bool enabled = false;
        switch (spec.operation)
        {
            case Operation::Next:
                enabled = hasPages && page >= 0 && page < count - 1;
                break;
            case Operation::Previous:
                enabled = hasPages && page > 0;
                break;
            case Operation::Start:
                enabled = hasPages && page > 0;
                break;
            case Operation::End:
                enabled = hasPages && page >= 0 && page < count - 1;
                break;
            case Operation::ZoomIn:
                enabled = hasPages && zoom < ViewportController::MaximumZoom - 1e-9;
                break;
            case Operation::ZoomOut:
                enabled = hasPages && zoom > ViewportController::MinimumZoom + 1e-9;
                break;
            case Operation::FitPage:
            case Operation::FitWidth:
            case Operation::FitHeight:
            case Operation::RotateLeft:
            case Operation::RotateRight:
                enabled = hasPages;
                break;
        }
        availability.insert(spec.id, enabled);
    }
    m_catalog->setEnabledBatch(availability);
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

    return m_viewport->blockIndexForPage(pageIndex);
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
