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

#include "pdfconstants.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentsession.h"
#include "pdfevidencegraph.h"
#include "pdfimage.h"
#include "pdfpreflightverdict.h"
#include "pdfschemaversion.h"
#include "preflightengine.h"

#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QtTest>

class EvidenceGraphTest : public QObject
{
    Q_OBJECT

private slots:
    void collectWithoutDocument_isIncomplete();
    void emptyPage_isComplete();
    void incompleteGraphCannotPass();
    void imageFamilyDualRunMatchesEngine();
    void colorantsFamilyDualRunMatchesEngine();
    void strokesFamilyDualRunMatchesEngine();
    void overprintTransparencyFamilyDualRunMatchesEngine();
    void fontsFamilyDualRunMatchesEngine();
    void remainingFamiliesCollectOnEmptyPage();
    void graphEnvelopeUsesEvidenceGraphKind();
};

namespace
{

pdf::PDFDocument buildLowDpiImagePage()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));

    QImage image(10, 10, QImage::Format_ARGB32);
    image.fill(qRgb(32, 32, 32));

    pdf::PDFImage::ImageEncodeOptions imageOptions;
    imageOptions.compression = pdf::PDFImage::ImageCompression::Flate;
    imageOptions.colorMode = pdf::PDFImage::ImageColorMode::Preserve;
    const pdf::PDFObjectReference imageReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFImage::createStreamFromImage(image, imageOptions))));

    QByteArray content("q 144 0 0 144 0 0 cm /Im1 Do Q");
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                               pdf::PDFObject::createInteger(content.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFStream(std::move(contentDictionary), std::move(content)))));

    pdf::PDFDictionary xObject;
    xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(pageReference,
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    return builder.build();
}

pdf::PDFDocument loadFixtureDocument(const char* relativePath)
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR) + QStringLiteral("/testdata/fixtures/") + QString::fromUtf8(relativePath);
    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        qFatal("Failed to load fixture '%s'", relativePath);
    }
    return document;
}

void assertFindingCitesGraphRecord(const QList<pdf::PreflightFinding>& findings,
                                   const QString& checkId,
                                   const QString& findingType,
                                   const pdf::PDFEvidenceRecord& record)
{
    for (const pdf::PreflightFinding& finding : findings)
    {
        if (finding.checkId == checkId && finding.type == findingType)
        {
            QVERIFY2(!finding.evidenceIds.isEmpty(),
                     qPrintable(QStringLiteral("Expected evidence_ids on %1 finding").arg(findingType)));
            QCOMPARE(finding.evidenceIds.first(), record.id);
            return;
        }
    }
    QFAIL(qPrintable(QStringLiteral("Expected finding type '%1' for check '%2'").arg(findingType, checkId)));
}

}   // namespace

void EvidenceGraphTest::collectWithoutDocument_isIncomplete()
{
    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(nullptr);
    QVERIFY(!graph.isComplete());
    QCOMPARE(graph.incompleteReason, QStringLiteral("missing-document"));
}

void EvidenceGraphTest::emptyPage_isComplete()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);

    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::pdfEvidenceAllDomains());
    QVERIFY(graph.isComplete());
    QVERIFY(graph.incompleteReason.isEmpty());
}

void EvidenceGraphTest::incompleteGraphCannotPass()
{
    pdf::PreflightEngine engine(nullptr);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Incomplete graph") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                            { QStringLiteral("min_dpi"), 300 } } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.inspectionComplete);
    QCOMPARE(result.errorCode, QStringLiteral("evidence-incomplete"));
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QVERIFY(!verdict.isPass());
    QVERIFY(!engine.lastEvidenceGraph().isComplete());
}

void EvidenceGraphTest::imageFamilyDualRunMatchesEngine()
{
    pdf::PDFDocument document = buildLowDpiImagePage();
    pdf::PDFDocumentSession session(&document);

    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::PDFEvidenceDomain::Images);
    QVERIFY(graph.isComplete());
    const QList<pdf::PDFEvidenceRecord> images = graph.recordsForTarget(pdf::PDFEvidenceDomain::Images,
                                                                        QStringLiteral("image-effective-dpi"));
    QVERIFY(!images.isEmpty());
    QVERIFY(images.first().observedValue < 300.0);

    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Image DPI") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                            { QStringLiteral("min_dpi"), 300 },
                                            { QStringLiteral("severity"), QStringLiteral("warning") } } } }
    };
    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.inspectionComplete);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().type, QStringLiteral("image-resolution"));
    QVERIFY(!result.warnings.first().evidenceIds.isEmpty());
    QCOMPARE(result.warnings.first().evidenceIds.first(), images.first().id);
    QVERIFY(engine.lastEvidenceGraph().isComplete());
}

