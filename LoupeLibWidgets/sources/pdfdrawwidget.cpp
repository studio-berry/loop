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

#include "pdfdrawwidget.h"
#include "pdfaccessibility.h"
#include "pdfdrawspacecontroller.h"
#include "pdfcompiler.h"
#include "pdfwidgettool.h"
#include "pdfannotation.h"
#include "pdfwidgetannotation.h"
#include "pdfwidgetformmanager.h"
#include "pdfblpainter.h"
#include "pdfpagecontentelements.h"
#include "pdfinteractionstate_p.h"
#include "pdfinteractiontrace_p.h"
#include "pdfinteractiontracewidget_p.h"
#include "pdfjobscheduler.h"

#include <QPainter>
#include <QGridLayout>
#include <QKeyEvent>
#include <QApplication>
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QPixmapCache>
#include <QColorSpace>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QScreen>
#include <QWindow>

#include "pdfdbgheap.h"

namespace pdf
{

PDFWidget::PDFWidget(const PDFCMSManager* cmsManager, RendererEngine engine, QWidget* parent) :
    QWidget(parent),
    m_cmsManager(cmsManager),
    m_toolManager(nullptr),
    m_annotationManager(nullptr),
    m_formManager(nullptr),
    m_drawWidget(nullptr),
    m_horizontalScrollBar(nullptr),
    m_verticalScrollBar(nullptr),
    m_proxy(nullptr),
    m_rendererEngine(engine)
{
    PDFAccessibility::install();
    m_drawWidget = new PDFDrawWidget(this, this);
    m_horizontalScrollBar = new QScrollBar(Qt::Horizontal, this);
    m_verticalScrollBar = new QScrollBar(Qt::Vertical, this);

    QGridLayout* layout = new QGridLayout(this);
    layout->setSpacing(0);
    layout->addWidget(m_drawWidget->getWidget(), 0, 0);
    layout->addWidget(m_horizontalScrollBar, 1, 0);
    layout->addWidget(m_verticalScrollBar, 0, 1);
    layout->setContentsMargins(QMargins());

    setLayout(layout);
    setFocusProxy(m_drawWidget->getWidget());

    m_proxy = new PDFDrawWidgetProxy(this);
    m_proxy->init(this);
    m_proxy->updateRenderer(m_rendererEngine);
    connect(m_proxy, &PDFDrawWidgetProxy::renderingError, this, &PDFWidget::onRenderingError);
    connect(m_proxy, &PDFDrawWidgetProxy::repaintNeeded, m_drawWidget->getWidget(), QOverload<>::of(&QWidget::update));
    connect(m_proxy, &PDFDrawWidgetProxy::pageImageChanged, this, &PDFWidget::onPageImageChanged);
}

PDFWidget::~PDFWidget()
{

}

bool PDFWidget::focusNextPrevChild(bool next)
{
    if (m_formManager && m_formManager->focusNextPrevFormField(next))
    {
        return true;
    }

    return QWidget::focusNextPrevChild(next);
}

void PDFWidget::setDocument(const PDFModifiedDocument& document, std::vector<PDFSignatureVerificationResult> signatureVerificationResult)
{
    m_proxy->setDocument(document, std::move(signatureVerificationResult));
    m_pageRenderingErrors.clear();
    m_drawWidget->getWidget()->update();
    if (PDFDrawWidget* drawWidget = dynamic_cast<PDFDrawWidget*>(m_drawWidget))
    {
        drawWidget->notifyAccessibilityUpdate();
    }
}

void PDFWidget::updateRenderer(RendererEngine engine)
{
    m_rendererEngine = engine;
    m_proxy->updateRenderer(m_rendererEngine);
}

void PDFWidget::updateCacheLimits(qsizetype compiledPageCacheLimit, int thumbnailsCacheLimit, int fontCacheLimit, int instancedFontCacheLimit)
{
    m_proxy->getCompiler()->setCacheLimit(compiledPageCacheLimit);
    QPixmapCache::setCacheLimit(qMax(thumbnailsCacheLimit, 16384));
    m_proxy->getFontCache()->setCacheLimits(fontCacheLimit, instancedFontCacheLimit);
}

void PDFWidget::setSmoothWheelScrolling(bool enabled)
{
    if (PDFDrawWidget* drawWidget = dynamic_cast<PDFDrawWidget*>(m_drawWidget))
    {
        drawWidget->setSmoothWheelScrolling(enabled);
    }
}

void PDFWidget::setWheelScrollSpeed(int horizontalPercent, int verticalPercent)
{
    if (PDFDrawWidget* drawWidget = dynamic_cast<PDFDrawWidget*>(m_drawWidget))
    {
        drawWidget->setWheelScrollSpeed(horizontalPercent, verticalPercent);
    }
}

int PDFWidget::getPageRenderingErrorCount() const
{
    int count = 0;
    for (const auto& item : m_pageRenderingErrors)
    {
        count += item.second.size();
    }
    return count;
}

void PDFWidget::onRenderingError(PDFInteger pageIndex, const QList<PDFRenderError>& errors)
{
    // Empty list of error should not be reported!
    Q_ASSERT(!errors.empty());
    m_pageRenderingErrors[pageIndex] = errors;
    Q_EMIT pageRenderingErrorsChanged(pageIndex, errors.size());
}

void PDFWidget::onPageImageChanged(bool all, const std::vector<PDFInteger>& pages)
{
    if (all)
    {
        m_drawWidget->getWidget()->update();
    }
    else
    {
        std::vector<PDFInteger> currentPages = m_drawWidget->getCurrentPages();

        Q_ASSERT(std::is_sorted(pages.cbegin(), pages.cend()));
        for (PDFInteger pageIndex : currentPages)
        {
            if (std::binary_search(pages.cbegin(), pages.cend(), pageIndex))
            {
                m_drawWidget->getWidget()->update();
                return;
            }
        }
    }
}

void PDFWidget::onSceneActiveStateChanged(bool)
{
    Q_EMIT sceneActivityChanged();
}

void PDFWidget::removeInputInterface(IDrawWidgetInputInterface* inputInterface)
{
    auto it = std::find(m_inputInterfaces.begin(), m_inputInterfaces.end(), inputInterface);
    if (it != m_inputInterfaces.end())
    {
        m_inputInterfaces.erase(it);
    }

    PDFPageContentScene* scene = dynamic_cast<PDFPageContentScene*>(inputInterface);
    if (scene)
    {
        auto itScene = std::find(m_scenes.begin(), m_scenes.end(), inputInterface);
        if (itScene != m_scenes.end())
        {
            m_scenes.erase(itScene);
            disconnect(scene, &PDFPageContentScene::sceneActiveStateChanged, this, &PDFWidget::onSceneActiveStateChanged);
        }
    }
}

void PDFWidget::addInputInterface(IDrawWidgetInputInterface* inputInterface)
{
    if (inputInterface)
    {
        m_inputInterfaces.push_back(inputInterface);
        std::sort(m_inputInterfaces.begin(), m_inputInterfaces.end(), IDrawWidgetInputInterface::Comparator());

        PDFPageContentScene* scene = dynamic_cast<PDFPageContentScene*>(inputInterface);
        if (scene)
        {
            m_scenes.push_back(scene);
            connect(scene, &PDFPageContentScene::sceneActiveStateChanged, this, &PDFWidget::onSceneActiveStateChanged);
        }
    }
}

bool PDFWidget::isAnySceneActive(PDFPageContentScene* sceneToSkip) const
{
    for (PDFPageContentScene* scene : m_scenes)
    {
        if (scene->isActive() && scene != sceneToSkip)
        {
            return true;
        }
    }

    return false;
}

PDFWidgetFormManager* PDFWidget::getFormManager() const
{
    return m_formManager;
}

void PDFWidget::setFormManager(PDFWidgetFormManager* formManager)
{
    removeInputInterface(m_formManager);
    m_formManager = formManager;
    addInputInterface(m_formManager);
}

void PDFWidget::setToolManager(PDFToolManager* toolManager)
{
    removeInputInterface(m_toolManager);
    m_toolManager = toolManager;
    addInputInterface(m_toolManager);
}

void PDFWidget::setAnnotationManager(PDFWidgetAnnotationManager* annotationManager)
{
    removeInputInterface(m_annotationManager);
    m_annotationManager = annotationManager;
    addInputInterface(m_annotationManager);
}

PDFDrawWidget::PDFDrawWidget(PDFWidget* widget, QWidget* parent) :
    BaseClass(parent),
    m_widget(widget),
    m_mouseOperation(MouseOperation::None),
    m_interactionState(std::make_unique<PDFInteractionState>())
{
    auto* traceRecorder = new PDFInteractionTraceRecorder({}, this);
    traceRecorder->setObjectName(QString::fromLatin1(PDFInteractionTraceObjectName));

    this->setFocusPolicy(Qt::StrongFocus);
    this->setAccessibleName(tr("Document canvas"));
    this->setAccessibleDescription(tr("Inspect the active document page with keyboard, pointer, or assistive technology."));
    this->setMouseTracking(true);
    this->setAcceptDrops(true);

    QObject::connect(&m_autoScrollTimer, &QTimer::timeout, this, &PDFDrawWidget::onAutoScrollTimeout);
    QObject::connect(&m_wheelScrollTimer, &QTimer::timeout, this, &PDFDrawWidget::onWheelScrollTimeout);
}

PDFDrawWidget::~PDFDrawWidget()
{
    if (m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::Destroyed);
    }
    resetInteractionInputs();
}

