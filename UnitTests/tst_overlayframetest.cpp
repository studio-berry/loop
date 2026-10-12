// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Architecture invariant I24, overlay half: overlays are an independent pass over
// the page surfaces, with a deterministic z-order and their own invalidation.
//
// As in tst_viewportcontrollertest.cpp, the strongest assertion here is the link
// line in UnitTests/CMakeLists.txt. This target links LoopLibInteraction,
// LoopLibCore, Qt6::Core, Qt6::Gui and Qt6::Test, and deliberately not
// Qt6::Widgets. QTEST_GUILESS_MAIN then proves that the overlay contract holds
// with no QPainter, no QWidget and no scene graph.

#include <QtTest>

#include <memory>

#include "interactionstate.h"
#include "overlaybuilder.h"
#include "overlayframe.h"
#include "viewportcontroller.h"

#include "pdfdocumentcontext.h"

namespace
{

constexpr QSizeF PageSizeMM = QSizeF(100.0, 100.0);
constexpr qreal PixelPerMM = 2.0;
constexpr qreal PageBoxSize = 100.0;

pdf::PDFRevisionIdentity makeRevision(const QString& documentId, quint64 documentRevision = 1)
{
    pdf::PDFRevisionIdentity revision;
    revision.document.documentId = documentId;
    revision.document.sourceDataHash = documentId.toUtf8();
    revision.documentRevision = documentRevision;
    return revision;
}

pdfinteraction::RevisionFencedToken makeToken(quint64 generation = 1, const QString& documentId = QStringLiteral("doc-1"))
{
    pdfinteraction::RevisionFencedToken token;
    token.generation = generation;
    token.revision = makeRevision(documentId);
    return token;
}

class FakeGeometrySource final : public pdfinteraction::IPageGeometrySource
{
public:
    explicit FakeGeometrySource(int pageCount) :
        m_pageCount(pageCount)
    {
    }

    int pageCount() const override { return m_pageCount; }

    QSizeF pageSizeMM(int pageIndex, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);

        const bool transposed = extraRotation == pdf::PageRotation::Rotate90 || extraRotation == pdf::PageRotation::Rotate270;
        return transposed ? PageSizeMM.transposed() : PageSizeMM;
    }

    QTransform pagePointToDeviceMatrix(int pageIndex, const QRectF& deviceRect, pdf::PageRotation extraRotation) const override
    {
        Q_UNUSED(pageIndex);
        Q_UNUSED(extraRotation);

        QTransform matrix;
        matrix.translate(deviceRect.left(), deviceRect.bottom());
        matrix.scale(deviceRect.width() / PageBoxSize, -deviceRect.height() / PageBoxSize);
        return matrix;
    }

private:
    int m_pageCount = 0;
};

pdfinteraction::InteractionTarget makeTarget(pdfinteraction::InteractionTargetKind kind, const QString& id, QRectF bounds, int pageIndex = 0)
{
    pdfinteraction::InteractionTarget target;
    target.kind = kind;
    target.pageIndex = pageIndex;
    target.id = id;
    target.pageBounds = bounds;
    return target;
}

const pdfinteraction::OverlayPrimitive* findPrimitive(const pdfinteraction::OverlayFrame& frame, const QString& id)
{
    for (const pdfinteraction::OverlayPrimitive& primitive : frame.primitives)
    {
        if (primitive.id == id)
        {
            return &primitive;
        }
    }

    return nullptr;
}

}   // namespace

class OverlayFrameTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void zOrderIsDeterministic();
    void tiesBreakByRegistrationSequence();
    void geometryUsesThePageSurfaceTransform();
    void geometrySurvivesPanZoomRotationAndPageChange();
    void offPageGeometryIsClippedNotDropped();
    void invalidGeometryIsNotRenderable();
    void overlappingMarkersAllSurvive();
    void hiddenAndFocusedMarkersAreStateNotMutation();
    void highMarkerCountStaysBounded();
    void invalidTokenYieldsAnEmptyFrame();
    void denyExtraGraphicsSuppressesOverlays();
    void measurementPaintsInTheGuidesBandWithItsReadout();
    void textSelectionPaintsInTheSelectionBandWithoutATarget();
    void annotationMarksPaintUnderFindingsWithTheirOwnSelection();
    void annotationMarksAreBounded();
    void findingAndAnnotationCapsLeaveHeadroomForSteeringPrimitives();

