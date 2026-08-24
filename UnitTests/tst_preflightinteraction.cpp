#include "preflightcontroller.h"

#include <QSignalSpy>
#include <QtTest>

using pdfinteraction::PreflightController;
using pdfinteraction::PreflightFindingsModel;

namespace
{

pdf::PreflightFinding makeFinding(const QString& checkId,
                                  int page,
                                  const QString& severity,
                                  const QRectF& bbox,
                                  const QStringList& evidenceIds = {})
{
    pdf::PreflightFinding finding;
    finding.checkId = checkId;
    finding.scope = QStringLiteral("page");
    finding.page = page;
    finding.severity = severity;
    finding.type = checkId;
    finding.message = QStringLiteral("%1 finding").arg(checkId);
    finding.bbox = bbox;
    finding.evidenceIds = evidenceIds;
    return finding;
}

pdf::PreflightResult resultWith(const QList<pdf::PreflightFinding>& errors,
                                const QList<pdf::PreflightFinding>& warnings = {})
{
    pdf::PreflightResult result;
    result.errors = errors;
    result.warnings = warnings;
    return result;
}

}   // namespace

class PreflightInteractionTest final : public QObject
{
    Q_OBJECT

private slots:
    void modelRetainsStableIdentityAndFilters();
    void modelSelectionAndOverlayAreRevisionBound();
    void controllerAcceptsCurrentResultAndBuildsNavigation();
    void controllerRejectsStaleAndCancelledResults();
    void controllerRepresentsIncompleteRun();
};

void PreflightInteractionTest::modelRetainsStableIdentityAndFilters()
{
    PreflightFindingsModel model;
    const pdf::PreflightFinding error = makeFinding(QStringLiteral("bleed"), 2, QStringLiteral("error"), QRectF(1, 2, 3, 4), { QStringLiteral("e-1") });
    const pdf::PreflightFinding warning = makeFinding(QStringLiteral("fonts"), 2, QStringLiteral("warning"), QRectF(5, 6, 7, 8));
    model.replace(QStringLiteral("doc"), QStringLiteral("rev-1"), { error }, { warning });

    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(0), PreflightFindingsModel::FindingIdRole).toString(), error.stableId());
    QCOMPARE(model.data(model.index(0), PreflightFindingsModel::EvidenceIdsRole).toStringList(), QStringList{ QStringLiteral("e-1") });
    QCOMPARE(model.filtered(QStringLiteral("error")).size(), 1);
    QCOMPARE(model.filtered({}, QStringLiteral("fonts"), 2).size(), 1);
    QCOMPARE(model.filtered({}, {}, 3).size(), 0);
    QCOMPARE(model.groupCounts().value(QStringLiteral("bleed")), 1);
    QCOMPARE(model.groupCounts(QStringLiteral("error")).value(QStringLiteral("fonts")), 0);
}

void PreflightInteractionTest::modelSelectionAndOverlayAreRevisionBound()
{
    PreflightFindingsModel model;
    const pdf::PreflightFinding finding = makeFinding(QStringLiteral("bleed"), 1, QStringLiteral("error"), QRectF(10, 20, 30, 40));
    model.replace(QStringLiteral("doc"), QStringLiteral("rev-1"), { finding }, {});
    const QString id = finding.stableId();

    model.setSelectedFinding(id);
    QCOMPARE(model.selectedFindingId(), id);
    QCOMPARE(model.overlays(QStringLiteral("rev-1"), 1).size(), 1);
    QVERIFY(model.overlays(QStringLiteral("rev-2"), 1).isEmpty());
    QVERIFY(model.containsCurrent(id, QStringLiteral("rev-1")));
    QVERIFY(!model.containsCurrent(id, QStringLiteral("rev-2")));
}

void PreflightInteractionTest::controllerAcceptsCurrentResultAndBuildsNavigation()
{
    PreflightController controller;
    QSignalSpy states(&controller, &PreflightController::stateChanged);
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), QStringLiteral("profile"), QStringLiteral("job-1"));

    const pdf::PreflightFinding finding = makeFinding(QStringLiteral("bleed"), 3, QStringLiteral("error"), QRectF(1, 2, 3, 4), { QStringLiteral("evidence-1") });
    QVERIFY(controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), resultWith({ finding })));
    QCOMPARE(controller.state(), PreflightController::State::Findings);
    QVERIFY(!states.isEmpty());

    PreflightController::EvidenceNavigationRequest request;
    QVERIFY(controller.navigationFor(finding.stableId(), &request));
    QCOMPARE(request.documentRevision, QStringLiteral("rev-1"));
    QCOMPARE(request.page, 3);
    QCOMPARE(request.evidenceIds, QStringList{ QStringLiteral("evidence-1") });
    QCOMPARE(controller.overlaysForPage(3).size(), 1);
}

void PreflightInteractionTest::controllerRejectsStaleAndCancelledResults()
{
    PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));
    controller.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-2"));
    QVERIFY(!controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), resultWith({})));

    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-2"), {}, QStringLiteral("job-2"));
    QVERIFY(controller.cancelRun(QStringLiteral("job-2")));
    QCOMPARE(controller.state(), PreflightController::State::Cancelled);
    QVERIFY(!controller.acceptResult(QStringLiteral("job-2"), QStringLiteral("rev-2"), resultWith({})));
}

void PreflightInteractionTest::controllerRepresentsIncompleteRun()
{
    PreflightController controller;
    controller.beginRun(QStringLiteral("doc"), QStringLiteral("rev-1"), {}, QStringLiteral("job-1"));
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    QVERIFY(controller.acceptResult(QStringLiteral("job-1"), QStringLiteral("rev-1"), result));
    QCOMPARE(controller.state(), PreflightController::State::Incomplete);
}

QTEST_GUILESS_MAIN(PreflightInteractionTest)
#include "tst_preflightinteraction.moc"
