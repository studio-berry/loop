#include "editorhost.h"

#include <QSignalSpy>
#include <QtTest>

class ShellKeyboardTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hostExposesAccessibilityHelpers();
    void selectFindingRequiresDocument();
};

void ShellKeyboardTest::hostExposesAccessibilityHelpers()
{
    EditorHost host;
    QVERIFY(host.focusRestoration() != nullptr);
    QVERIFY(host.preflight() != nullptr);
    QVERIFY(host.inspector() != nullptr);
    QVERIFY(host.preview() != nullptr);
    QCOMPARE(host.preflightStateName(), QStringLiteral("not-checked"));
}

void ShellKeyboardTest::selectFindingRequiresDocument()
{
    EditorHost host;
    host.selectFinding(QStringLiteral("missing"));
    QCOMPARE(host.inspectorTitle(), QString());
}

QTEST_MAIN(ShellKeyboardTest)
#include "tst_shellkeyboardtest.moc"