private:
    std::unique_ptr<FakeGeometrySource> m_geometry;
    std::unique_ptr<pdfinteraction::ViewportController> m_viewport;
    pdfinteraction::RenderPresentationPolicy m_policy;
    std::unique_ptr<pdfinteraction::OverlayBuilder> m_overlays;
    pdfinteraction::InteractionState m_state;
};

void OverlayFrameTest::init()
{
    m_policy = {};
    m_geometry = std::make_unique<FakeGeometrySource>(4);

    m_viewport = std::make_unique<pdfinteraction::ViewportController>();
    m_viewport->setGeometrySource(m_geometry.get());
    m_viewport->setPixelPerMM(PixelPerMM);
    m_viewport->setViewportSizePx(QSize(300, 500));
    m_viewport->setPageLayout(pdfinteraction::PageLayout::SinglePage);
    m_viewport->setZoom(1.0);

    m_overlays = std::make_unique<pdfinteraction::OverlayBuilder>(*m_viewport, m_policy);
}

void OverlayFrameTest::cleanup()
{
    m_overlays.reset();
    m_viewport.reset();
    m_geometry.reset();
    m_state.reset(pdfinteraction::InteractionCancelReason::DocumentClosed);
}

void OverlayFrameTest::zOrderIsDeterministic()
{
    m_overlays->setGuides({ makeTarget(pdfinteraction::InteractionTargetKind::PageBox, QStringLiteral("trim"), QRectF(5.0, 5.0, 90.0, 90.0)),
                            makeTarget(pdfinteraction::InteractionTargetKind::Guide, QStringLiteral("guide-1"), QRectF(10.0, 10.0, 30.0, 1.0)) });
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)) });
    m_overlays->setHandles({ makeTarget(pdfinteraction::InteractionTargetKind::DragHandle, QStringLiteral("handle-nw"), QRectF(19.0, 19.0, 2.0, 2.0)) });

    m_state.setSelected(makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)));

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    QVERIFY(frame.isOrdered());
    QCOMPARE(frame.primitives.size(), 5);

    // Page chrome under guides under findings under selection under handles.
    // Reverse of the hit-test precedence, so what is drawn on top is what a
    // click lands on.
    QCOMPARE(frame.primitives.at(0).layer, pdfinteraction::OverlayLayer::PageChrome);
    QCOMPARE(frame.primitives.at(1).layer, pdfinteraction::OverlayLayer::Guides);
    QCOMPARE(frame.primitives.at(2).layer, pdfinteraction::OverlayLayer::Findings);
    QCOMPARE(frame.primitives.at(3).layer, pdfinteraction::OverlayLayer::Selection);
    QCOMPARE(frame.primitives.at(4).layer, pdfinteraction::OverlayLayer::DragHandles);

    QCOMPARE(frame.primitives.at(0).id, QStringLiteral("trim"));
    QCOMPARE(frame.primitives.at(4).id, QStringLiteral("handle-nw"));
    QVERIFY(frame.primitives.at(2).selected);
}

void OverlayFrameTest::annotationMarksPaintUnderFindingsWithTheirOwnSelection()
{
    m_overlays->setAnnotations({ makeTarget(pdfinteraction::InteractionTargetKind::None, QStringLiteral("annotation:8 0 R"), QRectF(10.0, 10.0, 30.0, 20.0)),
                                 makeTarget(pdfinteraction::InteractionTargetKind::None, QStringLiteral("annotation:9 0 R"), QRectF(50.0, 50.0, 30.0, 20.0)) });
    m_overlays->setSelectedAnnotationId(QStringLiteral("annotation:9 0 R"));
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)) });

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    QVERIFY(frame.isOrdered());
    QCOMPARE(frame.primitives.size(), 3);

    // Annotations paint over guides and under the findings analysis band.
    QCOMPARE(frame.primitives.at(0).layer, pdfinteraction::OverlayLayer::Annotations);
    QCOMPARE(frame.primitives.at(1).layer, pdfinteraction::OverlayLayer::Annotations);
    QCOMPARE(frame.primitives.at(2).layer, pdfinteraction::OverlayLayer::Findings);

    // The review list's selected annotation carries the selection treatment;
    // the other mark does not.
    QVERIFY(!frame.primitives.at(0).selected);
    QVERIFY(frame.primitives.at(1).selected);

    // Read-only: an annotation mark keeps no interaction kind, so nothing
    // hit-tests it and no click can be routed to it.
    QCOMPARE(frame.primitives.at(0).target.kind, pdfinteraction::InteractionTargetKind::None);
    QCOMPARE(frame.primitives.at(1).target.kind, pdfinteraction::InteractionTargetKind::None);

    // The interaction selection is independent of the review selection: an
    // interaction selection of a finding does not move the annotation's.
    m_state.setSelected(makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)));
    const pdfinteraction::OverlayFrame selectedFrame = m_overlays->build(m_state, makeToken());
    QVERIFY(selectedFrame.primitives.at(1).selected);
    QVERIFY(selectedFrame.primitives.at(2).selected);
}