void PDFDrawWidget::ensureInteractionRevisionConnection()
{
    if (m_interactionRevisionConnected || !m_widget)
    {
        return;
    }

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    PDFDocumentContext* context = proxy ? proxy->getDocumentContext() : nullptr;
    if (!context)
    {
        return;
    }

    m_interactionRevisionConnection = QObject::connect(context, &PDFDocumentContext::revisionChanged, this,
                                                       [this](const PDFRevisionIdentity& previous, const PDFRevisionIdentity& current) {
                                                           if (m_interactionState)
                                                           {
                                                               const PDFInteractionState::CancelReason reason = previous.document == current.document
                                                                   ? PDFInteractionState::CancelReason::RevisionChanged
                                                                   : PDFInteractionState::CancelReason::DocumentReplaced;
                                                               m_interactionState->cancel(reason);
                                                           }
                                                           resetInteractionInputs();
                                                           updateCursor();
                                                       }, Qt::UniqueConnection);
    m_interactionRevisionConnected = true;
}

void PDFDrawWidget::resetInteractionInputs()
{
    m_autoScrollTimer.stop();
    m_wheelScrollTimer.stop();
    m_autoScrollOffset = QPointF(0.0, 0.0);
    m_wheelScrollPendingOffset = QPointF(0.0, 0.0);
    m_mouseOperation = MouseOperation::None;
    m_lastMousePosition = QPoint();
    m_autoScrollMousePosition = QPoint();
    m_autoScrollLastElapsedTimer.invalidate();
}

