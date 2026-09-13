#include "inspectormodel.h"
#include "pagesmodel.h"
#include "previewstatemodel.h"
#include "productionmodel.h"

#include <QtTest>
#include <QSignalSpy>

using pdfinteraction::InspectorModel;
using pdfinteraction::PagesModel;
using pdfinteraction::PageView;
using pdfinteraction::PreviewStateModel;
using pdfinteraction::ProductionModel;

namespace
{

pdf::PreflightFinding makeFinding()
{
    pdf::PreflightFinding finding;
    finding.checkId = QStringLiteral("bleed");
    finding.scope = QStringLiteral("page");
    finding.page = 2;
    finding.severity = QStringLiteral("error");
    finding.type = QStringLiteral("bleed");
    finding.message = QStringLiteral("Bleed is insufficient");
    finding.bbox = QRectF(1, 2, 3, 4);
    finding.evidenceIds = { QStringLiteral("evidence-1") };
    return finding;
}

}   // namespace

class P4S9InteractionTest final : public QObject
{
    Q_OBJECT

private slots:
    void inspectorDispatchesFindingContextAndRejectsStaleRevision();
    void inspectorProjectsReportBackedSectionsAndBudgetStatus();
    void inspectorUsesFallbackForUnknownChecksAndRejectsUnrelatedRepair();
    void previewAuthorityIsExplicitAndRevisionBound();
    void pagesModelKeepsSelectionBoundToRevision();
    void productionModelProjectsCoreStepsAndResetsOnRevisionChange();
};

void P4S9InteractionTest::inspectorDispatchesFindingContextAndRejectsStaleRevision()
{
    pdfinteraction::PreflightFindingsModel findings;
    const pdf::PreflightFinding finding = makeFinding();
    findings.replace(QStringLiteral("doc"), QStringLiteral("rev-1"), { finding }, {});

    InspectorModel inspector;
    inspector.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-1"));
    QVERIFY(inspector.setFindingSelection(findings, finding.stableId(), QStringLiteral("rev-1")));
    QCOMPARE(inspector.selectionKind(), InspectorModel::SelectionKind::Finding);
    QCOMPARE(inspector.selectionId(), finding.stableId());
    QVERIFY(inspector.rowCount() >= 8);
    QCOMPARE(inspector.data(inspector.index(0), InspectorModel::ValueRole).toString(), QStringLiteral("error"));
    QVERIFY(!inspector.setFindingSelection(findings, finding.stableId(), QStringLiteral("rev-2")));

    inspector.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-2"));
    QCOMPARE(inspector.rowCount(), 0);
    QCOMPARE(inspector.selectionKind(), InspectorModel::SelectionKind::EmptyCanvas);
}

