#include "editorhost.h"

#include <QSignalSpy>
#include <QtTest>

class ShellKeyboardTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hostExposesAccessibilityHelpers();
    void selectFindingRequiresDocument();
    void preferReducedMotionReadsEnvironment();
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

void ShellKeyboardTest::preferReducedMotionReadsEnvironment()
{
    const QByteArray previous = qgetenv("QT_ACCESSIBILITY_REDUCE_MOTION");
    qputenv("QT_ACCESSIBILITY_REDUCE_MOTION", "1");

    EditorHost host;
    QVERIFY(host.preferReducedMotion());

    qputenv("QT_ACCESSIBILITY_REDUCE_MOTION", previous);
}

QTEST_MAIN(ShellKeyboardTest)
#include "tst_shellkeyboardtest.moc"
