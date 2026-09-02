#include "focusrestoration.h"
#include "loopcanvasaccessible.h"
#include "loopcanvasitem.h"

#include <QAccessible>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

class QuickAccessibilityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void canvasAccessibleHasNoChildren();
    void focusRestorationRestoresItem();
};

void QuickAccessibilityTest::canvasAccessibleHasNoChildren()
{
    pdfquick::installLoopCanvasAccessibility();

    QQuickWindow window;
    window.resize(320, 240);
    window.show();

    pdfquick::LoopCanvasItem item(window.contentItem());
    item.setWidth(320);
    item.setHeight(240);
    item.setAccessibleDocumentSummary(QStringLiteral("Document canvas. Page 1 of 1. Zoom 100 percent."));

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&item);
    QVERIFY(iface);
    QCOMPARE(iface->role(), QAccessible::Canvas);
    QCOMPARE(iface->childCount(), 0);
    QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Document canvas"));
    QVERIFY(!iface->text(QAccessible::Description).isEmpty());
}

void QuickAccessibilityTest::focusRestorationRestoresItem()
{
    FocusRestoration restoration;
    QQuickWindow window;
    window.resize(320, 240);
    window.show();

    QQuickItem first(window.contentItem());
    QQuickItem second(window.contentItem());
    first.setSize(QSizeF(100, 100));
    second.setSize(QSizeF(100, 100));
    first.setFlag(QQuickItem::ItemIsFocusScope);
    second.setFlag(QQuickItem::ItemIsFocusScope);
    first.setFocus(true);
    QVERIFY(first.hasFocus());

    restoration.remember(&first);
    second.setFocus(true);
    QVERIFY(!first.hasFocus());
    restoration.restore();
    QVERIFY(first.hasFocus());
}

QTEST_MAIN(QuickAccessibilityTest)
#include "tst_quickaccessibilitytest.moc"
