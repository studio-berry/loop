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

// Issue #234 acceptance evidence: every annotation subtype is created through
// the typed direct-edit gateway on DocumentFacade into Core, reads back with the
// expected subtype, geometry and appearance stream, and each accepted edit
// appends one operation-history event and rollback point (the #124 model). The
// refusal lanes are stale revision and missing modify-annotations.
//
// QTEST_MAIN (QGuiApplication, not QWidgets): creating an inline-text or sticky
// note annotation renders glyphs through QPainter, which needs the GUI
// application object; the link line still excludes Qt6::Widgets and QML, and
// the ctest environment runs the platform offscreen.

#include <QtTest>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>

#include "commandcatalog.h"
#include "directeditrequest.h"
#include "documentfacade.h"
#include "documentloader.h"
#include "jobsubmitter.h"

#include "pdfannotation.h"
#include "pdfannotationauthoring.h"
#include "pdfauthoringhistory.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfdocumentwriter.h"
#include "pdfoperationhistorystore.h"
#include "pdfpage.h"
#include "pdfsecurityhandler.h"

namespace
{

pdf::PDFDocumentPointer buildDocument(int pageCount = 1)
{
    pdf::PDFDocumentBuilder builder;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 612, 792));
    }

    return pdf::PDFDocumentPointer(new pdf::PDFDocument(builder.build()));
}

/// A submitter that runs work inline; it is deliberately not a second
/// scheduler (mirrors tst_documentfacadetest.cpp).
class FakeJobSubmitter final : public pdfinteraction::IJobSubmitter
{
public:
    QString submit(pdf::PDFJobSpec spec, pdf::PDFJobWork work) override
    {
        const QString jobId = spec.jobId.isEmpty() ? QStringLiteral("job-%1").arg(++m_sequence) : spec.jobId;
        m_specs.insert(jobId, spec);
        m_status.insert(jobId, pdf::PDFJobStatus::Running);
        auto token = std::make_shared<pdf::PDFJobCancellationToken>();
        pdf::PDFJobContext context(token, pdf::PDFProcessingLimits::conservativeDefaults(), [](int) {});
        work(context);
        m_status.insert(jobId, pdf::PDFJobStatus::Succeeded);
        return jobId;
    }

    bool cancel(const QString&) override { return false; }

    pdf::PDFJobSnapshot snapshot(const QString& jobId) const override
    {
        pdf::PDFJobSnapshot result;
        result.jobId = jobId;
        result.status = m_status.value(jobId, pdf::PDFJobStatus::Succeeded);
        result.kind = m_specs.value(jobId).kind;
        return result;
    }

    void publishCurrentRevision(const QString& documentKey,
                                const pdf::PDFRevisionIdentity& revision) override
    {
        publishedRevisions.insert(documentKey, revision);
    }

    void clearCurrentRevision(const QString& documentKey) override
    {
        publishedRevisions.remove(documentKey);
    }

    QHash<QString, pdf::PDFRevisionIdentity> publishedRevisions;

private:
    int m_sequence = 0;
    QHash<QString, pdf::PDFJobSpec> m_specs;
    QHash<QString, pdf::PDFJobStatus> m_status;
};

class FakeDocumentLoader final : public pdfinteraction::IDocumentLoader
{
public:
    pdfinteraction::DocumentLoadResult load(const pdfinteraction::DocumentSource&,
                                            pdf::PDFJobContext&) override
    {
        ++loadCount;
        pdfinteraction::DocumentLoadResult result = nextResult;
        if (result.outcome == pdfinteraction::DocumentLoadOutcome::Loaded && !result.document)
        {
            result.document = buildDocument();
        }
        return result;
    }

    pdfinteraction::DocumentLoadResult nextResult;
    int loadCount = 0;
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

/// Everything one gateway test needs, wired the way a host wires it.
struct Harness
{
    Harness()
    {
        loader.nextResult.outcome = pdfinteraction::DocumentLoadOutcome::Loaded;
        facade = std::make_unique<pdfinteraction::DocumentFacade>(context, submitter, loader, writer, catalog);
    }

    /// Opens the fake document and waits for the queued admission.
    bool open()
    {
        facade->open(QStringLiteral("/corpus/fixture.pdf"));
        if (facade->state() == pdfinteraction::DocumentState::Ready)
        {
            return true;
        }
        return QTest::qWaitFor([this]()
                               { return facade->state() == pdfinteraction::DocumentState::Ready; });
    }

