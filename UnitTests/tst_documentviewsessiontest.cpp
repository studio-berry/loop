// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "documentviewsession.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdfpagecachebudget.h"

#include "interactioncontroller.h"
#include "overlaybuilder.h"
#include "pdfdocumentimageextractor.h"
#include "pdfexception.h"
#include "pdfrenderer.h"

#include <QtTest>

namespace
{

/// A page whose /UserUnit is 2 and whose own /Rotate is 90. Both are the
/// fixture facts issue #238 asks a measurement to respect; the page is built by
/// hand so the test needs no golden file.
pdf::PDFDocument makeRotatedUserUnitDocument()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 400, 200));
    builder.setPageRotation(page, pdf::PageRotation::Rotate90);
    builder.setPageUserUnit(page, 2.0);
    return builder.build();
}

/// A page carrying one 2x2 RGB image XObject and no text at all: the fixture
/// for "a page with no text layer" and for image extraction.
pdf::PDFDocument makeImageOnlyDocument()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));

    QByteArray imageData;
    for (int pixel = 0; pixel < 4; ++pixel)
    {
        imageData.append('\xff');
        imageData.append('\0');
        imageData.append('\0');
    }

    pdf::PDFDictionary imageDictionary;
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Image"));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Width"), pdf::PDFObject::createInteger(2));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Height"), pdf::PDFObject::createInteger(2));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"), pdf::PDFObject::createName("DeviceRGB"));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString("BitsPerComponent"), pdf::PDFObject::createInteger(8));
    imageDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH), pdf::PDFObject::createInteger(imageData.size()));
    pdf::PDFStream imageStream(std::move(imageDictionary), std::move(imageData));
    const pdf::PDFObjectReference imageReference =
        builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(imageStream)));

    const QByteArray content = "q 40 0 0 40 80 80 cm /Im0 Do Q";

    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(content.size()));
    pdf::PDFStream contentStream(std::move(contentDictionary), QByteArray(content));
    const pdf::PDFObjectReference contentReference =
        builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(contentStream)));

    pdf::PDFDictionary xObjects;
    xObjects.addEntry(pdf::PDFInplaceOrMemoryString("Im0"), pdf::PDFObject::createReference(imageReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObjects))));

    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));

    return builder.build();
}

/// A two-column text page with no painter and no GUI: four text-showing
/// operators in a hand-built content stream. The columns are far apart, so a
/// row-major order would interleave them and a reading order would not.
pdf::PDFDocument makeTwoColumnTextDocument()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 400, 300));

    pdf::PDFDictionary fontDictionary;
    fontDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("Font"));
    fontDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Type1"));
    fontDictionary.addEntry(pdf::PDFInplaceOrMemoryString("BaseFont"), pdf::PDFObject::createName("Helvetica"));
    const pdf::PDFObjectReference font =
        builder.addObject(pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(fontDictionary))));

    QByteArray content;
    const auto appendLine = [&content](const char* text, int x, int y)
    {
        content += "BT /F1 12 Tf 1 0 0 1 " + QByteArray::number(x) + " " + QByteArray::number(y) + " Tm (" + text + ") Tj ET\n";
    };
    appendLine("LEFT ONE", 40, 240);
    appendLine("LEFT TWO", 40, 220);
    appendLine("RIGHT ONE", 240, 240);
    appendLine("RIGHT TWO", 240, 220);

    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(content.size()));
    pdf::PDFStream contentStream(std::move(contentDictionary), std::move(content));
    const pdf::PDFObjectReference contentReference =
        builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(contentStream)));

    pdf::PDFDictionary fonts;
    fonts.addEntry(pdf::PDFInplaceOrMemoryString("F1"), pdf::PDFObject::createReference(font));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("Font"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(fonts))));

    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));

    return builder.build();
}

pdfinteraction::PointerIntent makePointer(pdfinteraction::PointerAction action,
                                          QPoint positionPx,
                                          quint64 sequence,
                                          Qt::MouseButton button = Qt::NoButton,
                                          Qt::MouseButtons buttons = Qt::NoButton)
{
    pdfinteraction::PointerIntent intent;
    intent.stamp.sequence = sequence;
    intent.stamp.monotonicNs = qint64(sequence) * 1000000;
    intent.action = action;
    intent.positionPx = positionPx;
    intent.button = button;
    intent.buttons = buttons;
    return intent;
}

