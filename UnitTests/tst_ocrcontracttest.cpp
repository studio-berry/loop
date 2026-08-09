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

#include "ocrsidecarprotocol.h"
#include "ocrsidecarutils.h"

#include <QtTest>

class OcrContractTest : public QObject
{
    Q_OBJECT

private slots:
    void validEditorEnvelope_isExtracted();
    void failedEditorEnvelope_isRejected();
    void missingEditorReport_isRejected();
    void wrongEditorReportSchema_isRejected();
    void sidecarResponse_requiresValidatedShape();
    void sidecarResponse_rejectsWrongPageAndMalformedLine();
    void languageSets_areNormalizedAndDeduplicated();
};

namespace
{

QJsonObject validReport()
{
    return QJsonObject{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("pass"), true },
        { QStringLiteral("pdf"), QStringLiteral("test.pdf") },
        { QStringLiteral("pages"), QJsonArray{} },
        { QStringLiteral("skipped_pages"), QJsonArray{} },
        { QStringLiteral("errors"), QJsonArray{} },
    };
}

QJsonObject validSidecarResponse()
{
    return QJsonObject{
        { QStringLiteral("page"), 1 },
        { QStringLiteral("ok"), true },
        { QStringLiteral("text"), QStringLiteral("Detected text") },
        { QStringLiteral("lines"), QJsonArray{
            QJsonObject{
                { QStringLiteral("text"), QStringLiteral("Detected text") },
                { QStringLiteral("confidence"), 0.97 },
                { QStringLiteral("bbox"), QJsonObject{
                    { QStringLiteral("x"), 20.0 },
                    { QStringLiteral("y"), 30.0 },
                    { QStringLiteral("width"), 100.0 },
                    { QStringLiteral("height"), 12.0 },
                } },
            },
        } },
    };
}

}   // namespace

void OcrContractTest::validEditorEnvelope_isExtracted()
{
    const QJsonObject envelope{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("command"), QStringLiteral("ocr") },
        { QStringLiteral("status"), QStringLiteral("success") },
        { QStringLiteral("data"), QJsonObject{ { QStringLiteral("report"), validReport() } } },
    };

    QJsonObject report;
    QString error;
    QVERIFY2(pdfplugin::ocr::extractOcrReport(envelope, &report, &error), qPrintable(error));
    QCOMPARE(report.value(QStringLiteral("schema_version")).toInt(), 1);
}

void OcrContractTest::failedEditorEnvelope_isRejected()
{
    const QJsonObject envelope{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("status"), QStringLiteral("error") },
    };

    QString error;
    QVERIFY(!pdfplugin::ocr::extractOcrReport(envelope, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("did not succeed")));
}

void OcrContractTest::missingEditorReport_isRejected()
{
    const QJsonObject envelope{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("status"), QStringLiteral("success") },
        { QStringLiteral("data"), QJsonObject{} },
    };

    QString error;
    QVERIFY(!pdfplugin::ocr::extractOcrReport(envelope, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("missing OCR report")));
}

void OcrContractTest::wrongEditorReportSchema_isRejected()
{
    const QJsonObject envelope{
        { QStringLiteral("status"), QStringLiteral("success") },
        { QStringLiteral("data"), QJsonObject{ { QStringLiteral("report"), validReport() } } },
    };
    QJsonObject report = envelope.value(QStringLiteral("data")).toObject()
                             .value(QStringLiteral("report")).toObject();
    report.insert(QStringLiteral("schema_version"), 99);

    QJsonObject malformedEnvelope = envelope;
    malformedEnvelope.insert(QStringLiteral("data"), QJsonObject{ { QStringLiteral("report"), report } });
    QString error;
    QVERIFY(!pdfplugin::ocr::extractOcrReport(malformedEnvelope, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("Unsupported OCR report schema_version")));
}

void OcrContractTest::sidecarResponse_requiresValidatedShape()
{
    QString error;
    QVERIFY2(pdftool::ocr::validateSidecarResponse(validSidecarResponse(), 1, &error), qPrintable(error));

    QJsonObject failedResponse{
        { QStringLiteral("page"), 1 },
        { QStringLiteral("ok"), false },
        { QStringLiteral("error"), QStringLiteral("image not found") },
    };
    QVERIFY2(pdftool::ocr::validateSidecarResponse(failedResponse, 1, &error), qPrintable(error));
}

void OcrContractTest::sidecarResponse_rejectsWrongPageAndMalformedLine()
{
    QString error;
    QJsonObject wrongPage = validSidecarResponse();
    wrongPage.insert(QStringLiteral("page"), 2);
    QVERIFY(!pdftool::ocr::validateSidecarResponse(wrongPage, 1, &error));

    QJsonObject malformedLineResponse = validSidecarResponse();
    malformedLineResponse.insert(QStringLiteral("lines"), QJsonArray{ QJsonObject{
        { QStringLiteral("text"), QStringLiteral("bad") },
        { QStringLiteral("confidence"), QStringLiteral("not-a-number") },
        { QStringLiteral("bbox"), QJsonObject{} },
    } });
    QVERIFY(!pdftool::ocr::validateSidecarResponse(malformedLineResponse, 1, &error));
}

void OcrContractTest::languageSets_areNormalizedAndDeduplicated()
{
    QCOMPARE(pdftool::ocr::normalizeLanguages(QStringLiteral(" EN,fr,en,, FR ")),
             QStringList{ QStringLiteral("en"), QStringLiteral("fr") });
    QCOMPARE(pdftool::ocr::normalizeLanguages(QString()), QStringList{ QStringLiteral("en") });
}

QTEST_MAIN(OcrContractTest)
#include "tst_ocrcontracttest.moc"