void PDFDrawWidget::cancelTransientInteraction()
{
    ensureInteractionRevisionConnection();
    if (m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::Explicit);
    }
    resetInteractionInputs();
    updateCursor();
}

void PDFDrawWidget::finishTransientInteraction()
{
    ensureInteractionRevisionConnection();
    if (!m_interactionState || !m_widget || !m_widget->getDrawWidgetProxy())
    {
        return;
    }

    const PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    const PDFInteractionState::Token token = m_interactionState->currentToken();
    if (!token.isValid())
    {
        return;
    }

    if (!m_interactionState->complete(token, proxy->getDocumentRevision()))
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::RevisionChanged);
        resetInteractionInputs();
    }
}

bool PDFDrawWidget::isTransientInteractionCurrent() const
{
    if (!m_interactionState || !m_widget || !m_widget->getDrawWidgetProxy())
    {
        return false;
    }

    const PDFInteractionState::Token token = m_interactionState->currentToken();
    return m_interactionState->isCurrent(token, m_widget->getDrawWidgetProxy()->getDocumentRevision());
}

void PDFDrawWidget::setSmoothWheelScrolling(bool enabled)
{
    m_smoothWheelScrolling = enabled;
    if (!m_smoothWheelScrolling)
    {
        m_wheelScrollTimer.stop();
        m_wheelScrollPendingOffset = QPointF(0.0, 0.0);
        if (m_interactionState && m_interactionState->isActive(PDFInteractionState::Kind::ZoomPan))
        {
            finishTransientInteraction();
        }
    }
}

void PDFDrawWidget::setWheelScrollSpeed(int horizontalPercent, int verticalPercent)
{
    m_horizontalWheelScrollSpeed = qMax(PDFReal(0.01), PDFReal(horizontalPercent) / PDFReal(100.0));
    m_verticalWheelScrollSpeed = qMax(PDFReal(0.01), PDFReal(verticalPercent) / PDFReal(100.0));
}

std::vector<PDFInteger> PDFDrawWidget::getCurrentPages() const
{
    return this->m_widget->getDrawWidgetProxy()->getPagesIntersectingRect(this->rect());
}

QString PDFDrawWidget::accessibleDocumentSummary() const
{
    const PDFDrawWidgetProxy* proxy = m_widget ? m_widget->getDrawWidgetProxy() : nullptr;
    const PDFDocument* document = proxy ? proxy->getDocument() : nullptr;
    if (!document || !document->getCatalog())
    {
        return tr("No document is currently open.");
    }

    const PDFInteger pageCount = document->getCatalog()->getPageCount();
    const std::vector<PDFInteger> pages = getCurrentPages();
    const PDFInteger currentPage = pages.empty() ? 0 : pages.front() + 1;
    const int zoomPercent = proxy ? qRound(proxy->getZoom() * 100.0) : 100;
    return tr("Document canvas. Page %1 of %2. Zoom %3 percent.")
        .arg(currentPage)
        .arg(pageCount)
        .arg(zoomPercent);
}

void PDFDrawWidget::notifyAccessibilityUpdate()
{
    QAccessibleValueChangeEvent event(this, accessibleDocumentSummary());
    QAccessible::updateAccessibility(&event);
}

QSize PDFDrawWidget::minimumSizeHint() const
{
    return QSize(200, 200);
}

bool PDFDrawWidget::event(QEvent* event)
{
    ensureInteractionRevisionConnection();

    if (event->type() == QEvent::ShortcutOverride)
    {
        PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::ShortcutOverride);
        return processEvent<QKeyEvent, &IDrawWidgetInputInterface::shortcutOverrideEvent>(static_cast<QKeyEvent*>(event));
    }

    return BaseClass::event(event);
}