    pdf::PDFDocumentContext context{ nullptr };
    FakeJobSubmitter submitter;
    FakeDocumentLoader loader;
    FakeDocumentWriter writer;
    pdfinteraction::CommandCatalog catalog;
    std::unique_ptr<pdfinteraction::DocumentFacade> facade;
};

pdfinteraction::DirectEditRequest makeRequest(pdf::AnnotationAuthoringSubtype subtype,
                                              pdf::PDFRevisionIdentity revision)
{
    pdfinteraction::DirectEditRequest request;
    request.kind = pdfinteraction::DirectEditKind::CreateAnnotation;
    request.annotation.subtype = subtype;
    request.annotation.pageIndex = 0;
    request.revision = revision;
    request.operatorIdentity = QStringLiteral("UnitTest");
    return request;
}

/// One fully-populated spec per subtype, so the data-driven round trip can name
/// its expected geometry from the same values the edit carried.
pdf::AnnotationAuthoringSpec specForSubtype(pdf::AnnotationAuthoringSubtype subtype)
{
    pdf::AnnotationAuthoringSpec spec;
    spec.subtype = subtype;
    spec.pageIndex = 0;
    spec.rectangle = QRectF(100, 200, 120, 24);

    switch (subtype)
    {
        case pdf::AnnotationAuthoringSubtype::StraightLine:
            spec.vertices = { QPointF(100, 300), QPointF(220, 340) };
            break;
        case pdf::AnnotationAuthoringSubtype::Polyline:
            spec.vertices = { QPointF(100, 400), QPointF(150, 430), QPointF(220, 410) };
            break;
        case pdf::AnnotationAuthoringSubtype::Polygon:
            spec.vertices = { QPointF(100, 500), QPointF(180, 500), QPointF(140, 560) };
            break;
        case pdf::AnnotationAuthoringSubtype::FreehandCurve:
            spec.vertices = { QPointF(300, 300), QPointF(320, 330), QPointF(360, 340), QPointF(400, 310) };
            break;
        case pdf::AnnotationAuthoringSubtype::InlineText:
            spec.contents = QStringLiteral("Inline note text");
            break;
        default:
            if (subtype >= pdf::AnnotationAuthoringSubtype::StickyNoteComment)
            {
                spec.contents = QStringLiteral("Sticky note text");
                spec.rectangle = QRectF(100, 600, pdf::StickyNoteIconSize, pdf::StickyNoteIconSize);
            }
            break;
    }

    return spec;
}

pdf::AnnotationType expectedAnnotationType(pdf::AnnotationAuthoringSubtype subtype)
{
    switch (subtype)
    {
        case pdf::AnnotationAuthoringSubtype::TextHighlight:
            return pdf::AnnotationType::Highlight;
        case pdf::AnnotationAuthoringSubtype::TextUnderline:
            return pdf::AnnotationType::Underline;
        case pdf::AnnotationAuthoringSubtype::TextStrikeout:
            return pdf::AnnotationType::StrikeOut;
        case pdf::AnnotationAuthoringSubtype::TextSquiggly:
            return pdf::AnnotationType::Squiggly;
        case pdf::AnnotationAuthoringSubtype::Rectangle:
            return pdf::AnnotationType::Square;
        case pdf::AnnotationAuthoringSubtype::Ellipse:
            return pdf::AnnotationType::Circle;
        case pdf::AnnotationAuthoringSubtype::StraightLine:
            return pdf::AnnotationType::Line;
        case pdf::AnnotationAuthoringSubtype::Polyline:
            return pdf::AnnotationType::Polyline;
        case pdf::AnnotationAuthoringSubtype::Polygon:
            return pdf::AnnotationType::Polygon;
        case pdf::AnnotationAuthoringSubtype::FreehandCurve:
            return pdf::AnnotationType::Ink;
        case pdf::AnnotationAuthoringSubtype::InlineText:
            return pdf::AnnotationType::FreeText;

        default:
            return pdf::AnnotationType::Text;
    }
}

/// Decoded normal appearance stream of one annotation, or an empty result when
/// the annotation has none. The AP dictionary is read from the document's
/// storage directly: the parsed-annotation accessor is an inline wrapper around
/// a Core symbol the shared library does not export to tests.
struct AppearanceStream
{
    bool present = false;
    QByteArray content;
    QRectF boundingBox;
};

AppearanceStream normalAppearanceOf(const pdf::PDFDocument* document, pdf::PDFObjectReference annotation)
{
    AppearanceStream result;
    const pdf::PDFObjectStorage& storage = document->getStorage();
    const pdf::PDFDictionary* annotationDictionary =
        storage.getObject(pdf::PDFObject::createReference(annotation)).getDictionary();
    if (!annotationDictionary)
    {
        return result;
    }

    const pdf::PDFDictionary* appearanceDictionary =
        storage.getObject(annotationDictionary->get("AP")).getDictionary();
    if (!appearanceDictionary)
    {
        return result;
    }

    const pdf::PDFStream* stream = storage.getObject(appearanceDictionary->get("N")).getStream();
    if (!stream)
    {
        return result;
    }

    result.present = true;
    result.content = document->getDecodedStream(stream);

    if (const pdf::PDFDictionary* dictionary = stream->getDictionary())
    {
        const pdf::PDFObject bbox = dictionary->get("BBox");
        if (const pdf::PDFArray* array = bbox.getArray())
        {
            if (array->getCount() == 4)
            {
                result.boundingBox = QRectF(QPointF(array->getItem(0).getReal(), array->getItem(1).getReal()),
                                            QPointF(array->getItem(2).getReal(), array->getItem(3).getReal()));
            }
        }
    }
    return result;
}

}   // namespace

class AnnotationAuthoringTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void catalogBindsTheWholeAnnotationFamily_data();
    void catalogBindsTheWholeAnnotationFamily();
    void parametersMapToTypedRequestsAndRefuseMalformedOnes();

    void subtypesRoundTripThroughTheGateway_data();
    void subtypesRoundTripThroughTheGateway();

    void appearanceStreamsAreDeterministicAndGeometrySensitive();

    void staleRevisionIsRefusedAndNothingChanges();
    void permissionRestrictedDocumentIsRefusedAndOwnerIsAdmitted();

    void acceptedEditAppendsOneHistoryEventAndRollbackPoint();
};

void AnnotationAuthoringTest::initTestCase()
{
    qRegisterMetaType<pdf::PDFRevisionIdentity>("pdf::PDFRevisionIdentity");
    qRegisterMetaType<pdfinteraction::CommandResult>("pdfinteraction::CommandResult");
}

void AnnotationAuthoringTest::catalogBindsTheWholeAnnotationFamily_data()
{
    QTest::addColumn<QString>("commandId");
    QTest::addColumn<int>("subtype");

    for (const auto& entry : pdfinteraction::annotationAuthoringCommandIds())
    {
        QTest::newRow(qPrintable(entry.first)) << entry.first << int(entry.second);
    }
}

void AnnotationAuthoringTest::catalogBindsTheWholeAnnotationFamily()
{
    QFETCH(QString, commandId);
    QFETCH(int, subtype);

    // The family table is the single declaration the handlers were registered
    // from; it must name exactly the contract ids, and the contract must agree
    // they are implemented with a classified capability.
    pdfinteraction::CommandCatalog catalog;
    QVERIFY2(catalog.isLoaded(), qPrintable(catalog.loadError()));

    const pdfinteraction::CommandDescriptor* descriptor = catalog.descriptor(commandId);
    QVERIFY2(descriptor != nullptr, qPrintable(commandId));
    QVERIFY(descriptor->isImplemented());
    QCOMPARE(descriptor->capability, pdfinteraction::CommandCapability::DocumentModify);
    QVERIFY(!descriptor->parameters.isEmpty());

    pdf::AnnotationAuthoringSubtype parsed;
    QVERIFY(pdf::parseAnnotationAuthoringSubtype(
        QLatin1String(pdf::getAnnotationAuthoringSubtypeName(pdf::AnnotationAuthoringSubtype(subtype))), &parsed));
    QCOMPARE(int(parsed), subtype);

    pdfinteraction::CommandCatalog::Handler handler;
    handler.invoke = [](pdfinteraction::CommandInvocationId, const QVariantMap&) {};
    QVERIFY(catalog.setHandler(commandId, handler));
}