/// A viewport large enough for one page at the injected density, so a test
/// never depends on the host monitor.
void configureViewport(pdfinteraction::ViewportController& viewport)
{
    viewport.setPixelPerMM(4.0);
    viewport.setDevicePixelRatio(1.0);
    viewport.setViewportSizePx(QSize(1000, 1000));
    viewport.setZoom(1.0);
}

}   // namespace

class DocumentViewSessionTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultCacheLimitPartitions();
    void cacheLimitNormalizesAndReappliesAfterDocumentReplacement();
    void denyExtraGraphicsSyncsFromRenderFeatures();
    void measurementRespectsUserUnitAndPageRotation();
    void copiedTextFollowsReadingOrderOnAMultiColumnPage();
    void selectionOnAPageWithNoTextLayerReportsThatState();
    void revisionChangeDropsMeasurementsAndTextSelection();
    void imageExtractionReportsThePagesImages();
};

void DocumentViewSessionTest::defaultCacheLimitPartitions()
{
    DocumentViewSession session;
    pdf::PDFDocumentSession* documentSession = session.context().getSession();
    QVERIFY(documentSession != nullptr);
    QVERIFY(session.surfaces() != nullptr);

    const qsizetype total = DocumentViewSession::DefaultCacheLimit;
    QCOMPARE(session.cacheLimit(), total);
    QCOMPARE(documentSession->cacheLimit(), total);
    QCOMPARE(documentSession->compiledCacheByteLimit(), pdf::PDFPageCacheBudget::compiledPages(total));
    QCOMPARE(session.surfaces()->cacheLimit(), total);
    QCOMPARE(session.surfaces()->bounds().maxAdmittedBytes,
             static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(total)));
}

void DocumentViewSessionTest::cacheLimitNormalizesAndReappliesAfterDocumentReplacement()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    DocumentViewSession session;
    session.setCacheLimit(101);

    auto assertPartitions = [&session](qsizetype total)
    {
        pdf::PDFDocumentSession* documentSession = session.context().getSession();
        QVERIFY(documentSession != nullptr);
        QCOMPARE(session.cacheLimit(), total);
        QCOMPARE(documentSession->cacheLimit(), total);
        QCOMPARE(documentSession->compiledCacheByteLimit(), pdf::PDFPageCacheBudget::compiledPages(total));
        QCOMPARE(session.surfaces()->cacheLimit(), total);
        QCOMPARE(session.surfaces()->bounds().maxAdmittedBytes,
                 static_cast<qint64>(pdf::PDFPageCacheBudget::pageSurfaces(total)));
    };

    assertPartitions(101);

    session.setCacheLimit(-1);
    assertPartitions(0);

    session.setCacheLimit(101);
    session.context().setDocument(&document);
    session.prepareDocumentView();
    assertPartitions(101);
}

void DocumentViewSessionTest::denyExtraGraphicsSyncsFromRenderFeatures()
{
    DocumentViewSession session;
    pdf::PDFRenderer::Features features = pdf::PDFRenderer::getDefaultFeatures();
    features |= pdf::PDFRenderer::DenyExtraGraphics;
    session.setSurfaceRenderFeatures(features);
    QVERIFY(session.overlays()->denyExtraGraphics());

    features &= ~pdf::PDFRenderer::DenyExtraGraphics;
    session.setSurfaceRenderFeatures(features);
    QVERIFY(!session.overlays()->denyExtraGraphics());
}