void OverlayFrameTest::annotationMarksAreBounded()
{
    pdfinteraction::OverlayBounds bounds = pdfinteraction::OverlayBounds::conservativeDefaults();
    bounds.maxAnnotationPrimitives = 1;
    pdfinteraction::OverlayBuilder bounded(*m_viewport, m_policy, bounds);

    bounded.setAnnotations({ makeTarget(pdfinteraction::InteractionTargetKind::None, QStringLiteral("annotation:a"), QRectF(10.0, 10.0, 20.0, 20.0)),
                             makeTarget(pdfinteraction::InteractionTargetKind::None, QStringLiteral("annotation:b"), QRectF(40.0, 10.0, 20.0, 20.0)),
                             makeTarget(pdfinteraction::InteractionTargetKind::None, QStringLiteral("annotation:c"), QRectF(70.0, 10.0, 20.0, 20.0)) });
    bounded.setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)) });

    const pdfinteraction::OverlayFrame frame = bounded.build(m_state, makeToken());

    // The bound is a bound, not a hint: one annotation mark survives, the other
    // two are counted as dropped, and the finding is never crowded out.
    QCOMPARE(frame.primitives.size(), 2);
    QCOMPARE(frame.droppedPrimitives, 2);
    QCOMPARE(frame.primitives.at(0).layer, pdfinteraction::OverlayLayer::Annotations);
    QCOMPARE(frame.primitives.at(1).layer, pdfinteraction::OverlayLayer::Findings);
}

void OverlayFrameTest::findingAndAnnotationCapsLeaveHeadroomForSteeringPrimitives()
{
    // The defaults face the corpus-scale frame: findings and annotation marks
    // beyond their caps, plus the hover and selection the user is steering. The
    // two marker caps must leave headroom for the steering primitives, because
    // annotations are emitted before them and the total bound is the last
    // resort, not the intended one.
    const pdfinteraction::OverlayBounds bounds = pdfinteraction::OverlayBounds::conservativeDefaults();

    QList<pdfinteraction::InteractionTarget> findings;
    for (int index = 0; index < bounds.maxFindingPrimitives + 100; ++index)
    {
        findings.push_back(makeTarget(pdfinteraction::InteractionTargetKind::Finding,
                                      QStringLiteral("finding-%1").arg(index),
                                      QRectF(qreal(index % 90), qreal((index / 90) % 90), 1.0, 1.0)));
    }

    QList<pdfinteraction::InteractionTarget> annotations;
    for (int index = 0; index < bounds.maxAnnotationPrimitives + 300; ++index)
    {
        annotations.push_back(makeTarget(pdfinteraction::InteractionTargetKind::None,
                                         QStringLiteral("annotation-%1").arg(index),
                                         QRectF(qreal(index % 90), qreal((index / 90) % 90), 1.0, 1.0)));
    }

    m_overlays->setFindings(findings);
    m_overlays->setAnnotations(annotations);
    m_state.setHovered(makeTarget(pdfinteraction::InteractionTargetKind::Finding,
                                  QStringLiteral("hovered"), QRectF(20.0, 20.0, 10.0, 10.0)));
    m_state.setSelected(makeTarget(pdfinteraction::InteractionTargetKind::Finding,
                                   QStringLiteral("selected"), QRectF(30.0, 30.0, 10.0, 10.0)));

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    QVERIFY(frame.primitives.size() <= bounds.maxPrimitives);

    // The steering primitives survive the saturated marker bands.
    QVERIFY(findPrimitive(frame, QStringLiteral("hovered#hover")));
    QVERIFY(findPrimitive(frame, QStringLiteral("selected#selection")));

    // The annotation band is capped at the constant, and the shortfall is
    // reported rather than implied.
    int annotationPrimitives = 0;
    for (const pdfinteraction::OverlayPrimitive& primitive : frame.primitives)
    {
        if (primitive.layer == pdfinteraction::OverlayLayer::Annotations)
        {
            ++annotationPrimitives;
        }
    }
    QCOMPARE(annotationPrimitives, bounds.maxAnnotationPrimitives);
    QCOMPARE(frame.droppedPrimitives, 400);
}