void AnnotationAuthoringTest::parametersMapToTypedRequestsAndRefuseMalformedOnes()
{
    using pdf::AnnotationAuthoringSubtype;

    QString refusal;
    QVERIFY(!pdfinteraction::directEditRequestFromParameters(AnnotationAuthoringSubtype::TextHighlight, {}, &refusal));
    QCOMPARE(refusal, QStringLiteral("annotation/missing-page"));

    const QVariantMap highlight{
        { QStringLiteral("page"), 0 },
        { QStringLiteral("x"), 10.0 },
        { QStringLiteral("y"), 20.0 },
        { QStringLiteral("width"), 100.0 },
        { QStringLiteral("height"), 12.0 },
        { QStringLiteral("color"), QStringLiteral("#00FF00") },
    };
    const auto parsed = pdfinteraction::directEditRequestFromParameters(
        AnnotationAuthoringSubtype::TextHighlight, highlight, &refusal);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->annotation.rectangle, QRectF(10, 20, 100, 12));
    QCOMPARE(parsed->annotation.color, QColor(QStringLiteral("#00FF00")));

    // Degenerate geometry is refused at the boundary, never normalized.
    QVariantMap degenerate = highlight;
    degenerate.insert(QStringLiteral("width"), 0.0);
    QVERIFY(!pdfinteraction::directEditRequestFromParameters(AnnotationAuthoringSubtype::TextHighlight, degenerate, &refusal));
    QCOMPARE(refusal, QStringLiteral("annotation/invalid-geometry"));

    // A malformed point list is refused whole, not truncated.
    QVERIFY(!pdfinteraction::directEditRequestFromParameters(
        AnnotationAuthoringSubtype::Polyline,
        { { QStringLiteral("page"), 0 }, { QStringLiteral("points"), QStringLiteral("1 2 3") } }, &refusal));
    QCOMPARE(refusal, QStringLiteral("annotation/invalid-points"));

    const auto polyline = pdfinteraction::directEditRequestFromParameters(
        AnnotationAuthoringSubtype::Polyline,
        { { QStringLiteral("page"), 0 }, { QStringLiteral("points"), QStringLiteral("1 2 30 40 50 60") } }, &refusal);
    QVERIFY(polyline.has_value());
    QCOMPARE(polyline->annotation.vertices.size(), std::size_t(3));
    QCOMPARE(polyline->annotation.vertices[1], QPointF(30, 40));

    // Text is required where the subtype carries text.
    QVERIFY(!pdfinteraction::directEditRequestFromParameters(
        AnnotationAuthoringSubtype::StickyNoteNote,
        { { QStringLiteral("page"), 0 }, { QStringLiteral("x"), 5.0 }, { QStringLiteral("y"), 5.0 } }, &refusal));
    QCOMPARE(refusal, QStringLiteral("annotation/missing-text"));

    const auto sticky = pdfinteraction::directEditRequestFromParameters(
        AnnotationAuthoringSubtype::StickyNoteNote,
        { { QStringLiteral("page"), 1 },
          { QStringLiteral("x"), 5.0 },
          { QStringLiteral("y"), 6.0 },
          { QStringLiteral("text"), QStringLiteral("note") } },
        &refusal);
    QVERIFY(sticky.has_value());
    QCOMPARE(sticky->annotation.pageIndex, pdf::PDFInteger(1));
    QCOMPARE(sticky->annotation.rectangle, QRectF(5, 6, pdf::StickyNoteIconSize, pdf::StickyNoteIconSize));
}

void AnnotationAuthoringTest::subtypesRoundTripThroughTheGateway_data()
{
    QTest::addColumn<int>("subtype");

    for (const auto& entry : pdfinteraction::annotationAuthoringCommandIds())
    {
        QTest::newRow(pdf::getAnnotationAuthoringSubtypeName(entry.second)) << int(entry.second);
    }
}

