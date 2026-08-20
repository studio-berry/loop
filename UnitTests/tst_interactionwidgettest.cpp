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

#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfdrawspacecontroller.h"
#include "pdfdrawwidget.h"

#include <memory>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QScrollBar>

#include <QtTest>

class InteractionWidgetTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void interactionsDoNotAdvanceDocumentRevision();
    void revisionChangeCancelsViewGesture();
    void escapeCancelsViewGesture();

private:
    void openLargeDocument();

    std::unique_ptr<pdf::PDFCMSManager> m_cmsManager;
    std::unique_ptr<pdf::PDFWidget> m_widget;
    std::unique_ptr<pdf::PDFDocument> m_document;
};

void InteractionWidgetTest::init()
{
    m_cmsManager = std::make_unique<pdf::PDFCMSManager>(nullptr);
    m_widget = std::make_unique<pdf::PDFWidget>(m_cmsManager.get(), pdf::RendererEngine::QPainter, nullptr);
    m_widget->resize(480, 360);
    m_widget->show();
    QCoreApplication::processEvents();
}

void InteractionWidgetTest::cleanup()
{
    m_widget.reset();
    m_document.reset();
    m_cmsManager.reset();
}

void InteractionWidgetTest::openLargeDocument()
{
    pdf::PDFDocumentBuilder builder;
    for (int index = 0; index < 4; ++index)
    {
        builder.appendPage(QRectF(0, 0, 1000, 1000));
    }
    m_document = std::make_unique<pdf::PDFDocument>(builder.build());
    m_widget->setDocument(pdf::PDFModifiedDocument(m_document.get(), nullptr), {});
    QCoreApplication::processEvents();
}

void InteractionWidgetTest::interactionsDoNotAdvanceDocumentRevision()
{
    openLargeDocument();

    pdf::PDFDrawWidget* drawWidget = dynamic_cast<pdf::PDFDrawWidget*>(m_widget->getDrawWidget());
    QVERIFY(drawWidget != nullptr);
    QWidget* canvas = drawWidget->getWidget();
    QVERIFY(canvas != nullptr);
    canvas->setFocus();

    const pdf::PDFRevisionIdentity before = m_widget->getDrawWidgetProxy()->getDocumentRevision();

    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(220, 180));
    QTest::mouseMove(canvas, QPoint(80, 80));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(80, 80));

    drawWidget->setSmoothWheelScrolling(false);
    QWheelEvent wheel(QPointF(220, 180), QPointF(220, 180), QPoint(0, 0), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &wheel);

    QTest::keyClick(canvas, Qt::Key_Escape);

    QCOMPARE(m_widget->getDrawWidgetProxy()->getDocumentRevision(), before);
}

void InteractionWidgetTest::revisionChangeCancelsViewGesture()
{
    openLargeDocument();

    pdf::PDFDrawWidget* drawWidget = dynamic_cast<pdf::PDFDrawWidget*>(m_widget->getDrawWidget());
    QVERIFY(drawWidget != nullptr);
    QWidget* canvas = drawWidget->getWidget();
    QVERIFY(canvas != nullptr);
    canvas->setFocus();

    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(220, 180));
    QTest::mouseMove(canvas, QPoint(80, 80));

    QScrollBar* vertical = m_widget->getVerticalScrollbar();
    QVERIFY(vertical != nullptr);
    const pdf::PDFRevisionIdentity beforeRevision = m_widget->getDrawWidgetProxy()->getDocumentRevision();

    m_widget->getDrawWidgetProxy()->getDocumentContext()->markModified(pdf::PDFModifiedDocument::PageContents);
    QCoreApplication::processEvents();
    const pdf::PDFRevisionIdentity afterRevisionIdentity = m_widget->getDrawWidgetProxy()->getDocumentRevision();
    QCOMPARE(afterRevisionIdentity.documentRevision, beforeRevision.documentRevision + 1);
    QCOMPARE(afterRevisionIdentity.cacheGeneration, beforeRevision.cacheGeneration + 1);
    const int afterRevision = vertical->value();

    QTest::mouseMove(canvas, QPoint(20, 20));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QCOMPARE(vertical->value(), afterRevision);
}

void InteractionWidgetTest::escapeCancelsViewGesture()
{
    openLargeDocument();

    pdf::PDFDrawWidget* drawWidget = dynamic_cast<pdf::PDFDrawWidget*>(m_widget->getDrawWidget());
    QVERIFY(drawWidget != nullptr);
    QWidget* canvas = drawWidget->getWidget();
    QVERIFY(canvas != nullptr);
    canvas->setFocus();

    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(220, 180));
    QTest::mouseMove(canvas, QPoint(80, 80));
    QTest::keyClick(canvas, Qt::Key_Escape);

    QScrollBar* vertical = m_widget->getVerticalScrollbar();
    QVERIFY(vertical != nullptr);
    const int afterEscape = vertical->value();
    QTest::mouseMove(canvas, QPoint(20, 20));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QCOMPARE(vertical->value(), afterEscape);
}

QTEST_MAIN(InteractionWidgetTest)

#include "tst_interactionwidgettest.moc"