void OverlayFrameTest::tiesBreakByRegistrationSequence()
{
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("second"), QRectF(20.0, 20.0, 20.0, 20.0)),
                              makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("first"), QRectF(20.0, 20.0, 20.0, 20.0)) });

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    QCOMPARE(frame.primitives.size(), 2);

    // Same layer, so the input order decides -- and it decides the same way
    // every frame, which is what issue #143 AC3 asks for.
    QCOMPARE(frame.primitives.at(0).id, QStringLiteral("second"));
    QCOMPARE(frame.primitives.at(1).id, QStringLiteral("first"));
    QVERIFY(frame.primitives.at(0).sequence < frame.primitives.at(1).sequence);
}

void OverlayFrameTest::geometryUsesThePageSurfaceTransform()
{
    const QRectF pageBounds(20.0, 20.0, 20.0, 20.0);
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), pageBounds) });

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());
    QCOMPARE(frame.primitives.size(), 1);

    // Geometry stays in page space. The host maps it with exactly the matrix the
    // page surfaces use; nothing here bakes in a scroll offset.
    QCOMPARE(frame.primitives.constFirst().pageBounds, pageBounds);
    QVERIFY(frame.primitives.constFirst().renderable);

    const QRectF viewportBounds = m_viewport->pagePointToViewportMatrix(0).mapRect(frame.primitives.constFirst().pageBounds);
    QVERIFY(m_viewport->placedPageRect(0).contains(viewportBounds.toRect()));
}

void OverlayFrameTest::geometrySurvivesPanZoomRotationAndPageChange()
{
    const QRectF pageBounds(20.0, 20.0, 20.0, 20.0);
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), pageBounds),
                              makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-b"), pageBounds, 1) });

    const auto boundsFor = [this](const QString& id)
    {
        const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());
        const pdfinteraction::OverlayPrimitive* primitive = findPrimitive(frame, id);
        return primitive ? primitive->pageBounds : QRectF();
    };

    QCOMPARE(boundsFor(QStringLiteral("finding-a")), pageBounds);

    // 400x400 px in a 300x500 viewport: wider than the view, so the pan below
    // actually moves something.
    m_viewport->setZoom(2.0);
    QCOMPARE(boundsFor(QStringLiteral("finding-a")), pageBounds);

    const QPoint panned = m_viewport->scrollByPixels(QPoint(-20, 0));
    QVERIFY(!panned.isNull());
    QCOMPARE(boundsFor(QStringLiteral("finding-a")), pageBounds);

    m_viewport->setRotation(pdf::PageRotation::Rotate90);
    QCOMPARE(boundsFor(QStringLiteral("finding-a")), pageBounds);

    // Page change: page 0's marker leaves the frame and page 1's enters it,
    // with the same page-space geometry.
    m_viewport->setRotation(pdf::PageRotation::None);
    m_viewport->setZoom(1.0);
    m_viewport->setBlockIndex(1);

    QCOMPARE(boundsFor(QStringLiteral("finding-a")), QRectF());
    QCOMPARE(boundsFor(QStringLiteral("finding-b")), pageBounds);
}

void OverlayFrameTest::offPageGeometryIsClippedNotDropped()
{
    // Half on the page, and one entirely outside it.
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("straddling"), QRectF(90.0, 40.0, 30.0, 10.0)),
                              makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("outside"), QRectF(140.0, 40.0, 10.0, 10.0)) });

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    QCOMPARE(frame.primitives.size(), 2);

    const pdfinteraction::OverlayPrimitive* straddling = findPrimitive(frame, QStringLiteral("straddling"));
    QVERIFY(straddling);
    QVERIFY(straddling->renderable);

    // Clipped to the visible part of its page, so it cannot paint over the page
    // next to it.
    QVERIFY(straddling->pageBounds.right() <= 100.5);
    QVERIFY(straddling->pageBounds.left() >= 89.5);

    const pdfinteraction::OverlayPrimitive* outside = findPrimitive(frame, QStringLiteral("outside"));
    QVERIFY(outside);

    // Emitted and counted rather than lost: a caller can say how many markers
    // are out of view instead of reporting fewer markers than exist.
    QVERIFY(!outside->renderable);
    QCOMPARE(frame.unrenderablePrimitives, 1);
}