void P4S9InteractionTest::inspectorProjectsReportBackedSectionsAndBudgetStatus()
{
    pdfinteraction::PreflightFindingsModel findings;
    pdf::PreflightFinding finding = makeFinding();
    finding.objectId = QStringLiteral("image-42");
    finding.evidence = {
        { QStringLiteral("effective_dpi"), 180 },
        { QStringLiteral("native_dpi"), 600 },
        { QStringLiteral("object_context"), QJsonObject{
                                                { QStringLiteral("resource_name"), QStringLiteral("Im42") },
                                                { QStringLiteral("color_space"), QStringLiteral("DeviceRGB") } } }
    };
    findings.replace(QStringLiteral("doc"), QStringLiteral("rev-1"), { finding }, {});

    pdf::PreflightResult report;
    pdf::PreflightCheckStatus status;
    status.id = finding.checkId;
    status.status = QStringLiteral("incomplete");
    status.reason = QStringLiteral("Raster budget exhausted");
    status.budgetKind = QStringLiteral("bytes");
    status.budgetPool = QStringLiteral("preflight");
    status.budgetLimit = 1000;
    status.budgetAttempted = 1200;
    report.checkStatuses = { status };
    pdf::PreflightFixupConfig fixup;
    fixup.id = QStringLiteral("add-bleed");
    fixup.description = QStringLiteral("Extend bleed");
    fixup.params = { { QStringLiteral("mode"), QStringLiteral("mirror") } };
    report.fixupsAvailable = { fixup };
    findings.setReport(report);

    InspectorModel inspector;
    inspector.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-1"));
    QVERIFY(inspector.setFindingSelection(findings, finding.stableId(), QStringLiteral("rev-1")));

    const auto propertyValue = [&inspector](const QString& propertyId)
    {
        for (int row = 0; row < inspector.rowCount(); ++row)
        {
            const QModelIndex index = inspector.index(row);
            if (inspector.data(index, InspectorModel::PropertyIdRole).toString() == propertyId)
            {
                return inspector.data(index, InspectorModel::ValueRole).toString();
            }
        }
        return QString();
    };

    QCOMPARE(propertyValue(QStringLiteral("object")), QStringLiteral("image-42"));
    QCOMPARE(propertyValue(QStringLiteral("bounds")), QStringLiteral("1,2 3x4"));
    QCOMPARE(propertyValue(QStringLiteral("evidence.effective_dpi")), QStringLiteral("180"));
    QCOMPARE(propertyValue(QStringLiteral("object-context.resource_name")), QStringLiteral("Im42"));
    QCOMPARE(propertyValue(QStringLiteral("check-status")), QStringLiteral("incomplete"));
    QCOMPARE(propertyValue(QStringLiteral("check-reason")), QStringLiteral("Raster budget exhausted"));
    QCOMPARE(propertyValue(QStringLiteral("budget-limit")), QStringLiteral("1000"));
    QCOMPARE(propertyValue(QStringLiteral("budget-attempted")), QStringLiteral("1200"));
    QCOMPARE(inspector.data(inspector.index(0), InspectorModel::SectionRole).toString(), QStringLiteral("identity"));

    QSignalSpy repairSpy(&inspector, &InspectorModel::correctiveOperationRequested);
    QVERIFY(inspector.requestCorrectiveOperation(QStringLiteral("add-bleed")));
    QCOMPARE(repairSpy.count(), 1);
    const pdfinteraction::InspectorCorrectiveOperationIntent intent =
        qvariant_cast<pdfinteraction::InspectorCorrectiveOperationIntent>(repairSpy.at(0).at(0));
    QCOMPARE(intent.findingId, finding.stableId());
    QCOMPARE(intent.operationId, QStringLiteral("add-bleed"));
    QCOMPARE(intent.parameters.value(QStringLiteral("mode")).toString(), QStringLiteral("mirror"));
}

void P4S9InteractionTest::inspectorUsesFallbackForUnknownChecksAndRejectsUnrelatedRepair()
{
    pdfinteraction::PreflightFindingsModel findings;
    pdf::PreflightFinding finding = makeFinding();
    finding.checkId = QStringLiteral("future-check");
    finding.type = QStringLiteral("future-defect");
    finding.page = 0;
    finding.bbox = QRectF();
    finding.objectId = QString();
    finding.evidence = { { QStringLiteral("observed"), true },
                         { QStringLiteral("measurements"), QJsonArray{ 1, 2 } } };
    findings.replace(QStringLiteral("doc"), QStringLiteral("rev-1"), { finding }, {});

    pdf::PreflightResult report;
    pdf::PreflightFixupConfig fixup;
    fixup.id = QStringLiteral("add-bleed");
    fixup.description = QStringLiteral("Extend bleed");
    fixup.params = { { QStringLiteral("mode"), QStringLiteral("mirror") } };
    report.fixupsAvailable = { fixup };
    findings.setReport(report);

    InspectorModel inspector;
    inspector.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-1"));
    QVERIFY(inspector.setFindingSelection(findings, finding.stableId(), QStringLiteral("rev-1")));

    QStringList ids;
    for (int row = 0; row < inspector.rowCount(); ++row)
    {
        ids.push_back(inspector.data(inspector.index(row), InspectorModel::PropertyIdRole).toString());
    }
    QVERIFY(ids.contains(QStringLiteral("evidence.observed")));
    QVERIFY(ids.contains(QStringLiteral("evidence.measurements[0]")));
    QVERIFY(!ids.contains(QStringLiteral("bounds")));
    QVERIFY(!ids.contains(QStringLiteral("object")));
    QVERIFY(ids.contains(QStringLiteral("evidence-ids")));
    QVERIFY(!ids.contains(QStringLiteral("check-status")));
    QVERIFY(!ids.contains(QStringLiteral("corrective.add-bleed")));
    QVERIFY(ids.contains(QStringLiteral("corrective-none")));

    QVERIFY(!inspector.requestCorrectiveOperation(QStringLiteral("add-bleed")));
    QVERIFY(!inspector.requestCorrectiveOperation(QStringLiteral("not-advertised")));
    QCOMPARE(inspector.rowCount(), ids.size());
}

