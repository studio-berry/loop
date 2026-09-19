#include "editorhost.h"

#include "inspectormodel.h"
#include "interactioncontroller.h"
#include "interactiontarget.h"
#include "preflightcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

class ShellInspectorDispatchTest : public QObject
{
    Q_OBJECT

private slots:
    void selectionKindsDispatchToInspectorModel();
    void findingSelectionDispatchesThroughInspectorAndNavigation();
    void staleFindingCannotSelectInspectorOrCanvas();
    void unknownSelectionFallsBackToEmptyCanvas();
};

void ShellInspectorDispatchTest::selectionKindsDispatchToInspectorModel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString path = directory.filePath(QStringLiteral("dispatch.pdf"));
    QVERIFY(writer.write(path, &document, true));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY(host.hasDocument());

    auto* inspector = qobject_cast<pdfinteraction::InspectorModel*>(host.inspector());
    auto* interaction = host.sessionForTest()->interaction();
    QVERIFY(inspector != nullptr);
    QVERIFY(interaction != nullptr);

    pdfinteraction::InteractionTarget pageTarget;
    pageTarget.kind = pdfinteraction::InteractionTargetKind::Page;
    pageTarget.pageIndex = 0;
    pageTarget.id = QStringLiteral("page-0");
    pageTarget.pageBounds = QRectF(0, 0, 612, 792);
    interaction->selectTarget(pageTarget);
    QTRY_COMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::Page);

    pdfinteraction::InteractionTarget imageTarget;
    imageTarget.kind = pdfinteraction::InteractionTargetKind::Page;
    imageTarget.pageIndex = 0;
    imageTarget.id = QStringLiteral("image:42");
    imageTarget.pageBounds = QRectF(10, 10, 40, 40);
    interaction->selectTarget(imageTarget);
    QTRY_COMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::Image);

    pdfinteraction::InteractionTarget separationTarget;
    separationTarget.kind = pdfinteraction::InteractionTargetKind::Page;
    separationTarget.pageIndex = 0;
    separationTarget.id = QStringLiteral("separation:Cyan");
    separationTarget.pageBounds = QRectF(20, 20, 40, 40);
    interaction->selectTarget(separationTarget);
    QTRY_COMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::Separation);

    pdfinteraction::InteractionTarget pageBoxTarget;
    pageBoxTarget.kind = pdfinteraction::InteractionTargetKind::PageBox;
    pageBoxTarget.pageIndex = 0;
    pageBoxTarget.id = QStringLiteral("trim");
    pageBoxTarget.pageBounds = QRectF(0, 0, 612, 792);
    interaction->selectTarget(pageBoxTarget);
    QTRY_COMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::Page);
}

void ShellInspectorDispatchTest::findingSelectionDispatchesThroughInspectorAndNavigation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString path = directory.filePath(QStringLiteral("finding-dispatch.pdf"));
    QVERIFY(writer.write(path, &document, true));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY(host.hasDocument());
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);

    auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    auto* inspector = qobject_cast<pdfinteraction::InspectorModel*>(host.inspector());
    auto* interaction = host.sessionForTest()->interaction();
    QVERIFY(preflight);
    QVERIFY(inspector);
    QVERIFY(interaction);

    pdf::PreflightFinding finding;
    finding.checkId = QStringLiteral("bleed");
    finding.scope = QStringLiteral("page");
    finding.page = 2;
    finding.severity = QStringLiteral("error");
    finding.type = QStringLiteral("bleed");
    finding.message = QStringLiteral("Bleed is insufficient");
    finding.bbox = QRectF(1, 2, 3, 4);
    const QString findingId = finding.stableId();

    preflight->beginRun(preflight->documentKey(), preflight->documentRevision(), QStringLiteral("profile"), QStringLiteral("job-1"));
    pdf::PreflightResult result;
    result.errors = { finding };
    QVERIFY(preflight->acceptResult(QStringLiteral("job-1"), preflight->documentRevision(), result));

    host.selectFinding(findingId);
    QTRY_COMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::Finding);
    QCOMPARE(inspector->selectionId(), findingId);
    QCOMPARE(host.currentPage(), 1);
    QCOMPARE(preflight->findingsModel()->selectedFindingId(), findingId);

    pdfinteraction::InteractionTarget overlayTarget;
    overlayTarget.kind = pdfinteraction::InteractionTargetKind::Finding;
    overlayTarget.pageIndex = 1;
    overlayTarget.id = findingId;
    overlayTarget.pageBounds = finding.bbox;
    interaction->selectTarget(overlayTarget);
    QTRY_COMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::Finding);
    QCOMPARE(preflight->findingsModel()->selectedFindingId(), findingId);
}

