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

// Page/zoom/rotate catalog handlers live in the host-neutral layer. This target
// links LoopLibInteraction, LoopLibCore, Qt6::Core, Qt6::Gui and Qt6::Test,
// and deliberately not Qt6::Widgets or Qt6::Qml. QTEST_GUILESS_MAIN is the
// P4-S7 proof that navigation commands do not need a presentation host.

#include <QtTest>

#include <QHash>
#include <QSignalSpy>

#include <memory>

#include "commandcatalog.h"
#include "documentfacade.h"
#include "documentloader.h"
#include "jobsubmitter.h"
#include "pagesurfacecoordinator.h"
#include "pagesurfacerenderer.h"
#include "viewportcommandbridge.h"
#include "viewportcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfpage.h"

namespace
{

pdf::PDFDocumentPointer buildDocument(int pageCount)
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 100, 100));
    }

    return pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build()));
}

class FakeJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        const QString jobId =
            spec.jobId.isEmpty() ? QStringLiteral("job-%1").arg(++m_sequence) : spec.jobId;
        m_status.insert(jobId, pdf::PDFJobStatus::Queued);

        if (runInline)
        {
            m_status.insert(jobId, pdf::PDFJobStatus::Running);
            auto token = std::make_shared<pdf::PDFJobCancellationToken>();
            pdf::PDFJobContext context(token, pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});
            work(context);
            m_status.insert(jobId, pdf::PDFJobStatus::Succeeded);
        }

        return jobId;
    }

    bool cancel(const QString& jobId) override
    {
        m_status.insert(jobId, pdf::PDFJobStatus::Cancelled);
        return true;
    }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = m_status.value(jobId, pdf::PDFJobStatus::Succeeded);
        return result;
    }

    void publishCurrentRevision(const QString& documentKey, const pdf::PDFRevisionIdentity& revision) override
    {
        published.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override
    {
        published.remove(documentKey);
    }

    bool runInline = true;
    QHash<QString, pdf::PDFRevisionIdentity> published;

private:
    quint64 m_sequence = 0;
    QHash<QString, pdf::PDFJobStatus> m_status;
};

class FakeDocumentLoader final : public pdfinteraction::IDocumentLoader
{
public:
    pdfinteraction::DocumentLoadResult load(const pdfinteraction::DocumentSource&,
                                            pdf::PDFJobContext&) override
    {
        pdfinteraction::DocumentLoadResult result;
        if (fail)
        {
            result.outcome = pdfinteraction::DocumentLoadOutcome::Failed;
            result.typedError = QStringLiteral("test/open-failed");
            return result;
        }

        result.outcome = pdfinteraction::DocumentLoadOutcome::Loaded;
        result.document = buildDocument(pageCount);
        return result;
    }

    int pageCount = 4;
    bool fail = false;
};

class FakeDocumentWriter final : public pdfinteraction::IDocumentWriter
{
public:
    pdfinteraction::DocumentWriteResult write(const pdfinteraction::DocumentSource&,
                                              const pdf::PDFDocument*,
                                              pdf::PDFJobContext&) override
    {
        return { pdfinteraction::DocumentWriteOutcome::Written, QString() };
    }
};

class FakePageSurfaceRenderer final : public pdfinteraction::IPageSurfaceRenderer
{
public:
    pdfinteraction::PageSurfaceResult render(const pdfinteraction::PageSurfaceRequest&,
                                             pdf::PDFJobContext&) override
    {
        pdfinteraction::PageSurfaceResult result;
        result.state = pdfinteraction::SurfaceTerminalState::Failed;
        result.typedError = QStringLiteral("test/unused");
        return result;
    }
};

}   // namespace

class ViewportCommandBridgeTest : public QObject
{
    Q_OBJECT

private slots:
    void commandsStayDisabledUntilADocumentIsReady();
    void nextPreviousStartAndEndMoveTheSinglePageViewport();
    void zoomInOutAndFitChangeViewportZoom();
    void rotateLeftAndRightWalkThePresentationRotation();
    void closeDisablesNavigationAndInvalidatesSurfaces();
    void openingAndErrorKeepNavigationDisabled();
    void replacingADocumentInvalidatesSurfaces();
};

namespace
{

struct Harness
{
    Harness()
    {
        facade = std::make_unique<pdfinteraction::DocumentFacade>(context, submitter, loader, writer, catalog);
        bridge = std::make_unique<pdfinteraction::ViewportCommandBridge>(catalog, *facade, viewport);
        viewport.setPixelPerMM(1.0);
        viewport.setViewportSizePx(QSize(200, 200));
        viewport.setPageLayout(pdfinteraction::PageLayout::SinglePage);
    }

    void openAndBind()
    {
        facade->open(QStringLiteral("/corpus/report.pdf"));
        QTRY_COMPARE(facade->state(), pdfinteraction::DocumentState::Ready);
        geometry = std::make_unique<pdfinteraction::PDFDocumentPageGeometrySource>(&context);
        viewport.setGeometrySource(geometry.get());
    }

    pdf::PDFDocumentContext context{ nullptr };
    FakeJobSubmitter submitter;
    FakeDocumentLoader loader;
    FakeDocumentWriter writer;
    pdfinteraction::CommandCatalog catalog;
    pdfinteraction::ViewportController viewport;
    std::unique_ptr<pdfinteraction::DocumentFacade> facade;
    std::unique_ptr<pdfinteraction::ViewportCommandBridge> bridge;
    std::unique_ptr<pdfinteraction::PDFDocumentPageGeometrySource> geometry;
};

}   // namespace