void P4S9InteractionTest::previewAuthorityIsExplicitAndRevisionBound()
{
    PreviewStateModel preview;
    preview.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-1"));
    QVERIFY(preview.setState(QStringLiteral("doc"),
                             QStringLiteral("rev-1"),
                             PreviewStateModel::Authority::Approximate,
                             QStringLiteral("Preview uses an approximation"),
                             QStringLiteral("No authoritative output profile was selected")));
    QCOMPARE(preview.status(), PreviewStateModel::Status::Ready);
    QCOMPARE(preview.authority(), PreviewStateModel::Authority::Approximate);
    QCOMPARE(PreviewStateModel::authorityName(preview.authority()), QStringLiteral("approximate"));
    QVERIFY(!preview.setState(QStringLiteral("doc"),
                              QStringLiteral("rev-2"),
                              PreviewStateModel::Authority::Authoritative,
                              QStringLiteral("stale")));

    preview.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-2"));
    QCOMPARE(preview.status(), PreviewStateModel::Status::Stale);
    QCOMPARE(preview.authority(), PreviewStateModel::Authority::None);
    QVERIFY(preview.setIncomplete(QStringLiteral("doc"),
                                  QStringLiteral("rev-2"),
                                  QStringLiteral("Preview is incomplete"),
                                  QStringLiteral("The required profile evidence is unavailable")));
    QCOMPARE(preview.status(), PreviewStateModel::Status::Incomplete);
    QVERIFY(preview.setState(QStringLiteral("doc"),
                             QStringLiteral("rev-2"),
                             PreviewStateModel::Authority::Authoritative,
                             QStringLiteral("Authoritative preview"),
                             {},
                             QStringLiteral("output-profile")));
    QCOMPARE(preview.status(), PreviewStateModel::Status::Ready);
    QCOMPARE(preview.profileIdentity(), QStringLiteral("output-profile"));
}

void P4S9InteractionTest::pagesModelKeepsSelectionBoundToRevision()
{
    PagesModel pages;
    QVector<PageView> pageViews = {
        { QStringLiteral("page-1"), QStringLiteral("Page 1"), QStringLiteral("source.pdf"), 1, QSizeF(595, 842), 0, false, false },
        { QStringLiteral("page-2"), QStringLiteral("Page 2"), QStringLiteral("source.pdf"), 2, QSizeF(595, 842), 90, true, false }
    };
    QVERIFY(pages.replace(QStringLiteral("doc"), QStringLiteral("rev-1"), pageViews));
    QVERIFY(pages.setSelectedPage(QStringLiteral("page-2"), QStringLiteral("doc"), QStringLiteral("rev-1")));
    QCOMPARE(pages.selectedPageId(), QStringLiteral("page-2"));
    QVERIFY(pages.data(pages.index(1), PagesModel::SelectedRole).toBool());
    QVERIFY(!pages.setSelectedPage(QStringLiteral("page-2"), QStringLiteral("other-doc"), QStringLiteral("rev-1")));
    QVERIFY(!pages.setSelectedPage(QStringLiteral("page-2"), QStringLiteral("doc"), QStringLiteral("rev-2")));

    pages.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-2"));
    QCOMPARE(pages.rowCount(), 0);
    QVERIFY(!pages.containsCurrent(QStringLiteral("page-2"), QStringLiteral("doc"), QStringLiteral("rev-1")));
}

void P4S9InteractionTest::productionModelProjectsCoreStepsAndResetsOnRevisionChange()
{
    ProductionModel production;
    pdf::PDFProcessingStep step;
    step.id = QStringLiteral("die-1");
    step.kind = pdf::PDFProcessingStepKind::Cut;
    step.type = pdf::PDFProcessingStepType::CuttingDie;
    step.displayName = QStringLiteral("Cutting die");
    step.spotColorName = QStringLiteral("DieLine");
    step.pageIndices = { 0, 1 };

    QVERIFY(production.replace(QStringLiteral("doc"), QStringLiteral("rev-1"), { step }));
    QCOMPARE(production.rowCount(), 1);
    QCOMPARE(production.data(production.index(0), ProductionModel::KindRole).toString(), QStringLiteral("cut"));
    QCOMPARE(production.data(production.index(0), ProductionModel::TypeRole).toString(), QStringLiteral("cutting-die"));
    QVERIFY(production.setState(QStringLiteral("doc"),
                                QStringLiteral("rev-1"),
                                ProductionModel::State::Ready));
    QCOMPARE(ProductionModel::stateName(production.state()), QStringLiteral("READY"));

    production.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("rev-2"));
    QCOMPARE(production.rowCount(), 0);
    QCOMPARE(production.state(), ProductionModel::State::NotReady);
    QVERIFY(!production.setState(QStringLiteral("doc"),
                                 QStringLiteral("rev-1"),
                                 ProductionModel::State::OutputWritten));
}

QTEST_GUILESS_MAIN(P4S9InteractionTest)
#include "tst_p4s9interaction.moc"