void ShellInspectorDispatchTest::staleFindingCannotSelectInspectorOrCanvas()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString path = directory.filePath(QStringLiteral("stale-finding.pdf"));
    QVERIFY(writer.write(path, &document, true));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = file.readAll();
    file.close();

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY(host.hasDocument());
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);

    auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    auto* inspector = qobject_cast<pdfinteraction::InspectorModel*>(host.inspector());
    auto* overlays = host.sessionForTest()->overlays();
    QVERIFY(preflight);
    QVERIFY(inspector);
    QVERIFY(overlays);

    pdf::PreflightFinding finding;
    finding.checkId = QStringLiteral("bleed");
    finding.scope = QStringLiteral("page");
    finding.page = 2;
    finding.severity = QStringLiteral("error");
    finding.type = QStringLiteral("bleed");
    finding.message = QStringLiteral("Bleed is insufficient");
    finding.bbox = QRectF(12, 18, 20, 22);
    const QString findingId = finding.stableId();
    pdf::PreflightResult report;
    report.errors = { finding };

    preflight->beginRun(preflight->documentKey(), preflight->documentRevision(),
                        QStringLiteral("profile-a"), QStringLiteral("job-1"));
    QVERIFY(preflight->acceptResult(QStringLiteral("job-1"), preflight->documentRevision(), report));
    host.selectFinding(findingId);
    QCOMPARE(host.currentPage(), 1);
    QCOMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::Finding);
    QCOMPARE(preflight->findingsModel()->selectedFindingId(), findingId);
    QCOMPARE(overlays->findings().size(), 1);

    preflight->markProfileStale();
    QCOMPARE(preflight->state(), pdfinteraction::PreflightController::State::Stale);
    QCOMPARE(preflight->findingsModel()->rowCount(), 1);
    QVERIFY(overlays->findings().isEmpty());
    QVERIFY(preflight->findingsModel()->selectedFindingId().isEmpty());
    QVERIFY(inspector->selectionKind() != pdfinteraction::InspectorModel::SelectionKind::Finding);

    host.goToPage(0);
    host.selectFinding(findingId);
    QCOMPARE(host.currentPage(), 0);
    QVERIFY(inspector->selectionKind() != pdfinteraction::InspectorModel::SelectionKind::Finding);
    QVERIFY(overlays->findings().isEmpty());

    preflight->beginRun(preflight->documentKey(), preflight->documentRevision(),
                        QStringLiteral("profile-b"), QStringLiteral("job-2"));
    QVERIFY(preflight->acceptResult(QStringLiteral("job-2"), preflight->documentRevision(), report));
    host.selectFinding(findingId);
    QCOMPARE(host.currentPage(), 1);
    QCOMPARE(inspector->selectionId(), findingId);
    QCOMPARE(overlays->findings().size(), 1);

    pdf::PreflightFinding unknown = finding;
    unknown.checkId = QStringLiteral("future-check");
    unknown.type = QStringLiteral("future-check");
    unknown.objectId = QStringLiteral("67");
    report.errors = { unknown };
    preflight->beginRun(preflight->documentKey(), preflight->documentRevision(),
                        QStringLiteral("profile-b"), QStringLiteral("job-3"));
    QVERIFY(preflight->acceptResult(QStringLiteral("job-3"), preflight->documentRevision(), report));
    host.selectFinding(unknown.stableId());
    QCOMPARE(host.currentPage(), 1);
    QCOMPARE(inspector->selectionId(), unknown.stableId());
    QVERIFY(overlays->findings().isEmpty());
    bool targetingUnavailable = false;
    for (int row = 0; row < inspector->rowCount(); ++row)
    {
        const QModelIndex property = inspector->index(row);
        if (inspector->data(property, pdfinteraction::InspectorModel::PropertyIdRole).toString() ==
            QStringLiteral("targeting"))
        {
            QCOMPARE(inspector->data(property, pdfinteraction::InspectorModel::ValueRole).toString(),
                     QStringLiteral("Page navigation only; object targeting is unsupported for this check."));
            targetingUnavailable = true;
        }
    }
    QVERIFY(targetingUnavailable);

    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), originalBytes);
}

void ShellInspectorDispatchTest::unknownSelectionFallsBackToEmptyCanvas()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString path = directory.filePath(QStringLiteral("empty-canvas.pdf"));
    QVERIFY(writer.write(path, &document, true));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY(host.hasDocument());

    auto* inspector = qobject_cast<pdfinteraction::InspectorModel*>(host.inspector());
    auto* interaction = host.sessionForTest()->interaction();
    QVERIFY(inspector != nullptr);
    QVERIFY(interaction != nullptr);

    pdfinteraction::InteractionTarget guideTarget;
    guideTarget.kind = pdfinteraction::InteractionTargetKind::Guide;
    guideTarget.pageIndex = 0;
    guideTarget.id = QStringLiteral("guide-1");
    guideTarget.pageBounds = QRectF(5, 5, 10, 10);
    interaction->selectTarget(guideTarget);
    QTRY_COMPARE(inspector->selectionKind(), pdfinteraction::InspectorModel::SelectionKind::EmptyCanvas);
}

QTEST_MAIN(ShellInspectorDispatchTest)
#include "tst_shellinspectordispatch.moc"