void OverlayFrameTest::invalidGeometryIsNotRenderable()
{
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("null"), QRectF()),
                              makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("empty"), QRectF(20.0, 20.0, 0.0, 0.0)),
                              makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("good"), QRectF(20.0, 20.0, 10.0, 10.0)) });

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    // Issue #143 AC6: a safe not-renderable state, never a blocked frame.
    QCOMPARE(frame.primitives.size(), 3);
    QCOMPARE(frame.unrenderablePrimitives, 2);
    QVERIFY(!findPrimitive(frame, QStringLiteral("null"))->renderable);
    QVERIFY(!findPrimitive(frame, QStringLiteral("empty"))->renderable);
    QVERIFY(findPrimitive(frame, QStringLiteral("good"))->renderable);
}

void OverlayFrameTest::overlappingMarkersAllSurvive()
{
    QList<pdfinteraction::InteractionTarget> findings;
    for (int index = 0; index < 6; ++index)
    {
        findings.push_back(makeTarget(pdfinteraction::InteractionTargetKind::Finding,
                                      QStringLiteral("finding-%1").arg(index),
                                      QRectF(20.0, 20.0, 20.0 - qreal(index), 20.0 - qreal(index))));
    }

    m_overlays->setFindings(findings);

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    // Overlap is not deduplication. Six coincident markers are six markers.
    QCOMPARE(frame.primitives.size(), 6);
    QCOMPARE(frame.unrenderablePrimitives, 0);
    QVERIFY(frame.isOrdered());
}

void OverlayFrameTest::hiddenAndFocusedMarkersAreStateNotMutation()
{
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("visible"), QRectF(20.0, 20.0, 10.0, 10.0)),
                              makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("hidden"), QRectF(40.0, 20.0, 10.0, 10.0)) });
    m_overlays->setHiddenFindingIds({ QStringLiteral("hidden") });
    m_overlays->setFocusedId(QStringLiteral("visible"));
    m_overlays->setSeverities({ { QStringLiteral("visible"), pdfinteraction::OverlaySeverity::Error } });

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, makeToken());

    QCOMPARE(frame.primitives.size(), 1);
    QCOMPARE(frame.primitives.constFirst().id, QStringLiteral("visible"));
    QVERIFY(frame.primitives.constFirst().focused);
    QCOMPARE(frame.primitives.constFirst().severity, pdfinteraction::OverlaySeverity::Error);

    // Unhiding brings it straight back: hiding is presentation state, so nothing
    // about the finding was destroyed (issue #143 AC4).
    m_overlays->setHiddenFindingIds({});
    QCOMPARE(m_overlays->build(m_state, makeToken()).primitives.size(), 2);
}

void OverlayFrameTest::highMarkerCountStaysBounded()
{
    pdfinteraction::OverlayBounds bounds;
    bounds.maxPrimitives = 64;
    bounds.maxFindingPrimitives = 32;

    pdfinteraction::OverlayBuilder builder(*m_viewport, bounds);

    QList<pdfinteraction::InteractionTarget> findings;
    for (int index = 0; index < 5000; ++index)
    {
        findings.push_back(makeTarget(pdfinteraction::InteractionTargetKind::Finding,
                                      QStringLiteral("finding-%1").arg(index),
                                      QRectF(qreal(index % 90), qreal((index / 90) % 90), 1.0, 1.0)));
    }

    builder.setFindings(findings);

    const pdfinteraction::OverlayFrame frame = builder.build(m_state, makeToken());

    // A pre-registered bound, not a discovered one. The shortfall is reported
    // rather than implied.
    QCOMPARE(frame.primitives.size(), 32);
    QCOMPARE(frame.droppedPrimitives, 5000 - 32);
    QVERIFY(frame.isOrdered());
}

void OverlayFrameTest::denyExtraGraphicsSuppressesOverlays()
{
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)) });
    m_overlays->setGuides({ makeTarget(pdfinteraction::InteractionTargetKind::Guide, QStringLiteral("guide-a"), QRectF(5.0, 5.0, 90.0, 90.0)) });
    m_state.setSelected(makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)));
    m_policy.features |= pdf::PDFRenderer::DenyExtraGraphics;

    const pdfinteraction::OverlayFrame suppressed = m_overlays->build(m_state, makeToken());
    QVERIFY(suppressed.primitives.isEmpty());

    m_policy.features &= ~pdf::PDFRenderer::DenyExtraGraphics;
    const pdfinteraction::OverlayFrame restored = m_overlays->build(m_state, makeToken());
    QVERIFY(restored.primitives.size() >= 2);
}

