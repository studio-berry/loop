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

#include "pdfevidencegraph.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdfprocessingbudget.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QtGlobal>
#include <QtTest>

class HugeDocumentEnvelopeTest : public QObject
{
    Q_OBJECT

private slots:
    void generatedEnvelopeRecordsIdentityAndStaysBounded();
    void budgetExhaustionReportsExactKind();
};

void HugeDocumentEnvelopeTest::generatedEnvelopeRecordsIdentityAndStaysBounded()
{
    pdf::PDFDocumentBuilder builder;
    for (int i = 0; i < 40; ++i)
    {
        builder.appendPage(QRectF(0, 0, 200, 200));
    }
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    const pdf::PDFEvidenceGraph graph = pdf::PDFEvidenceCollector::collect(&session);
    QVERIFY(graph.isComplete());

    QJsonObject envelope{
        { QStringLiteral("commit"), qEnvironmentVariable("GIT_COMMIT", QStringLiteral("unspecified")) },
        { QStringLiteral("os"), QSysInfo::prettyProductName() },
        { QStringLiteral("qt"), QString::fromLatin1(qVersion()) },
        { QStringLiteral("pages"), 40 },
        { QStringLiteral("records"), graph.records.size() },
        { QStringLiteral("fixture_digest"), QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("huge-envelope-40"), QCryptographicHash::Sha256).toHex()) }
    };
    QVERIFY(!envelope.value(QStringLiteral("commit")).toString().isEmpty());
    QVERIFY(!envelope.value(QStringLiteral("os")).toString().isEmpty());
    QVERIFY(envelope.value(QStringLiteral("pages")).toInt() == 40);
    Q_UNUSED(QJsonDocument(envelope).toJson());
}

void HugeDocumentEnvelopeTest::budgetExhaustionReportsExactKind()
{
    pdf::PDFProcessingLimits limits;
    limits.maxRasterTileBytes = 4;
    pdf::PDFProcessingBudget budget(limits);
    try
    {
        budget.chargeRasterTileBytes(16, QStringLiteral("raster"));
        QFAIL("expected budget exception");
    }
    catch (const pdf::PDFBudgetExceededException& exception)
    {
        QCOMPARE(exception.getDetail().kind, pdf::PDFBudgetKind::RasterTileBytes);
        QCOMPARE(QString::fromLatin1(pdf::getPDFBudgetKindName(exception.getDetail().kind)),
                 QStringLiteral("raster-tile-bytes"));
    }
}

QTEST_APPLESS_MAIN(HugeDocumentEnvelopeTest)
#include "tst_hugedocumentenvelopetest.moc"
