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

#include "pdfschemaversion.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class SchemaEvolutionTest : public QObject
{
    Q_OBJECT

private slots:
    void integerSchemaVersionIsMajorWithZeroMinor();
    void unsupportedMajorFailsClosed();
    void currentAndPreviousReportGoldensRoundTrip();
    void migrateIsPure();
};

void SchemaEvolutionTest::integerSchemaVersionIsMajorWithZeroMinor()
{
    bool ok = false;
    const pdf::PDFSchemaVersion version = pdf::PDFSchemaVersion::fromJsonValue(3, &ok);
    QVERIFY(ok);
    QCOMPARE(int(version.major), 3);
    QCOMPARE(int(version.minor), 0);
}

void SchemaEvolutionTest::unsupportedMajorFailsClosed()
{
    QCOMPARE(pdf::checkSchemaCompatibility(pdf::PDFSchemaKind::PreflightReport, { 99, 0 }),
             pdf::PDFSchemaCompatibility::UnsupportedMajor);
    QCOMPARE(pdf::checkSchemaCompatibility(pdf::PDFSchemaKind::Unknown, { 1, 0 }),
             pdf::PDFSchemaCompatibility::UnknownKind);
    QCOMPARE(pdf::checkSchemaCompatibility(pdf::PDFSchemaKind::PreflightReport, { 3, 0 }),
             pdf::PDFSchemaCompatibility::Compatible);
}

void SchemaEvolutionTest::currentAndPreviousReportGoldensRoundTrip()
{
    auto load = [](const QString& name, bool* opened)
    {
        QFile file(QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/schemas/") + name);
        *opened = file.open(QIODevice::ReadOnly);
        if (!*opened)
        {
            return QJsonObject();
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    };

    bool opened = false;
    const QJsonObject current = load(QStringLiteral("preflight-report-v3.json"), &opened);
    QVERIFY2(opened, "preflight-report-v3.json");
    const pdf::PDFSchemaEnvelope currentEnvelope = pdf::readSchemaEnvelope(current);
    QCOMPARE(currentEnvelope.kind, pdf::PDFSchemaKind::PreflightReport);
    QCOMPARE(int(currentEnvelope.version.major), 3);
    QCOMPARE(pdf::checkSchemaCompatibility(currentEnvelope.kind, currentEnvelope.version),
             pdf::PDFSchemaCompatibility::Compatible);

    const QJsonObject previous = load(QStringLiteral("preflight-report-v2.json"), &opened);
    QVERIFY2(opened, "preflight-report-v2.json");
    const pdf::PDFSchemaEnvelope previousEnvelope = pdf::readSchemaEnvelope(previous);
    QCOMPARE(int(previousEnvelope.version.major), 2);
    QCOMPARE(pdf::checkSchemaCompatibility(previousEnvelope.kind, previousEnvelope.version),
             pdf::PDFSchemaCompatibility::Compatible);

    const QJsonObject unsupported = load(QStringLiteral("unsupported-major.json"), &opened);
    QVERIFY2(opened, "unsupported-major.json");
    const pdf::PDFSchemaEnvelope bad = pdf::readSchemaEnvelope(unsupported);
    QCOMPARE(pdf::checkSchemaCompatibility(bad.kind, bad.version),
             pdf::PDFSchemaCompatibility::UnsupportedMajor);
}

void SchemaEvolutionTest::migrateIsPure()
{
    QJsonObject document{ { QStringLiteral("extra"), QStringLiteral("keep") } };
    const QJsonObject migrated = pdf::migrateSchemaDocument(pdf::PDFSchemaKind::PreflightReport, { 2, 0 }, document);
    QCOMPARE(migrated.value(QStringLiteral("extra")).toString(), QStringLiteral("keep"));
}

QTEST_APPLESS_MAIN(SchemaEvolutionTest)
#include "tst_schemaevolutiontest.moc"