void AnnotationAuthoringTest::subtypesRoundTripThroughTheGateway()
{
    QFETCH(int, subtype);
    const pdf::AnnotationAuthoringSubtype authoringSubtype = pdf::AnnotationAuthoringSubtype(subtype);

    Harness harness;
    QVERIFY(harness.open());

    const pdf::PDFRevisionIdentity before = harness.facade->currentRevision();
    const pdf::AnnotationAuthoringSpec spec = specForSubtype(authoringSubtype);

    pdfinteraction::DirectEditRequest request = makeRequest(authoringSubtype, before);
    request.annotation = spec;

    const pdfinteraction::DirectEditOutcome outcome = harness.facade->applyDirectEdit(request);
    QVERIFY2(outcome.accepted, qPrintable(outcome.typedError));
    QVERIFY(outcome.annotation.isValid());

    // One accepted edit is exactly one new revision, and the document is dirty.
    const pdf::PDFRevisionIdentity after = harness.facade->currentRevision();
    QVERIFY(after != before);
    QCOMPARE(after.documentRevision, before.documentRevision + 1);
    QVERIFY(harness.facade->facets().testFlag(pdfinteraction::DocumentFacet::Dirty));
    QCOMPARE(harness.facade->shellDocumentStatus(), pdfinteraction::ShellDocumentStatus::Modified);

    // Read back through Core's own model: subtype, geometry, appearance stream.
    const pdf::PDFDocument* document = harness.context.getDocument();
    QVERIFY(document != nullptr);
    const pdf::PDFPage* page = document->getCatalog()->getPage(std::size_t(spec.pageIndex));
    QVERIFY(page != nullptr);
    const std::vector<pdf::PDFObjectReference>& annotations = page->getAnnotations();
    QVERIFY(std::find(annotations.cbegin(), annotations.cend(), outcome.annotation) != annotations.cend());

    const pdf::PDFAnnotationPtr annotation = pdf::PDFAnnotation::parse(&document->getStorage(), outcome.annotation);
    QVERIFY(annotation != nullptr);
    QCOMPARE(annotation->getType(), expectedAnnotationType(authoringSubtype));

    switch (authoringSubtype)
    {
        case pdf::AnnotationAuthoringSubtype::StraightLine:
        {
            const auto* line = dynamic_cast<const pdf::PDFLineAnnotation*>(annotation.data());
            QVERIFY(line != nullptr);
            QCOMPARE(line->getLine(), QLineF(spec.vertices[0], spec.vertices[1]));
            break;
        }
        case pdf::AnnotationAuthoringSubtype::Polyline:
        case pdf::AnnotationAuthoringSubtype::Polygon:
        {
            const auto* polygonal = dynamic_cast<const pdf::PDFPolygonalGeometryAnnotation*>(annotation.data());
            QVERIFY(polygonal != nullptr);
            QCOMPARE(polygonal->getVertices().size(), spec.vertices.size());
            for (std::size_t index = 0; index < spec.vertices.size(); ++index)
            {
                QCOMPARE(polygonal->getVertices()[index], spec.vertices[index]);
            }
            break;
        }
        case pdf::AnnotationAuthoringSubtype::FreehandCurve:
        {
            const auto* ink = dynamic_cast<const pdf::PDFInkAnnotation*>(annotation.data());
            QVERIFY(ink != nullptr);
            const QRectF inkBounds = ink->getInkPath().boundingRect();
            QVERIFY(inkBounds.contains(QRectF(spec.vertices[0], spec.vertices[3]).normalized()));
            break;
        }
        case pdf::AnnotationAuthoringSubtype::TextUnderline:
        case pdf::AnnotationAuthoringSubtype::TextStrikeout:
        case pdf::AnnotationAuthoringSubtype::TextSquiggly:
        {
            // These subtypes' appearance is a stroke, and the builder rewrites
            // Rect to the drawn bounding box, so the read-back rectangle is the
            // requested one expanded by the border width on each side.
            QCOMPARE(annotation->getRectangle(),
                     spec.rectangle.adjusted(-spec.borderWidth, -spec.borderWidth,
                                             spec.borderWidth, spec.borderWidth));
            break;
        }

        case pdf::AnnotationAuthoringSubtype::InlineText:
            QCOMPARE(annotation->getContents(), spec.contents);
            QCOMPARE(annotation->getRectangle(), spec.rectangle);
            break;

        default:
            QCOMPARE(annotation->getRectangle(), spec.rectangle);
            break;
    }

    if (authoringSubtype >= pdf::AnnotationAuthoringSubtype::StickyNoteComment)
    {
        QCOMPARE(annotation->getContents(), spec.contents);
    }

    const AppearanceStream appearance = normalAppearanceOf(document, outcome.annotation);
    QVERIFY2(appearance.present, pdf::getAnnotationAuthoringSubtypeName(authoringSubtype));
    QVERIFY(!appearance.content.isEmpty());
    QVERIFY(!appearance.boundingBox.isEmpty());
    QVERIFY2(appearance.boundingBox.intersects(annotation->getRectangle()),
             qPrintable(QStringLiteral("%1: BBox %2,%3 %4x%5 does not intersect %6,%7 %8x%9")
                            .arg(QLatin1String(pdf::getAnnotationAuthoringSubtypeName(authoringSubtype)))
                            .arg(appearance.boundingBox.x())
                            .arg(appearance.boundingBox.y())
                            .arg(appearance.boundingBox.width())
                            .arg(appearance.boundingBox.height())
                            .arg(annotation->getRectangle().x())
                            .arg(annotation->getRectangle().y())
                            .arg(annotation->getRectangle().width())
                            .arg(annotation->getRectangle().height())));
}

