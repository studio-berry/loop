// MIT License

#include "findingnavigation.h"

#include "interactioncontroller.h"
#include "overlayframe.h"
#include "viewportcontroller.h"

#include <QtTest>

#include <algorithm>

namespace
{

class FakeGeometrySource final : public pdfinteraction::IPageGeometrySource
{
public:
    int pageCount() const override { return 3; }

    QSizeF pageSizeMM(int pageIndex, pdf::PageRotation rotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(rotation);
        return QSizeF(100.0, 100.0);
    }

    QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation rotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(rotation);
        QTransform matrix;
        matrix.translate(deviceRect.left(), deviceRect.bottom());
        matrix.scale(deviceRect.width() / 100.0, -deviceRect.height() / 100.0);
        return matrix;
    }
};

class FakeRevisionSource final : public pdfinteraction::IDocumentRevisionSource
{
public:
    pdf::PDFRevisionIdentity currentRevision() const override { return revision; }
    bool isCurrent(const pdf::PDFRevisionIdentity& candidate) const override { return candidate == revision; }

    pdf::PDFRevisionIdentity revision;
};

pdfinteraction::FindingNavigationRequest requestFor(const QString& checkId,
                                                    const QString& findingId,
                                                    const QString& documentKey,
                                                    const QString& documentRevision)
{
    pdfinteraction::FindingNavigationRequest request;
    request.findingId = findingId;
    request.checkId = checkId;
    request.documentKey = documentKey;
    request.documentRevision = documentRevision;
    request.page = 2;
    return request;
}

pdfinteraction::FindingTargetingCapabilityRegistry registryFor(pdfinteraction::FindingNavigationCapabilities capabilities,
                                                               pdfinteraction::FindingInspectionMode mode = pdfinteraction::FindingInspectionMode::None)
{
    pdfinteraction::FindingTargetingCapabilityRegistry registry;
    pdfinteraction::FindingTargetingCapability capability;
    capability.supportsPageNavigation = capabilities.testFlag(pdfinteraction::FindingNavigationCapability::PageNavigation);
    capability.supportsObjectTargeting = capabilities.testFlag(pdfinteraction::FindingNavigationCapability::ObjectTargeting);
    capability.supportsOverlayEvidence = capabilities.testFlag(pdfinteraction::FindingNavigationCapability::OverlayEvidence);
    capability.inspectionMode = QString::fromLatin1(pdfinteraction::getFindingInspectionModeName(mode));
    registry.registerCapability(QStringLiteral("test-check"), capability);
    return registry;
}

}   // namespace

class FindingNavigationTest final : public QObject
{
    Q_OBJECT

private slots:
    void registryRequiresExplicitCapabilities();
    void pageFallbackRevealsPageWithoutInventingObjectTarget();
    void objectNavigationUsesContextMarginAndStableSelection();
    void evidenceUsesIndependentStateAndModeResetsOnDeselect();
    void staleRequestsClearPresentationAndCannotBecomeCurrent();
};

void FindingNavigationTest::registryRequiresExplicitCapabilities()
{
    const pdfinteraction::FindingTargetingCapabilityRegistry empty;
    QVERIFY(!empty.contains(QStringLiteral("unregistered")));
    QVERIFY(!empty.capabilityFor(QStringLiteral("unregistered")).has_value());

    const pdfinteraction::FindingTargetingCapabilityRegistry defaults =
        pdfinteraction::FindingTargetingCapabilityRegistry::defaultRegistry();
    QVERIFY(defaults.contains(QStringLiteral("image-resolution")));
    QVERIFY(defaults.capabilityFor(QStringLiteral("image-resolution"))->supportsObjectTargeting);
    QVERIFY(defaults.contains(QStringLiteral("bleed")));
    QVERIFY(!defaults.capabilityFor(QStringLiteral("bleed"))->supportsObjectTargeting);
}