void ViewportCommandBridgeTest::commandsStayDisabledUntilADocumentIsReady()
{
    Harness harness;
    QVERIFY(harness.bridge->handlersRegistered());
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId));
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::ZoomInCommandId));
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::RotateRightCommandId));

    const pdfinteraction::CommandInvocationId invocation =
        harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId);
    QCOMPARE(invocation, pdfinteraction::InvalidCommandInvocation);
}

void ViewportCommandBridgeTest::nextPreviousStartAndEndMoveTheSinglePageViewport()
{
    Harness harness;
    harness.openAndBind();

    QCOMPARE(harness.viewport.pageCount(), 4);
    QCOMPARE(harness.viewport.currentPage(), 0);
    QVERIFY(harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId));
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::GoToPreviousPageCommandId));

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.currentPage(), 1);

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::GoToDocumentEndCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.currentPage(), 3);
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId));
    QVERIFY(harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::GoToPreviousPageCommandId));

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::GoToPreviousPageCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.currentPage(), 2);

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::GoToDocumentStartCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.currentPage(), 0);
}

void ViewportCommandBridgeTest::zoomInOutAndFitChangeViewportZoom()
{
    Harness harness;
    harness.openAndBind();

    const qreal start = harness.viewport.zoom();
    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::ZoomInCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.zoom(), start * pdfinteraction::ViewportController::ZoomStep);

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::ZoomOutCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.zoom(), start);

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::FitPageCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.zoom(), harness.viewport.zoomHint(pdfinteraction::ZoomHint::Fit));

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::FitWidthCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.zoom(), harness.viewport.zoomHint(pdfinteraction::ZoomHint::FitWidth));

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::FitHeightCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.zoom(), harness.viewport.zoomHint(pdfinteraction::ZoomHint::FitHeight));
}

void ViewportCommandBridgeTest::rotateLeftAndRightWalkThePresentationRotation()
{
    Harness harness;
    harness.openAndBind();
    QCOMPARE(harness.viewport.rotation(), pdf::PageRotation::None);

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::RotateRightCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.rotation(), pdf::PageRotation::Rotate90);

    QVERIFY(harness.catalog.invoke(pdfinteraction::ViewportCommandBridge::RotateLeftCommandId) !=
            pdfinteraction::InvalidCommandInvocation);
    QCOMPARE(harness.viewport.rotation(), pdf::PageRotation::None);
}

void ViewportCommandBridgeTest::closeDisablesNavigationAndInvalidatesSurfaces()
{
    Harness harness;
    harness.openAndBind();

    pdfinteraction::PDFDocumentContextSource revisions(&harness.context);
    FakePageSurfaceRenderer renderer;
    pdfinteraction::PageSurfaceCoordinator coordinator(revisions, harness.submitter, renderer, harness.viewport);
    const quint64 generationBefore = coordinator.generation();
    harness.bridge->setCoordinator(&coordinator);

    QVERIFY(harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::FitPageCommandId));
    harness.facade->close();
    QCOMPARE(harness.facade->state(), pdfinteraction::DocumentState::Empty);

    harness.viewport.setGeometrySource(nullptr);
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId));
    QVERIFY(!harness.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::FitPageCommandId));
    QVERIFY(coordinator.generation() > generationBefore);
}

void ViewportCommandBridgeTest::openingAndErrorKeepNavigationDisabled()
{
    Harness opening;
    opening.submitter.runInline = false;
    opening.facade->open(QStringLiteral("/corpus/report.pdf"));
    QCOMPARE(opening.facade->state(), pdfinteraction::DocumentState::Opening);
    QVERIFY(!opening.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId));
    QCOMPARE(opening.catalog.invoke(pdfinteraction::ViewportCommandBridge::GoToNextPageCommandId),
             pdfinteraction::InvalidCommandInvocation);

    Harness error;
    error.loader.fail = true;
    error.facade->open(QStringLiteral("/corpus/missing.pdf"));
    QTRY_COMPARE(error.facade->state(), pdfinteraction::DocumentState::Error);
    QVERIFY(!error.catalog.isEnabled(pdfinteraction::ViewportCommandBridge::ZoomInCommandId));
    QCOMPARE(error.catalog.invoke(pdfinteraction::ViewportCommandBridge::ZoomInCommandId),
             pdfinteraction::InvalidCommandInvocation);
}

void ViewportCommandBridgeTest::replacingADocumentInvalidatesSurfaces()
{
    Harness harness;
    harness.openAndBind();

    pdfinteraction::PDFDocumentContextSource revisions(&harness.context);
    FakePageSurfaceRenderer renderer;
    pdfinteraction::PageSurfaceCoordinator coordinator(revisions, harness.submitter, renderer, harness.viewport);
    const quint64 generationBefore = coordinator.generation();
    harness.bridge->setCoordinator(&coordinator);

    harness.loader.pageCount = 2;
    harness.facade->open(QStringLiteral("/corpus/other.pdf"));
    QTRY_COMPARE(harness.facade->state(), pdfinteraction::DocumentState::Ready);
    harness.geometry = std::make_unique<pdfinteraction::PDFDocumentPageGeometrySource>(&harness.context);
    harness.viewport.setGeometrySource(harness.geometry.get());

    QVERIFY(coordinator.generation() > generationBefore);
    QCOMPARE(harness.viewport.pageCount(), 2);
}

QTEST_GUILESS_MAIN(ViewportCommandBridgeTest)

#include "tst_viewportcommandbridgetest.moc"