void OverlayFrameTest::invalidTokenYieldsAnEmptyFrame()
{
    m_overlays->setFindings({ makeTarget(pdfinteraction::InteractionTargetKind::Finding, QStringLiteral("finding-a"), QRectF(20.0, 20.0, 20.0, 20.0)) });

    pdfinteraction::RevisionFencedToken empty;
    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, empty);

    // A frame for a document state nobody holds is not worth drawing, and
    // drawing it is how a stale marker survives a reload.
    QVERIFY(frame.isEmpty());
    QVERIFY(!frame.token.isValid());
}

void OverlayFrameTest::measurementPaintsInTheGuidesBandWithItsReadout()
{
    const pdfinteraction::RevisionFencedToken token = makeToken();

    QVERIFY(m_state.begin(pdfinteraction::InteractionKind::Measure, token));

    const pdfinteraction::Measurement draft =
        pdfinteraction::measureBetween(0, QPointF(10.0, 10.0), QPointF(40.0, 50.0), pdf::PDF_POINT_TO_MM);
    QVERIFY(m_state.setMeasurementDraft(draft, token));

    pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, token);
    QVERIFY(frame.isOrdered());
    QCOMPARE(frame.primitives.size(), 1);

    const pdfinteraction::OverlayPrimitive* primitive = findPrimitive(frame, QStringLiteral("measurement#draft"));
    QVERIFY(primitive);
    QCOMPARE(primitive->layer, pdfinteraction::OverlayLayer::Guides);
    QCOMPARE(primitive->kind, pdfinteraction::OverlayPrimitiveKind::Measurement);
    QCOMPARE(primitive->label, pdfinteraction::measurementReadout(draft));
    QCOMPARE(primitive->pagePolygon.size(), 2);
    QVERIFY(primitive->renderable);

    // Completing it moves the primitive to its stable completed id.
    QVERIFY(m_state.completeMeasurement(token).has_value());

    frame = m_overlays->build(m_state, token);
    QVERIFY(findPrimitive(frame, QStringLiteral("measurement#0")));
    QVERIFY(!findPrimitive(frame, QStringLiteral("measurement#draft")));
    QCOMPARE(frame.primitives.size(), 1);
}

void OverlayFrameTest::textSelectionPaintsInTheSelectionBandWithoutATarget()
{
    const pdfinteraction::RevisionFencedToken token = makeToken();

    pdfinteraction::TextSelection selection;
    selection.kind = pdfinteraction::TextSelectionKind::Text;
    selection.pageIndex = 0;
    selection.available = true;
    selection.text = QStringLiteral("two lines");
    selection.geometry = { QRectF(10.0, 10.0, 40.0, 8.0), QRectF(10.0, 20.0, 40.0, 8.0) };
    QVERIFY(m_state.setTextSelection(selection, token));

    const pdfinteraction::OverlayFrame frame = m_overlays->build(m_state, token);
    QVERIFY(frame.isOrdered());
    QCOMPARE(frame.primitives.size(), 2);

    const pdfinteraction::OverlayPrimitive* first = findPrimitive(frame, QStringLiteral("text-selection#0"));
    const pdfinteraction::OverlayPrimitive* second = findPrimitive(frame, QStringLiteral("text-selection#1"));
    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(first->layer, pdfinteraction::OverlayLayer::Selection);
    QCOMPARE(first->kind, pdfinteraction::OverlayPrimitiveKind::Rectangle);
    QVERIFY(first->renderable);

    // There is no text target kind, so a selection rectangle is not a routed
    // click target.
    QVERIFY(!first->target.isValid());

    // An unavailable selection -- a page with no text layer -- has no geometry
    // to paint.
    pdfinteraction::TextSelection unavailable;
    unavailable.pageIndex = 0;
    unavailable.available = false;
    unavailable.unavailableReason = QStringLiteral("This page has no text layer.");
    QVERIFY(m_state.setTextSelection(unavailable, token));
    QVERIFY(m_overlays->build(m_state, token).isEmpty());
}

QTEST_GUILESS_MAIN(OverlayFrameTest)

#include "tst_overlayframetest.moc"
