#include "editorhost.h"
#include "preflightcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"

#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>
#include <QUrl>

class ShellKeyboardTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hostExposesAccessibilityHelpers();
    void selectFindingRequiresDocument();
    void preferReducedMotionReadsEnvironment();
    void findingKeyboardNavigationMovesSelection();
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

void ShellKeyboardTest::findingKeyboardNavigationMovesSelection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString path = directory.filePath(QStringLiteral("keyboard-nav.pdf"));
    QVERIFY(writer.write(path, &document, true));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY(host.hasDocument());

    auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    QVERIFY(preflight);
    pdf::PreflightFinding first;
    first.checkId = QStringLiteral("bleed");
    first.scope = QStringLiteral("page");
    first.page = 1;
    first.severity = QStringLiteral("error");
    first.type = QStringLiteral("bleed");
    first.message = QStringLiteral("First bleed finding");
    first.bbox = QRectF(1, 2, 3, 4);
    pdf::PreflightFinding second = first;
    second.message = QStringLiteral("Second bleed finding");
    second.bbox = QRectF(5, 6, 7, 8);

    preflight->beginRun(preflight->documentKey(), preflight->documentRevision(), QStringLiteral("profile"), QStringLiteral("job-1"));
    pdf::PreflightResult result;
    result.errors = { first, second };
    QVERIFY(preflight->acceptResult(QStringLiteral("job-1"), preflight->documentRevision(), result));

    host.selectFinding(first.stableId());
    QCOMPARE(preflight->findingsModel()->selectedFindingId(), first.stableId());
    QVERIFY(host.selectNextFinding());
    QCOMPARE(preflight->findingsModel()->selectedFindingId(), second.stableId());
    QVERIFY(host.selectPreviousFinding());
    QCOMPARE(preflight->findingsModel()->selectedFindingId(), first.stableId());
}

QTEST_MAIN(ShellKeyboardTest)
#include "tst_shellkeyboardtest.moc"