void AnnotationAuthoringTest::appearanceStreamsAreDeterministicAndGeometrySensitive()
{
    const pdf::AnnotationAuthoringSpec highlight = specForSubtype(pdf::AnnotationAuthoringSubtype::TextHighlight);

    const auto createHighlight = [](const pdf::AnnotationAuthoringSpec& spec, pdf::PDFObjectReference* annotation)
    {
        Harness harness;
        if (!harness.open())
        {
            return AppearanceStream();
        }
        pdfinteraction::DirectEditRequest request = makeRequest(spec.subtype, harness.facade->currentRevision());
        request.annotation = spec;
        const pdfinteraction::DirectEditOutcome outcome = harness.facade->applyDirectEdit(request);
        if (!outcome.accepted)
        {
            return AppearanceStream();
        }
        *annotation = outcome.annotation;
        return normalAppearanceOf(harness.context.getDocument(), outcome.annotation);
    };

    // The same edit on two identical documents renders the same appearance
    // stream, byte for byte: creation is deterministic, not time- or
    // pointer-dependent.
    pdf::PDFObjectReference firstAnnotation;
    pdf::PDFObjectReference secondAnnotation;
    const AppearanceStream first = createHighlight(highlight, &firstAnnotation);
    const AppearanceStream second = createHighlight(highlight, &secondAnnotation);
    QVERIFY(first.present);
    QVERIFY(second.present);
    QCOMPARE(first.content, second.content);
    QCOMPARE(first.boundingBox, second.boundingBox);

    // A different geometry must produce a different stream, or the comparison
    // above would be vacuous.
    pdf::AnnotationAuthoringSpec moved = highlight;
    moved.rectangle = QRectF(300, 400, 60, 18);
    pdf::PDFObjectReference thirdAnnotation;
    const AppearanceStream third = createHighlight(moved, &thirdAnnotation);
    QVERIFY(third.present);
    QVERIFY(third.content != first.content);
}

void AnnotationAuthoringTest::staleRevisionIsRefusedAndNothingChanges()
{
    Harness harness;
    QVERIFY(harness.open());

    const pdf::PDFRevisionIdentity stale = harness.facade->currentRevision();

    pdfinteraction::DirectEditRequest first = makeRequest(pdf::AnnotationAuthoringSubtype::TextHighlight, stale);
    first.annotation = specForSubtype(pdf::AnnotationAuthoringSubtype::TextHighlight);
    QVERIFY(harness.facade->applyDirectEdit(first).accepted);

    const pdf::PDFRevisionIdentity current = harness.facade->currentRevision();
    QCOMPARE(harness.context.getDocument()->getCatalog()->getPage(0)->getAnnotations().size(), std::size_t(1));

    // The second edit carries the revision it was begun against before the
    // first landed. It must fail closed before any mutation (invariant I07).
    pdfinteraction::DirectEditRequest second = makeRequest(pdf::AnnotationAuthoringSubtype::TextUnderline, stale);
    second.annotation = specForSubtype(pdf::AnnotationAuthoringSubtype::TextUnderline);
    const pdfinteraction::DirectEditOutcome refused = harness.facade->applyDirectEdit(second);
    QVERIFY(!refused.accepted);
    QCOMPARE(refused.typedError, QStringLiteral("direct-edit/stale-revision"));

    QCOMPARE(harness.facade->currentRevision(), current);
    QCOMPARE(harness.context.getDocument()->getCatalog()->getPage(0)->getAnnotations().size(), std::size_t(1));
}