void EvidenceGraphTest::colorantsFamilyDualRunMatchesEngine()
{
    pdf::PDFDocument document = loadFixtureDocument("rich-black.pdf");
    pdf::PDFDocumentSession session(&document);

    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::PDFEvidenceDomain::Colorants);
    QVERIFY(graph.isComplete());
    const QList<pdf::PDFEvidenceRecord> richBlack = graph.recordsForTarget(pdf::PDFEvidenceDomain::Colorants,
                                                                           QStringLiteral("rich-black"));
    QVERIFY(!richBlack.isEmpty());

    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Color inventory") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("color-inventory") },
                                            { QStringLiteral("severity"), QStringLiteral("info") },
                                            { QStringLiteral("probe_dpi"), 150 },
                                            { QStringLiteral("rich_black_k_percent"), 10 } } } }
    };
    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.inspectionComplete);
    assertFindingCitesGraphRecord(result.warnings,
                                  QStringLiteral("color-inventory"),
                                  QStringLiteral("rich-black"),
                                  richBlack.first());
    QVERIFY(engine.lastEvidenceGraph().isComplete());
}

void EvidenceGraphTest::strokesFamilyDualRunMatchesEngine()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFPageContentStreamBuilder contentBuilder(&builder,
                                                    pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    QPainter* painter = contentBuilder.begin(page);
    QVERIFY(painter != nullptr);

    QPen thinPen(Qt::black);
    thinPen.setWidthF(0.1);
    painter->setPen(thinPen);
    painter->drawLine(QPointF(20, 20), QPointF(180, 20));
    contentBuilder.end(painter);

    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);

    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::PDFEvidenceDomain::Strokes);
    QVERIFY(graph.isComplete());
    const QList<pdf::PDFEvidenceRecord> strokes = graph.recordsForTarget(pdf::PDFEvidenceDomain::Strokes,
                                                                         QStringLiteral("stroke-width"));
    QVERIFY(!strokes.isEmpty());

    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Thin strokes") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("thin-strokes") },
                                            { QStringLiteral("min_effective_width_pt"), 0.25 },
                                            { QStringLiteral("thin_stroke_severity"), QStringLiteral("warning") } } } }
    };
    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.inspectionComplete);
    assertFindingCitesGraphRecord(result.warnings,
                                  QStringLiteral("thin-strokes"),
                                  QStringLiteral("thin-stroke"),
                                  strokes.first());
    QVERIFY(engine.lastEvidenceGraph().isComplete());
}

void EvidenceGraphTest::overprintTransparencyFamilyDualRunMatchesEngine()
{
    pdf::PDFDocument document = loadFixtureDocument("white-overprint.pdf");
    pdf::PDFDocumentSession session(&document);

    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session,
                                                                           pdf::PDFEvidenceDomain::OverprintTransparency);
    QVERIFY(graph.isComplete());
    const QList<pdf::PDFEvidenceRecord> whiteOverprint = graph.recordsForTarget(
        pdf::PDFEvidenceDomain::OverprintTransparency, QStringLiteral("white-overprint"));
    QVERIFY(!whiteOverprint.isEmpty());

    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("White overprint") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("white-overprint") },
                                            { QStringLiteral("severity"), QStringLiteral("warning") } } } }
    };
    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.inspectionComplete);
    assertFindingCitesGraphRecord(result.warnings,
                                  QStringLiteral("white-overprint"),
                                  QStringLiteral("white-overprint"),
                                  whiteOverprint.first());
    QVERIFY(engine.lastEvidenceGraph().isComplete());
}

void EvidenceGraphTest::fontsFamilyDualRunMatchesEngine()
{
    pdf::PDFDocument document = loadFixtureDocument("font-not-embedded.pdf");
    pdf::PDFDocumentSession session(&document);

    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::PDFEvidenceDomain::Fonts);
    QVERIFY(graph.isComplete());
    const QList<pdf::PDFEvidenceRecord> fonts = graph.recordsForTarget(pdf::PDFEvidenceDomain::Fonts,
                                                                       QStringLiteral("font-resource"));
    QVERIFY(!fonts.isEmpty());

    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Embedded fonts") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("embedded-fonts") },
                                            { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };
    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.inspectionComplete);
    assertFindingCitesGraphRecord(result.errors,
                                  QStringLiteral("embedded-fonts"),
                                  QStringLiteral("embedded-fonts"),
                                  fonts.first());
    QVERIFY(engine.lastEvidenceGraph().isComplete());
}

void EvidenceGraphTest::remainingFamiliesCollectOnEmptyPage()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);

    const pdf::PDFEvidenceDomains remaining = pdf::PDFEvidenceDomain::Colorants | pdf::PDFEvidenceDomain::Strokes | pdf::PDFEvidenceDomain::OverprintTransparency | pdf::PDFEvidenceDomain::Fonts;
    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, remaining);
    QVERIFY(graph.isComplete());
}

void EvidenceGraphTest::graphEnvelopeUsesEvidenceGraphKind()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session, pdf::PDFEvidenceDomain::Images);
    const QJsonObject json = graph.toJson();
    const pdf::PDFSchemaEnvelope envelope = pdf::readSchemaEnvelope(json);
    QCOMPARE(envelope.kind, pdf::PDFSchemaKind::EvidenceGraph);
    QCOMPARE(int(envelope.version.major), 1);
    QCOMPARE(pdf::checkSchemaCompatibility(envelope.kind, envelope.version),
             pdf::PDFSchemaCompatibility::Compatible);
}

QTEST_GUILESS_MAIN(EvidenceGraphTest)
#include "tst_evidencegraphtest.moc"
