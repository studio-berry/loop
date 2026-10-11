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
    void canvasAccessibleCarriesTheInteractionReadout();
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

void QuickAccessibilityTest::canvasAccessibleCarriesTheInteractionReadout()
{
    pdfquick::installLoopCanvasAccessibility();

    QQuickWindow window;
    window.resize(320, 240);
    window.show();

    pdfquick::LoopCanvasItem item(window.contentItem());
    item.setWidth(320);
    item.setHeight(240);

    // The host composes the readout into the same summary the canvas exposes:
    // a measurement's production-unit value and a text selection's size are
    // read out, not derived by the screen reader's client.
    item.setAccessibleDocumentSummary(
        QStringLiteral("Document canvas. Page 1 of 1. Zoom 100 percent. Selected 12 character(s). Measurements: 1. Latest: 42.3 mm."));

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&item);
    QVERIFY(iface);
    QCOMPARE(iface->role(), QAccessible::Canvas);

    const QString description = iface->text(QAccessible::Description);
    QVERIFY(description.contains(QStringLiteral("42.3 mm")));
    QVERIFY(description.contains(QStringLiteral("Selected 12 character(s).")));
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