void AnnotationAuthoringTest::permissionRestrictedDocumentIsRefusedAndOwnerIsAdmitted()
{
    // The fixture is a real encrypted file opened through the real reader: a
    // factory-built in-memory handler is owner-authorized by construction, so
    // only a reader-created handler shows the permissions the file declares.
    const auto writeEncryptedFixture = [](const QString& path, const QString& userPassword,
                                          const QString& ownerPassword, quint32 permissions)
    {
        pdf::PDFDocumentBuilder builder;
        builder.appendPage(QRectF(0, 0, 612, 792));
        pdf::PDFSecurityHandlerFactory::SecuritySettings settings;
        settings.algorithm = pdf::PDFSecurityHandlerFactory::AES_256;
        settings.permissions = permissions;
        settings.userPassword = userPassword;
        settings.ownerPassword = ownerPassword;
        settings.id = builder.build().getIdPart(0);
        builder.setSecurityHandler(pdf::PDFSecurityHandlerFactory::createSecurityHandler(settings));
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        return bool(writer.write(path, &document, false));
    };

    struct EncryptedHarness
    {
        EncryptedHarness(const QString& password)
        {
            loader = std::make_unique<pdfinteraction::PDFReaderDocumentLoader>(
                [password](bool* ok, const pdf::PDFOperationControl*)
                {
                    *ok = true;
                    return password;
                });
            facade = std::make_unique<pdfinteraction::DocumentFacade>(context, submitter, *loader, writer, catalog);
        }

        pdf::PDFDocumentContext context{ nullptr };
        FakeJobSubmitter submitter;
        std::unique_ptr<pdfinteraction::PDFReaderDocumentLoader> loader;
        FakeDocumentWriter writer;
        pdfinteraction::CommandCatalog catalog;
        std::unique_ptr<pdfinteraction::DocumentFacade> facade;
    };

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString restrictedPath = directory.filePath(QStringLiteral("restricted.pdf"));
    const QString ownerPath = directory.filePath(QStringLiteral("owner.pdf"));
    QVERIFY(writeEncryptedFixture(restrictedPath, QStringLiteral("user"), QStringLiteral("owner"), 0));
    QVERIFY(writeEncryptedFixture(ownerPath, QStringLiteral("user"), QStringLiteral("owner"), 0));

    // User access without modify-annotations: the edit is refused and nothing
    // is created.
    EncryptedHarness restricted(QStringLiteral("user"));
    restricted.facade->open(restrictedPath);
    QTRY_VERIFY(restricted.facade->state() == pdfinteraction::DocumentState::Ready);
    QVERIFY(restricted.facade->permissions().encrypted);
    QVERIFY(!restricted.facade->permissions().modifyAnnotations);

    pdfinteraction::DirectEditRequest request = makeRequest(pdf::AnnotationAuthoringSubtype::TextHighlight,
                                                            restricted.facade->currentRevision());
    request.annotation = specForSubtype(pdf::AnnotationAuthoringSubtype::TextHighlight);
    const pdfinteraction::DirectEditOutcome refused = restricted.facade->applyDirectEdit(request);
    QVERIFY(!refused.accepted);
    QCOMPARE(refused.typedError, QStringLiteral("direct-edit/permission-denied"));
    QCOMPARE(restricted.context.getDocument()->getCatalog()->getPage(0)->getAnnotations().size(), std::size_t(0));

    // Owner authorization satisfies the same permission bit: the same edit is
    // admitted when the file is opened with the owner password.
    EncryptedHarness owner(QStringLiteral("owner"));
    owner.facade->open(ownerPath);
    QTRY_VERIFY(owner.facade->state() == pdfinteraction::DocumentState::Ready);
    QVERIFY(owner.facade->permissions().modifyAnnotations);

    pdfinteraction::DirectEditRequest ownerRequest = makeRequest(pdf::AnnotationAuthoringSubtype::TextHighlight,
                                                                 owner.facade->currentRevision());
    ownerRequest.annotation = specForSubtype(pdf::AnnotationAuthoringSubtype::TextHighlight);
    const pdfinteraction::DirectEditOutcome accepted = owner.facade->applyDirectEdit(ownerRequest);
    QVERIFY2(accepted.accepted, qPrintable(accepted.typedError));
    QCOMPARE(owner.context.getDocument()->getCatalog()->getPage(0)->getAnnotations().size(), std::size_t(1));
}

