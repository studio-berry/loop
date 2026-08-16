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
#include "pdfdocumentsession.h"
#include "pdfevidencegraph.h"
#include "pdfimage.h"
#include "pdfpreflightverdict.h"
#include "pdfschemaversion.h"
#include "preflightengine.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

class EvidenceGraphTest : public QObject
{
    Q_OBJECT

private slots:
    void collectWithoutDocument_isIncomplete();
    void emptyPage_isComplete();
    void incompleteGraphCannotPass();
    void imageFamilyDualRunMatchesEngine();
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