void FindingNavigationTest::pageFallbackRevealsPageWithoutInventingObjectTarget()
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = QStringLiteral("doc");
    revision.document.sourceDataHash = QByteArrayLiteral("doc");
    revision.documentRevision = 1;

    FakeRevisionSource revisions;
    revisions.revision = revision;
    FakeGeometrySource geometry;
    pdfinteraction::ViewportController viewport;
    viewport.setPixelPerMM(2.0);
    viewport.setViewportSizePx(QSize(240, 240));
    viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);
    viewport.setGeometrySource(&geometry);

    pdfinteraction::OverlayBuilder overlays(viewport);
    pdfinteraction::HitTestDispatcher hitTest;
    pdfinteraction::InteractionController interaction(revisions, viewport, hitTest, overlays);
    auto registry = registryFor(pdfinteraction::FindingNavigationCapability::PageNavigation);
    pdfinteraction::FindingCanvasNavigator navigator(revisions, viewport, interaction, overlays, std::move(registry));

    const auto request = requestFor(QStringLiteral("test-check"), QStringLiteral("stable-finding"),
                                    QStringLiteral("doc"), revision.toString());
    const pdfinteraction::FindingNavigationResult result = navigator.navigate(request);

    QCOMPARE(result.outcome, pdfinteraction::FindingNavigationOutcome::PageFallback);
    QCOMPARE(result.pageIndex, 1);
    QCOMPARE(viewport.currentPage(), 1);
    QVERIFY(!interaction.state().selected().isValid());
    QVERIFY(navigator.isCurrent(result));
}

void FindingNavigationTest::objectNavigationUsesContextMarginAndStableSelection()
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = QStringLiteral("doc");
    revision.document.sourceDataHash = QByteArrayLiteral("doc");
    revision.documentRevision = 1;
    FakeRevisionSource revisions;
    revisions.revision = revision;
    FakeGeometrySource geometry;
    pdfinteraction::ViewportController viewport;
    viewport.setPixelPerMM(2.0);
    viewport.setViewportSizePx(QSize(240, 240));
    viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);
    viewport.setGeometrySource(&geometry);
    pdfinteraction::OverlayBuilder overlays(viewport);
    pdfinteraction::HitTestDispatcher hitTest;
    pdfinteraction::InteractionController interaction(revisions, viewport, hitTest, overlays);
    auto registry = registryFor(pdfinteraction::FindingNavigationCapability::PageNavigation |
                                    pdfinteraction::FindingNavigationCapability::ObjectTargeting |
                                    pdfinteraction::FindingNavigationCapability::OverlayEvidence,
                                pdfinteraction::FindingInspectionMode::None);
    pdfinteraction::FindingCanvasNavigator navigator(revisions, viewport, interaction, overlays, std::move(registry));

    auto request = requestFor(QStringLiteral("test-check"), QStringLiteral("stable-finding"),
                              QStringLiteral("doc"), revision.toString());
    request.objectId = QStringLiteral("object-7");
    request.pageBounds = QRectF(40.0, 40.0, 10.0, 10.0);
    const pdfinteraction::FindingNavigationResult result = navigator.navigate(request);

    QCOMPARE(result.outcome, pdfinteraction::FindingNavigationOutcome::ObjectTargeted);
    QCOMPARE(interaction.state().selected().id, QStringLiteral("stable-finding"));
    QCOMPARE(interaction.state().selected().pageIndex, 1);
    QCOMPARE(interaction.state().selected().pageBounds, request.pageBounds);
    QVERIFY(viewport.zoom() > 1.0);
}