void PDFDrawWidget::performMouseOperation(QPoint currentMousePosition)
{
    if (m_mouseOperation != MouseOperation::None && !isTransientInteractionCurrent())
    {
        if (m_interactionState)
        {
            m_interactionState->cancel(PDFInteractionState::CancelReason::RevisionChanged);
        }
        resetInteractionInputs();
        return;
    }

    switch (m_mouseOperation)
    {
        case MouseOperation::None:
            // No operation performed
            break;

        case MouseOperation::Translate:
        {
            QPoint difference = currentMousePosition - m_lastMousePosition;
            m_widget->getDrawWidgetProxy()->scrollByPixels(difference);
            m_lastMousePosition = currentMousePosition;
            break;
        }

        case MouseOperation::AutoScroll:
        {
            m_lastMousePosition = currentMousePosition;
            onAutoScrollTimeout();
            break;
        }

        default:
            Q_ASSERT(false);
    }
}

template<typename Event, void (IDrawWidgetInputInterface::* Function)(QWidget*, Event*)>
bool PDFDrawWidget::processEvent(Event* event)
{
    QString tooltip;
    for (IDrawWidgetInputInterface* inputInterface : m_widget->getInputInterfaces())
    {
        (inputInterface->*Function)(this, event);

        // Update tooltip
        if (tooltip.isEmpty())
        {
            tooltip = inputInterface->getTooltip();
        }

        // If event is accepted, then update cursor/tooltip and return
        if (event->isAccepted())
        {
            this->setToolTip(tooltip);
            this->updateCursor();
            return true;
        }
    }
    this->setToolTip(tooltip);

    return false;
}

void PDFDrawWidget::keyPressEvent(QKeyEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::KeyPress);

    event->ignore();

    if (event->key() == Qt::Key_Escape)
    {
        if (m_interactionState)
        {
            m_interactionState->cancel(PDFInteractionState::CancelReason::Escape);
        }
        resetInteractionInputs();
        updateCursor();
    }

    if (processEvent<QKeyEvent, &IDrawWidgetInputInterface::keyPressEvent>(event))
    {
        return;
    }

    // Vertical navigation
    QScrollBar* verticalScrollbar = m_widget->getVerticalScrollbar();
    if (verticalScrollbar->isVisible())
    {
        constexpr std::pair<QKeySequence::StandardKey, PDFDrawWidgetProxy::Operation> keyToOperations[] =
        {
            { QKeySequence::MoveToStartOfDocument, PDFDrawWidgetProxy::NavigateDocumentStart },
            { QKeySequence::MoveToEndOfDocument, PDFDrawWidgetProxy::NavigateDocumentEnd },
            { QKeySequence::MoveToNextPage, PDFDrawWidgetProxy::NavigateNextPage },
            { QKeySequence::MoveToPreviousPage, PDFDrawWidgetProxy::NavigatePreviousPage },
            { QKeySequence::MoveToNextLine, PDFDrawWidgetProxy::NavigateNextStep },
            { QKeySequence::MoveToPreviousLine, PDFDrawWidgetProxy::NavigatePreviousStep }
        };

        for (const std::pair<QKeySequence::StandardKey, PDFDrawWidgetProxy::Operation>& keyToOperation : keyToOperations)
        {
            if (event->matches(keyToOperation.first))
            {
                m_widget->getDrawWidgetProxy()->performOperation(keyToOperation.second);
                event->accept();
            }
        }
    }

    updateCursor();
}

void PDFDrawWidget::keyReleaseEvent(QKeyEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::KeyRelease);

    event->ignore();

    if (processEvent<QKeyEvent, &IDrawWidgetInputInterface::keyReleaseEvent>(event))
    {
        return;
    }

    event->accept();
}

void PDFDrawWidget::mousePressEvent(QMouseEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::MousePress);

    event->ignore();

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    if (m_interactionState && proxy)
    {
        m_interactionState->begin(PDFInteractionState::Kind::ToolGesture, proxy->getDocumentRevision());
    }

    if (processEvent<QMouseEvent, &IDrawWidgetInputInterface::mousePressEvent>(event))
    {
        return;
    }

    if (event->button() == Qt::LeftButton)
    {
        if (m_interactionState && proxy)
        {
            m_interactionState->begin(PDFInteractionState::Kind::Drag, proxy->getDocumentRevision());
        }
        m_mouseOperation = MouseOperation::Translate;
        m_lastMousePosition = event->pos();
    }

    if (event->button() == Qt::MiddleButton)
    {
        if (m_mouseOperation == MouseOperation::AutoScroll)
        {
            m_mouseOperation = MouseOperation::None;
            m_autoScrollTimer.stop();
            m_autoScrollLastElapsedTimer.restart();
            m_autoScrollOffset = QPointF(0.0, 0.0);
            finishTransientInteraction();
        }
        else
        {
            if (m_interactionState && proxy)
            {
                m_interactionState->begin(PDFInteractionState::Kind::Drag, proxy->getDocumentRevision());
            }
            m_mouseOperation = MouseOperation::AutoScroll;
            m_autoScrollMousePosition = event->pos();
            m_autoScrollLastElapsedTimer.restart();
            m_autoScrollOffset = QPointF(0.0, 0.0);
            m_lastMousePosition = event->pos();
            m_autoScrollTimer.setInterval(10);
            m_autoScrollTimer.start();
        }
    }
    else if (event->button() != Qt::LeftButton && m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::Explicit);
    }

    updateCursor();
    event->accept();
}

void PDFDrawWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::MouseDoubleClick);

    event->ignore();

    if (processEvent<QMouseEvent, &IDrawWidgetInputInterface::mouseDoubleClickEvent>(event))
    {
        return;
    }
}

void PDFDrawWidget::mouseReleaseEvent(QMouseEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::MouseRelease);

    event->ignore();

    if (processEvent<QMouseEvent, &IDrawWidgetInputInterface::mouseReleaseEvent>(event))
    {
        if (m_mouseOperation != MouseOperation::AutoScroll)
        {
            finishTransientInteraction();
        }
        return;
    }

    performMouseOperation(event->pos());

    switch (m_mouseOperation)
    {
        case MouseOperation::None:
            break;

        case MouseOperation::Translate:
        {
            if (event->button() != Qt::MiddleButton)
            {
                m_mouseOperation = MouseOperation::None;
                finishTransientInteraction();
            }
            break;
        }

        case MouseOperation::AutoScroll:
            break;

        default:
            Q_ASSERT(false);
            break;
    }

    updateCursor();
    event->accept();
}

void PDFDrawWidget::mouseMoveEvent(QMouseEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::MouseMove);

    event->ignore();

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    if (m_interactionState && proxy && m_mouseOperation == MouseOperation::None && !m_interactionState->snapshot().active())
    {
        m_interactionState->begin(PDFInteractionState::Kind::Hover, proxy->getDocumentRevision());
    }

    if (processEvent<QMouseEvent, &IDrawWidgetInputInterface::mouseMoveEvent>(event))
    {
        return;
    }

    performMouseOperation(event->pos());
    if (m_interactionState && proxy && m_mouseOperation != MouseOperation::None)
    {
        const PDFInteractionState::Token token = m_interactionState->currentToken();
        if (!m_interactionState->update(token, proxy->getDocumentRevision()))
        {
            m_interactionState->cancel(PDFInteractionState::CancelReason::RevisionChanged);
            resetInteractionInputs();
        }
    }
    updateCursor();
    event->accept();
}

void PDFDrawWidget::dragEnterEvent(QDragEnterEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::DragEnter);

    event->ignore();

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    if (m_interactionState && proxy)
    {
        m_interactionState->begin(PDFInteractionState::Kind::Drag, proxy->getDocumentRevision());
    }

    PDFWidgetAnnotationManager* annotationManager = m_widget->getAnnotationManager();
    if (annotationManager && annotationManager->canAcceptAnnotationDrag(event->mimeData()))
    {
        const Qt::DropAction action = event->modifiers().testFlag(Qt::ControlModifier) ? Qt::CopyAction : Qt::MoveAction;
        event->setDropAction(action);
        event->accept();
    }
    else if (m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::InvalidDrop);
    }
}

void PDFDrawWidget::dragMoveEvent(QDragMoveEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::DragMove);

    event->ignore();

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    if (m_interactionState && proxy)
    {
        m_interactionState->begin(PDFInteractionState::Kind::Drag, proxy->getDocumentRevision());
    }

    PDFWidgetAnnotationManager* annotationManager = m_widget->getAnnotationManager();
    if (annotationManager && annotationManager->canAcceptAnnotationDrag(event->mimeData()))
    {
        const Qt::DropAction action = event->modifiers().testFlag(Qt::ControlModifier) ? Qt::CopyAction : Qt::MoveAction;
        event->setDropAction(action);
        event->accept();
    }
    else if (m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::InvalidDrop);
    }
}

void PDFDrawWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    ensureInteractionRevisionConnection();
    if (m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::InvalidDrop);
    }
    event->accept();
}

void PDFDrawWidget::dropEvent(QDropEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::Drop);

    event->ignore();

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    if (m_interactionState && proxy)
    {
        m_interactionState->begin(PDFInteractionState::Kind::Drag, proxy->getDocumentRevision());
    }

    PDFWidgetAnnotationManager* annotationManager = m_widget->getAnnotationManager();
    if (annotationManager && annotationManager->canAcceptAnnotationDrag(event->mimeData()))
    {
        const Qt::DropAction action = event->modifiers().testFlag(Qt::ControlModifier) ? Qt::CopyAction : Qt::MoveAction;
        if (annotationManager->handleAnnotationDrop(event->mimeData(), event->position().toPoint(), action))
        {
            event->setDropAction(action);
            event->accept();
            finishTransientInteraction();
        }
        else if (m_interactionState)
        {
            m_interactionState->cancel(PDFInteractionState::CancelReason::InvalidDrop);
        }
    }
    else if (m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::InvalidDrop);
    }
}