void DocumentViewSessionTest::measurementRespectsUserUnitAndPageRotation()
{
    pdf::PDFDocument document = makeRotatedUserUnitDocument();

    DocumentViewSession session;
    session.context().setDocument(&document);
    session.prepareDocumentView();
    configureViewport(session.viewport());

    pdfinteraction::InteractionController* interaction = session.interaction();
    QVERIFY(interaction != nullptr);
    interaction->setActiveTool(pdfinteraction::InteractionTool::Measure);

    const QPointF startPagePoint(50.0, 50.0);
    const QPointF endPagePoint(150.0, 150.0);

    const QPoint fromPx = session.viewport().pagePointToViewportMatrix(0).map(startPagePoint).toPoint();
    const QPoint toPx = session.viewport().pagePointToViewportMatrix(0).map(endPagePoint).toPoint();

    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Press, fromPx, 1, Qt::LeftButton, Qt::LeftButton));
    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Move, toPx, 2, Qt::NoButton, Qt::LeftButton));
    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Release, toPx, 3, Qt::LeftButton));

    QCOMPARE(interaction->state().measurements().size(), 1);
    const pdfinteraction::Measurement measurement = interaction->state().measurements().constFirst();

    // The reference is the retired dimensions plugin's own formula -- the
    // page-space length times /UserUnit, converted to millimetres -- within the
    // tolerance the pointer round trip through integer pixels costs.
    const qreal referenceMM = std::hypot(100.0, 100.0) * pdf::PDF_POINT_TO_MM * 2.0;
    QVERIFY(qAbs(measurement.distanceMM - referenceMM) <= 1.0);
    QCOMPARE(measurement.pointToMM, pdf::PDF_POINT_TO_MM * 2.0);

    // The same page-space segment under the opposite viewer rotation measures
    // the same: a page-space segment is rotation invariant, and the controller
    // resolves the points through the viewport's own transforms.
    session.viewport().setRotation(pdf::PageRotation::Rotate270);
    const QPoint fromRotated = session.viewport().pagePointToViewportMatrix(0).map(startPagePoint).toPoint();
    const QPoint toRotated = session.viewport().pagePointToViewportMatrix(0).map(endPagePoint).toPoint();

    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Press, fromRotated, 4, Qt::LeftButton, Qt::LeftButton));
    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Move, toRotated, 5, Qt::NoButton, Qt::LeftButton));
    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Release, toRotated, 6, Qt::LeftButton));

    QCOMPARE(interaction->state().measurements().size(), 2);
    const pdfinteraction::Measurement rotated = interaction->state().measurements().constLast();
    QVERIFY(qAbs(rotated.distanceMM - measurement.distanceMM) <= 1.0);
}

void DocumentViewSessionTest::copiedTextFollowsReadingOrderOnAMultiColumnPage()
{
    pdf::PDFDocument document = makeTwoColumnTextDocument();

    DocumentViewSession session;
    session.context().setDocument(&document);
    session.prepareDocumentView();
    configureViewport(session.viewport());

    pdfinteraction::InteractionController* interaction = session.interaction();
    QVERIFY(interaction != nullptr);

    QVERIFY(interaction->selectAllText());

    const std::optional<pdfinteraction::TextSelection>& selection = interaction->state().textSelection();
    QVERIFY(selection.has_value());
    QVERIFY(selection->available);
    QVERIFY(selection->isValid());
    QVERIFY(!selection->geometry.isEmpty());

    // Column-major reading order, as Core's text layer orders the blocks: every
    // line of the left column first, then the right column's. A row-major order
    // would interleave them ("LEFT ONE", "RIGHT ONE", "LEFT TWO", ...). Each
    // line is its own block in this fixture, and the whole-page selection joins
    // the blocks' texts with a newline.
    QCOMPARE(selection->text, QStringLiteral("LEFT ONE\nLEFT TWO\nRIGHT ONE\nRIGHT TWO"));

    const pdfinteraction::TextCopyResult copy = interaction->selectedText();
    QVERIFY(copy.copied);
    QCOMPARE(copy.text, selection->text);

    // Stepping by line walks the same order: a whole-page selection already
    // ends on the last line, so stepping forward is refused, and the first line
    // is reachable by deselecting and stepping forward again.
    QVERIFY(!interaction->stepTextSelectionByLine(1));
    QVERIFY(interaction->deselectText());
    QVERIFY(interaction->stepTextSelectionByLine(1));
    QCOMPARE(interaction->state().textSelection()->text, QStringLiteral("LEFT ONE"));
    QVERIFY(interaction->stepTextSelectionByLine(1));
    QCOMPARE(interaction->state().textSelection()->text, QStringLiteral("LEFT TWO"));
}

