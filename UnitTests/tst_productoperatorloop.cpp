// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "editorhost.h"
#include "inspectormodel.h"
#include "operatoracceptancehelpers.h"
#include "preflightcontroller.h"
#include "previewstatemodel.h"

#include <QtTest>

using pdfinteraction::InspectorModel;
using pdfinteraction::PreflightController;
using pdfinteraction::PreviewStateModel;

namespace
{

pdf::PreflightFinding makeFinding()
{
    pdf::PreflightFinding finding;
    finding.checkId = QStringLiteral("bleed");
    finding.scope = QStringLiteral("page");
    finding.page = 1;
    finding.severity = QStringLiteral("error");
    finding.type = QStringLiteral("bleed");
    finding.message = QStringLiteral("Bleed is insufficient");
    finding.bbox = QRectF(10, 20, 30, 40);
    finding.evidenceIds = { QStringLiteral("evidence-bleed-1") };
    return finding;
}

}   // namespace

class ProductOperatorLoopTest final : public QObject
{
    Q_OBJECT

private slots:
    void openDetectPinpointInspectUnderstandState();
};

void ProductOperatorLoopTest::openDetectPinpointInspectUnderstandState()
{
    const QString pdfPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY2(QFileInfo::exists(pdfPath), pdfPath.toUtf8().constData());

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(pdfPath));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QCOMPARE(host.pageCount(), 1);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 800, 600);
    QCOMPARE(host.currentPage(), 0);

    auto* preflight = qobject_cast<PreflightController*>(host.preflight());
    QVERIFY(preflight);
    const QString documentKey = preflight->documentKey();
    const QString documentRevision = preflight->documentRevision();
    QVERIFY(!documentKey.isEmpty());
    QVERIFY(!documentRevision.isEmpty());

    preflight->beginRun(documentKey, documentRevision, QStringLiteral("profile"), QStringLiteral("job-1"));
    const pdf::PreflightFinding finding = makeFinding();
    pdf::PreflightResult result;
    result.errors = { finding };
    QVERIFY(preflight->acceptResult(QStringLiteral("job-1"), documentRevision, result));
    QCOMPARE(preflight->state(), PreflightController::State::Findings);
    QCOMPARE(host.preflightStateName(), QStringLiteral("findings"));

    const QString findingId = finding.stableId();
    host.selectFinding(findingId);

    auto* inspector = qobject_cast<InspectorModel*>(host.inspector());
    QVERIFY(inspector);
    QCOMPARE(inspector->selectionKind(), InspectorModel::SelectionKind::Finding);
    QCOMPARE(inspector->selectionId(), findingId);
    QVERIFY(inspector->rowCount() > 0);

    auto* preview = qobject_cast<PreviewStateModel*>(host.preview());
    QVERIFY(preview);
    QCOMPARE(preview->authority(), PreviewStateModel::Authority::Approximate);
    QCOMPARE(PreviewStateModel::authorityName(preview->authority()), QStringLiteral("approximate"));
    QVERIFY(!host.previewSummary().isEmpty());
    QVERIFY(!host.inspectorTitle().isEmpty());
    QCOMPARE(host.currentPage(), 0);
}

QTEST_MAIN(ProductOperatorLoopTest)

#include "tst_productoperatorloop.moc"