void FindingNavigationTest::evidenceUsesIndependentStateAndModeResetsOnDeselect()
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = QStringLiteral("doc");
    revision.document.sourceDataHash = QByteArrayLiteral("doc");
    revision.documentRevision = 1;
    FakeRevisionSource revisions;
    revisions.revision = revision;
    FakeGeometrySource geometry;
    pdfinteraction::ViewportController viewport;
    viewport.setPixelPerMM(2.0);
    viewport.setViewportSizePx(QSize(240, 240));
    viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);
    viewport.setGeometrySource(&geometry);
    pdfinteraction::OverlayBuilder overlays(viewport);
    pdfinteraction::HitTestDispatcher hitTest;
    pdfinteraction::InteractionController interaction(revisions, viewport, hitTest, overlays);
    auto registry = registryFor(pdfinteraction::FindingNavigationCapability::PageNavigation |
                                    pdfinteraction::FindingNavigationCapability::OverlayEvidence |
                                    pdfinteraction::FindingNavigationCapability::InspectionMode,
                                pdfinteraction::FindingInspectionMode::Probe);
    pdfinteraction::FindingCanvasNavigator navigator(revisions, viewport, interaction, overlays, std::move(registry));
    QSignalSpy modeRequests(&navigator, &pdfinteraction::FindingCanvasNavigator::inspectionModeRequested);
    QSignalSpy modeResets(&navigator, &pdfinteraction::FindingCanvasNavigator::inspectionModeReset);

    auto request = requestFor(QStringLiteral("test-check"), QStringLiteral("stable-finding"),
                              QStringLiteral("doc"), revision.toString());
    request.pageBounds = QRectF(20.0, 20.0, 20.0, 20.0);
    request.evidenceTargets = { { QStringLiteral("evidence-a"), 2, QRectF(30.0, 30.0, 10.0, 10.0) } };
    const pdfinteraction::FindingNavigationResult result = navigator.navigate(request);

    QCOMPARE(result.outcome, pdfinteraction::FindingNavigationOutcome::PageFallback);
    QCOMPARE(result.inspectionMode, pdfinteraction::FindingInspectionMode::Probe);
    QCOMPARE(modeRequests.size(), 1);
    QCOMPARE(overlays.evidence().size(), 1);
    QCOMPARE(overlays.findings().size(), 0);
    QVERIFY(std::any_of(interaction.overlayFrame().primitives.cbegin(),
                        interaction.overlayFrame().primitives.cend(),
                        [](const pdfinteraction::OverlayPrimitive& primitive)
                        { return primitive.id == QStringLiteral("evidence-a"); }));

    pdfinteraction::RevisionFencedToken token;
    token.generation = 1;
    token.revision = revision;
    const pdfinteraction::OverlayFrame frame = overlays.build(interaction.state(), token);
    QCOMPARE(frame.primitives.size(), 2);
    QCOMPARE(frame.primitives.at(0).id, QStringLiteral("evidence-a"));

    navigator.deselect();
    QCOMPARE(navigator.currentInspectionMode(), pdfinteraction::FindingInspectionMode::None);
    QVERIFY(overlays.evidence().isEmpty());
    QCOMPARE(modeResets.size(), 1);
    QVERIFY(!interaction.state().selected().isValid());
}

void FindingNavigationTest::staleRequestsClearPresentationAndCannotBecomeCurrent()
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = QStringLiteral("doc");
    revision.document.sourceDataHash = QByteArrayLiteral("doc");
    revision.documentRevision = 1;
    FakeRevisionSource revisions;
    revisions.revision = revision;
    FakeGeometrySource geometry;
    pdfinteraction::ViewportController viewport;
    viewport.setPixelPerMM(2.0);
    viewport.setViewportSizePx(QSize(240, 240));
    viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);
    viewport.setGeometrySource(&geometry);
    pdfinteraction::OverlayBuilder overlays(viewport);
    pdfinteraction::HitTestDispatcher hitTest;
    pdfinteraction::InteractionController interaction(revisions, viewport, hitTest, overlays);
    auto registry = registryFor(pdfinteraction::FindingNavigationCapability::PageNavigation |
                                    pdfinteraction::FindingNavigationCapability::OverlayEvidence |
                                    pdfinteraction::FindingNavigationCapability::InspectionMode,
                                pdfinteraction::FindingInspectionMode::Render);
    pdfinteraction::FindingCanvasNavigator navigator(revisions, viewport, interaction, overlays, std::move(registry));

    auto currentRequest = requestFor(QStringLiteral("test-check"), QStringLiteral("stable-finding"),
                                     QStringLiteral("doc"), revision.toString());
    currentRequest.page = 1;
    currentRequest.pageBounds = QRectF(20.0, 20.0, 20.0, 20.0);
    const pdfinteraction::FindingNavigationResult current = navigator.navigate(currentRequest);
    QVERIFY(current.accepted());
    QVERIFY(interaction.state().selected().isValid());

    auto staleRequest = currentRequest;
    staleRequest.documentRevision = QStringLiteral("old-revision");
    const pdfinteraction::FindingNavigationResult stale = navigator.navigate(staleRequest);
    QCOMPARE(stale.outcome, pdfinteraction::FindingNavigationOutcome::Stale);
    QVERIFY(!interaction.state().selected().isValid());
    QVERIFY(overlays.evidence().isEmpty());
    QVERIFY(!navigator.isCurrent(current));

    const int pageBeforeUnknown = viewport.currentPage();
    auto unknownRequest = currentRequest;
    unknownRequest.checkId = QStringLiteral("future-check");
    const pdfinteraction::FindingNavigationResult unknown = navigator.navigate(unknownRequest);
    QCOMPARE(unknown.outcome, pdfinteraction::FindingNavigationOutcome::UnsupportedCheck);
    QCOMPARE(viewport.currentPage(), pageBeforeUnknown);
}

QTEST_GUILESS_MAIN(FindingNavigationTest)
#include "tst_findingnavigationtest.moc"