void DocumentViewSessionTest::selectionOnAPageWithNoTextLayerReportsThatState()
{
    pdf::PDFDocument document = makeImageOnlyDocument();

    DocumentViewSession session;
    session.context().setDocument(&document);
    session.prepareDocumentView();
    configureViewport(session.viewport());

    pdfinteraction::InteractionController* interaction = session.interaction();
    QVERIFY(interaction != nullptr);

    // The attempt stores a typed selection that says why there is nothing.
    QVERIFY(interaction->selectAllText());

    const std::optional<pdfinteraction::TextSelection>& selection = interaction->state().textSelection();
    QVERIFY(selection.has_value());
    QVERIFY(!selection->available);
    QCOMPARE(selection->unavailableReason, QStringLiteral("This page has no text layer."));
    QVERIFY(selection->text.isEmpty());

    // And Copy reports that state instead of copying an empty string.
    const pdfinteraction::TextCopyResult copy = interaction->selectedText();
    QVERIFY(!copy.copied);
    QCOMPARE(copy.refusalCode, QStringLiteral("text/no-text-layer"));
    QVERIFY(copy.text.isEmpty());
}

void DocumentViewSessionTest::revisionChangeDropsMeasurementsAndTextSelection()
{
    pdf::PDFDocument first = makeRotatedUserUnitDocument();
    pdf::PDFDocument second = makeTwoColumnTextDocument();

    DocumentViewSession session;
    session.context().setDocument(&first);
    session.prepareDocumentView();
    configureViewport(session.viewport());

    pdfinteraction::InteractionController* interaction = session.interaction();
    QVERIFY(interaction != nullptr);

    interaction->setActiveTool(pdfinteraction::InteractionTool::Measure);
    const QPoint fromPx = session.viewport().pagePointToViewportMatrix(0).map(QPointF(50.0, 50.0)).toPoint();
    const QPoint toPx = session.viewport().pagePointToViewportMatrix(0).map(QPointF(150.0, 150.0)).toPoint();
    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Press, fromPx, 1, Qt::LeftButton, Qt::LeftButton));
    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Move, toPx, 2, Qt::NoButton, Qt::LeftButton));
    interaction->handlePointer(makePointer(pdfinteraction::PointerAction::Release, toPx, 3, Qt::LeftButton));

    QCOMPARE(interaction->state().measurements().size(), 1);

    // A revision change, delivered the way the shell delivers one: the context
    // replaces the document and the session's own wiring drops both derived
    // facts.
    session.context().setDocument(&second);

    QVERIFY(interaction->state().measurements().isEmpty());
    QVERIFY(!interaction->state().measurementDraft().has_value());
    QVERIFY(!interaction->state().textSelection().has_value());
}

void DocumentViewSessionTest::imageExtractionReportsThePagesImages()
{
    pdf::PDFDocument document = makeImageOnlyDocument();

    DocumentViewSession session;
    session.context().setDocument(&document);
    session.prepareDocumentView();

    pdfinteraction::PDFDocumentImageExtractor* extractor = session.imageExtractor();
    QVERIFY(extractor != nullptr);

    QString typedError;
    const QList<pdfinteraction::PageImage> images = extractor->extractPage(0, &typedError);

    QVERIFY(typedError.isEmpty());
    QCOMPARE(images.size(), 1);
    QCOMPARE(images.constFirst().pageIndex, 0);
    QCOMPARE(images.constFirst().order, 0);
    QCOMPARE(images.constFirst().image.size(), QSize(2, 2));
    QVERIFY(!images.constFirst().image.isNull());

    // A page index that does not exist is a typed refusal, not an empty success.
    const QList<pdfinteraction::PageImage> missing = extractor->extractPage(7, &typedError);
    QVERIFY(missing.isEmpty());
    QVERIFY(!typedError.isEmpty());
}

QTEST_GUILESS_MAIN(DocumentViewSessionTest)

#include "tst_documentviewsessiontest.moc"