void PDFDrawWidget::updateCursor()
{
    std::optional<QCursor> cursor;

    for (IDrawWidgetInputInterface* inputInterface : m_widget->getInputInterfaces())
    {
        cursor = inputInterface->getCursor();

        if (cursor)
        {
            // We have found cursor
            break;
        }
    }

    if (!cursor)
    {
        switch (m_mouseOperation)
        {
            case MouseOperation::None:
                cursor = QCursor(Qt::OpenHandCursor);
                break;

            case MouseOperation::Translate:
                cursor = QCursor(Qt::ClosedHandCursor);
                break;

            case MouseOperation::AutoScroll:
                cursor = QCursor(Qt::SizeAllCursor);
                break;

            default:
                Q_ASSERT(false);
                break;
        }
    }

    if (cursor)
    {
        this->setCursor(*cursor);
    }
    else
    {
        this->unsetCursor();
    }
}

void PDFDrawWidget::onAutoScrollTimeout()
{
    ensureInteractionRevisionConnection();

    if (m_mouseOperation != MouseOperation::AutoScroll)
    {
        return;
    }

    if (!isTransientInteractionCurrent())
    {
        if (m_interactionState)
        {
            m_interactionState->cancel(PDFInteractionState::CancelReason::RevisionChanged);
        }
        resetInteractionInputs();
        updateCursor();
        return;
    }

    QPointF offset = m_autoScrollMousePosition - m_lastMousePosition;
    QPointF scrollOffset = m_autoScrollOffset;

    qreal secondsElapsed = qreal(m_autoScrollLastElapsedTimer.nsecsElapsed()) * 0.000000001;
    m_autoScrollLastElapsedTimer.restart();
    scrollOffset += offset * secondsElapsed;

    int scrollX = qFloor(scrollOffset.x());
    int scrollY = qFloor(scrollOffset.y());

    scrollOffset -= QPointF(scrollX, scrollY);
    m_autoScrollOffset = scrollOffset;

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    proxy->scrollByPixels(QPoint(scrollX, scrollY));
}

void PDFDrawWidget::onWheelScrollTimeout()
{
    ensureInteractionRevisionConnection();

    if (m_wheelScrollPendingOffset.isNull())
    {
        m_wheelScrollTimer.stop();
        if (m_interactionState && m_interactionState->isActive(PDFInteractionState::Kind::ZoomPan))
        {
            finishTransientInteraction();
        }
        return;
    }

    if (!isTransientInteractionCurrent())
    {
        if (m_interactionState)
        {
            m_interactionState->cancel(PDFInteractionState::CancelReason::RevisionChanged);
        }
        resetInteractionInputs();
        updateCursor();
        return;
    }

    QPointF stepOffset = m_wheelScrollPendingOffset * 0.35;
    int stepX = qRound(stepOffset.x());
    int stepY = qRound(stepOffset.y());

    if (stepX == 0 && !qFuzzyIsNull(m_wheelScrollPendingOffset.x()))
    {
        stepX = (m_wheelScrollPendingOffset.x() > 0.0) ? 1 : -1;
    }
    if (stepY == 0 && !qFuzzyIsNull(m_wheelScrollPendingOffset.y()))
    {
        stepY = (m_wheelScrollPendingOffset.y() > 0.0) ? 1 : -1;
    }

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    const QPoint scrollStep(stepX, stepY);
    const QPoint appliedOffset = proxy->scrollByPixels(scrollStep);

    if (appliedOffset.y() == 0 && scrollStep.y() != 0 && proxy->isBlockMode())
    {
        // We must move to another block (we are in block mode)
        const bool up = scrollStep.y() > 0;

        QScrollBar* verticalScrollbar = m_widget->getVerticalScrollbar();
        const int newValue = verticalScrollbar->value() + (up ? -1 : 1);

        if (newValue >= verticalScrollbar->minimum() && newValue <= verticalScrollbar->maximum())
        {
            verticalScrollbar->setValue(newValue);
            proxy->scrollByPixels(QPoint(0, up ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max()));
        }
    }

    if (appliedOffset.x() == 0 && scrollStep.x() != 0)
    {
        m_wheelScrollPendingOffset.setX(0.0);
    }
    else
    {
        m_wheelScrollPendingOffset.setX(m_wheelScrollPendingOffset.x() - appliedOffset.x());
    }

    if (appliedOffset.y() == 0 && scrollStep.y() != 0)
    {
        m_wheelScrollPendingOffset.setY(0.0);
    }
    else
    {
        m_wheelScrollPendingOffset.setY(m_wheelScrollPendingOffset.y() - appliedOffset.y());
    }

    if (qAbs(m_wheelScrollPendingOffset.x()) < 0.5)
    {
        m_wheelScrollPendingOffset.setX(0.0);
    }
    if (qAbs(m_wheelScrollPendingOffset.y()) < 0.5)
    {
        m_wheelScrollPendingOffset.setY(0.0);
    }

    if (m_wheelScrollPendingOffset.isNull())
    {
        m_wheelScrollTimer.stop();
        finishTransientInteraction();
    }
}