void AnnotationAuthoringTest::acceptedEditAppendsOneHistoryEventAndRollbackPoint()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString documentPath = directory.filePath(QStringLiteral("annotated.pdf"));

    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFDocumentPointer source = buildDocument();
    QVERIFY(writer.write(documentPath, source.data(), false));

    // A real open of a real file: the sidecar has a document path to live beside.
    struct RealHarness
    {
        RealHarness()
        {
            facade = std::make_unique<pdfinteraction::DocumentFacade>(context, submitter, loader, writer, catalog);
        }

        pdf::PDFDocumentContext context{ nullptr };
        FakeJobSubmitter submitter;
        pdfinteraction::PDFReaderDocumentLoader loader;
        FakeDocumentWriter writer;
        pdfinteraction::CommandCatalog catalog;
        std::unique_ptr<pdfinteraction::DocumentFacade> facade;
    };

    RealHarness harness;
    harness.facade->open(documentPath);
    QTRY_VERIFY(harness.facade->state() == pdfinteraction::DocumentState::Ready);

    const auto applyEdit = [&harness](pdf::AnnotationAuthoringSubtype subtype)
    {
        pdfinteraction::DirectEditRequest request = makeRequest(subtype, harness.facade->currentRevision());
        request.annotation = specForSubtype(subtype);
        return harness.facade->applyDirectEdit(request);
    };

    const pdfinteraction::DirectEditOutcome first = applyEdit(pdf::AnnotationAuthoringSubtype::TextHighlight);
    QVERIFY2(first.accepted, qPrintable(first.typedError));
    QVERIFY2(first.historyAppended, qPrintable(first.historyError));
    QVERIFY(first.historySequence != 0);

    const pdfinteraction::DirectEditOutcome second = applyEdit(pdf::AnnotationAuthoringSubtype::StickyNoteNote);
    QVERIFY2(second.accepted, qPrintable(second.typedError));
    QVERIFY2(second.historyAppended, qPrintable(second.historyError));

    // The #124 model: every accepted edit is one event and one rollback point,
    // and the first edit's undo target is the protected original input.
    const QString databasePath =
        QDir(documentPath + QStringLiteral(".loop-history")).filePath(QStringLiteral("history.sqlite3"));
    QVERIFY(QFile::exists(databasePath));

    pdf::PDFOperationHistoryStore history(databasePath);
    QString error;
    QVERIFY2(history.open(&error), qPrintable(error));
    const pdf::PDFOperationHistoryVerification verification = history.verify();
    QVERIFY2(verification.verified, qPrintable(verification.errorMessage));
    QCOMPARE(verification.eventsChecked, qint64(2));

    const QList<pdf::PDFOperationHistoryEvent> events = history.events(&error);
    QCOMPARE(events.size(), 2);
    for (const pdf::PDFOperationHistoryEvent& event : events)
    {
        QCOMPARE(event.kind, pdf::PDFOperationHistoryEventKind::FixApplied);
        QCOMPARE(event.status, pdf::PDFOperationHistoryStatus::Accepted);
        QVERIFY(event.output.has_value());
        QCOMPARE(event.approval.kind, pdf::PDFApprovalKind::System);
        QCOMPARE(event.operatorIdentity, QStringLiteral("UnitTest"));
    }
    QVERIFY(events[0].output->sha256 != events[1].output->sha256);

    const QList<pdf::PDFRollbackPoint> points = history.rollbackPoints(&error);
    QCOMPARE(points.size(), 3);   // original input + one point per accepted edit
    QCOMPARE(std::count_if(points.cbegin(), points.cend(),
                           [](const pdf::PDFRollbackPoint& point)
                           { return point.isOriginalInput; }),
             qsizetype(1));
    QCOMPARE(std::count_if(points.cbegin(), points.cend(),
                           [](const pdf::PDFRollbackPoint& point)
                           { return !point.isOriginalInput; }),
             qsizetype(2));
}

QTEST_MAIN(AnnotationAuthoringTest)
#include "tst_annotationauthoringtest.moc"