void PDFDrawWidget::wheelEvent(QWheelEvent* event)
{
    ensureInteractionRevisionConnection();

    PDFInteractionTraceInputScope traceScope(this, PDFInteractionTraceRecorder::InputKind::Wheel);

    event->ignore();

    if (processEvent<QWheelEvent, &IDrawWidgetInputInterface::wheelEvent>(event))
    {
        return;
    }

    Qt::KeyboardModifiers keyboardModifiers = QApplication::keyboardModifiers();
    const bool shiftModifier = keyboardModifiers.testFlag(Qt::ShiftModifier);

    PDFDrawWidgetProxy* proxy = m_widget->getDrawWidgetProxy();
    if (keyboardModifiers.testFlag(Qt::ControlModifier))
    {
        if (m_interactionState)
        {
            m_interactionState->begin(PDFInteractionState::Kind::ZoomPan, proxy->getDocumentRevision());
        }
        // Zoom in/Zoom out
        const int angleDeltaY = event->angleDelta().y();
        const PDFReal zoom = m_widget->getDrawWidgetProxy()->getZoom();
        const PDFReal zoomStep = std::pow(PDFDrawWidgetProxy::ZOOM_STEP, static_cast<PDFReal>(angleDeltaY) / static_cast<PDFReal>(QWheelEvent::DefaultDeltasPerStep));
        const PDFReal newZoom = zoom * zoomStep;
        proxy->zoom(newZoom, event->position());
        finishTransientInteraction();
    }
    else
    {
        if (m_interactionState)
        {
            m_interactionState->begin(PDFInteractionState::Kind::ZoomPan, proxy->getDocumentRevision());
        }
        // Move Up/Down. Angle is negative, if wheel is scrolled down. First we try to scroll by pixel delta.
        // Otherwise we compute scroll using angle.
        QPoint scrollByPixels = event->pixelDelta();
        if (!scrollByPixels.isNull() && shiftModifier)
        {
            if (scrollByPixels.x() == 0)
            {
                scrollByPixels.setX(scrollByPixels.y());
            }
            scrollByPixels.setY(0);
        }

        if (scrollByPixels.isNull())
        {
            QPoint angleDelta = event->angleDelta();
            if (shiftModifier)
            {
                if (angleDelta.x() == 0)
                {
                    angleDelta.setX(angleDelta.y());
                }
                angleDelta.setY(0);
            }

            int stepVertical = 0;
            int stepHorizontal = 0;

            if (proxy->isBlockMode())
            {
                // In block mode, we must calculate pixel offsets differently - scrollbars corresponds to indices of blocks,
                // not to the pixels.
                QRect boundingBox = proxy->getPagesIntersectingRectBoundingBox(this->rect());

                if (boundingBox.isEmpty())
                {
                    // This occurs, when we have not opened a document
                    boundingBox = this->rect();
                }

                stepVertical = shiftModifier ? qMax(boundingBox.height(), 1) : qMax(boundingBox.height() / 10, 1);
                stepHorizontal = shiftModifier ? qMax(boundingBox.width(), 1) : qMax(boundingBox.width() / 10, 1);
            }
            else
            {
                stepVertical = shiftModifier ? m_widget->getVerticalScrollbar()->pageStep() : m_widget->getVerticalScrollbar()->singleStep();
                stepHorizontal = shiftModifier ? m_widget->getHorizontalScrollbar()->pageStep() : m_widget->getHorizontalScrollbar()->singleStep();
            }

            const int scrollVertical = stepVertical * static_cast<PDFReal>(angleDelta.y()) / static_cast<PDFReal>(QWheelEvent::DefaultDeltasPerStep);
            const int scrollHorizontal = stepHorizontal * static_cast<PDFReal>(angleDelta.x()) / static_cast<PDFReal>(QWheelEvent::DefaultDeltasPerStep);

            scrollByPixels = QPoint(scrollHorizontal, scrollVertical);
        }

        scrollByPixels.setX(qRound(static_cast<PDFReal>(scrollByPixels.x()) * m_horizontalWheelScrollSpeed));
        scrollByPixels.setY(qRound(static_cast<PDFReal>(scrollByPixels.y()) * m_verticalWheelScrollSpeed));

        if (m_smoothWheelScrolling)
        {
            m_wheelScrollPendingOffset += QPointF(scrollByPixels);
            if (!m_wheelScrollTimer.isActive())
            {
                m_wheelScrollTimer.setInterval(10);
                m_wheelScrollTimer.start();
            }
            onWheelScrollTimeout();
        }
        else
        {
            QPoint offset = proxy->scrollByPixels(scrollByPixels);

            if (offset.y() == 0 && scrollByPixels.y() != 0 && proxy->isBlockMode())
            {
                // We must move to another block (we are in block mode)
                bool up = scrollByPixels.y() > 0;

                QScrollBar* verticalScrollbar = m_widget->getVerticalScrollbar();
                const int newValue = verticalScrollbar->value() + (up ? -1 : 1);

                if (newValue >= verticalScrollbar->minimum() && newValue <= verticalScrollbar->maximum())
                {
                    verticalScrollbar->setValue(newValue);
                    proxy->scrollByPixels(QPoint(0, up ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max()));
                }
            }
            finishTransientInteraction();
        }
    }

    event->accept();
}

void PDFDrawWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    ensureInteractionRevisionConnection();
    if (m_interactionState && m_interactionState->snapshot().active() && !isTransientInteractionCurrent())
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::RevisionChanged);
        resetInteractionInputs();
    }

    PDFDrawWidgetProxy* proxy = getPDFWidget()->getDrawWidgetProxy();
    PDFInteractionTraceRecorder* traceRecorder = findInteractionTraceRecorder(this);
    const PDFRenderer::Features features = proxy->getFeatures();
    if (traceRecorder)
    {
        traceRecorder->setEnabled(features.testFlag(PDFRenderer::DisplayTimes));
    }

    const bool traceEnabled = traceRecorder && traceRecorder->isEnabled();
    const int visiblePages = traceEnabled ? static_cast<int>(proxy->getPagesIntersectingRect(this->rect()).size()) : -1;
    int queueDepth = -1;
    const RendererEngine rendererEngine = proxy->getRendererEngine();
    auto frameScope = traceEnabled
        ? traceRecorder->beginFrame(visiblePages, queueDepth)
        : PDFInteractionTraceRecorder::FrameScope();

    if (traceEnabled)
    {
        traceRecorder->observeDocumentRevision(proxy->getDocumentRevision().documentRevision);
        if (frameScope.id() % 4 == 0)
        {
            queueDepth = PDFJobScheduler::global().queuedJobs().size() + PDFJobScheduler::global().runningJobs().size();
            traceRecorder->recordQueueDepth(queueDepth);
        }

        QScreen* screen = nullptr;
        if (QWindow* windowHandle = window()->windowHandle())
        {
            screen = windowHandle->screen();
        }
        if (screen && screen->refreshRate() > 0.0)
        {
            traceRecorder->setRefreshRateHz(screen->refreshRate());
        }
    }

    switch (rendererEngine)
    {
        case RendererEngine::Blend2D_MultiThread:
        case RendererEngine::Blend2D_SingleThread:
        {
            QRect rect = this->rect();

            qreal devicePixelRatio = devicePixelRatioF();
            m_blend2DframeBuffer.setDevicePixelRatio(devicePixelRatio);

            qreal dpmX = logicalDpiX() / 0.0254;
            qreal dpmY = logicalDpiY() / 0.0254;
            m_blend2DframeBuffer.setDotsPerMeterX(qCeil(dpmX));
            m_blend2DframeBuffer.setDotsPerMeterY(qCeil(dpmY));

            QSize requiredSize = rect.size() * devicePixelRatio;
            if (m_blend2DframeBuffer.size() != requiredSize)
            {
                m_blend2DframeBuffer = QImage(requiredSize, QImage::Format_ARGB32_Premultiplied);
            }

            const bool multithreaded = rendererEngine == RendererEngine::Blend2D_MultiThread;
            PDFBLPaintDevice blPaintDevice(m_blend2DframeBuffer, multithreaded);
            QPainter blPainter;

            if (blPainter.begin(&blPaintDevice))
            {
                proxy->draw(&blPainter, rect);
                blPainter.end();
            }

            QPainter painter(this);
            auto compositionScope = traceRecorder && traceRecorder->isEnabled()
                ? traceRecorder->beginStage(PDFInteractionTraceRecorder::Stage::Composition)
                : PDFInteractionTraceRecorder::StageScope();
            painter.drawImage(QPoint(0, 0), m_blend2DframeBuffer);
            break;
        }

        case RendererEngine::QPainter:
        {
            QPainter painter(this);
            proxy->draw(&painter, this->rect());
            m_blend2DframeBuffer = QImage();
            break;
        }

        default:
            Q_ASSERT(false);
            break;
    }

    if (traceRecorder && traceRecorder->isEnabled())
    {
        auto overlayScope = traceRecorder->beginStage(PDFInteractionTraceRecorder::Stage::Overlay);
        drawInteractionTraceOverlay(this, traceRecorder->summary());
    }
}

void PDFDrawWidget::resizeEvent(QResizeEvent* event)
{
    ensureInteractionRevisionConnection();
    BaseClass::resizeEvent(event);

    getPDFWidget()->getDrawWidgetProxy()->update();
}

void PDFDrawWidget::focusOutEvent(QFocusEvent* event)
{
    if (m_interactionState)
    {
        m_interactionState->cancel(PDFInteractionState::CancelReason::FocusLost);
    }
    resetInteractionInputs();
    updateCursor();
    BaseClass::focusOutEvent(event);
}

void PDFDrawWidget::leaveEvent(QEvent* event)
{
    ensureInteractionRevisionConnection();
    if (m_interactionState)
    {
        m_interactionState->clearHover();
    }
    BaseClass::leaveEvent(event);
}

}   // namespace pdf
