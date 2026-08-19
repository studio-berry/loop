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
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

class SchemaEvolutionTest : public QObject
{
    Q_OBJECT

private slots:
    void integerSchemaVersionIsMajorWithZeroMinor();
    void malformedAndOversizedSchemaVersionsFailClosed();
    void unsupportedMajorFailsClosed();
    void compatibilityResourceIsCwdIndependentAndMatchesEveryKind();
    void unavailableCompatibilityMatrixFailsClosed();
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

void SchemaEvolutionTest::malformedAndOversizedSchemaVersionsFailClosed()
{
    const QList<QJsonValue> invalidValues{
        QJsonValue(65536),
        QJsonValue(65537),
        QJsonValue(1.5),
        QJsonValue(QStringLiteral("65536.0")),
        QJsonValue(QStringLiteral("65537.0")),
        QJsonValue(QStringLiteral("1.0.1")),
        QJsonValue(QStringLiteral("-1")),
        QJsonValue(QStringLiteral("1.-1")),
        QJsonValue(QStringLiteral("1.")),
        QJsonValue(QStringLiteral(".1"))
    };
    for (const QJsonValue& value : invalidValues)
    {
        bool ok = true;
        QVERIFY2(!pdf::PDFSchemaVersion::fromJsonValue(value, &ok).isValid(),
                 qPrintable(QStringLiteral("accepted invalid schema version %1").arg(value.toVariant().toString())));
        QVERIFY(!ok);
    }

    bool ok = false;
    const pdf::PDFSchemaVersion maximum = pdf::PDFSchemaVersion::fromJsonValue(QStringLiteral("65535.65535"), &ok);
    QVERIFY(ok);
    QCOMPARE(maximum.major, quint16(65535));
    QCOMPARE(maximum.minor, quint16(65535));

    const pdf::PDFSchemaVersion zeroMinor = pdf::PDFSchemaVersion::fromJsonValue(QStringLiteral("1.0"), &ok);
    QVERIFY(ok);
    const pdf::PDFSchemaVersion expectedZeroMinor{ 1, 0 };
    QCOMPARE(zeroMinor, expectedZeroMinor);
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

void SchemaEvolutionTest::compatibilityResourceIsCwdIndependentAndMatchesEveryKind()
{
    QFile resource(QStringLiteral(":/loupe/schema-compatibility.json"));
    QVERIFY(resource.open(QIODevice::ReadOnly));
    const QJsonObject matrix = QJsonDocument::fromJson(resource.readAll()).object();
    const QJsonObject kinds = matrix.value(QStringLiteral("kinds")).toObject();
    QVERIFY(!kinds.isEmpty());

    const QList<QString> kindNames = kinds.keys();
    for (const QString& kindName : kindNames)
    {
        const pdf::PDFSchemaKind kind = pdf::pdfSchemaKindFromString(kindName);
        QVERIFY2(kind != pdf::PDFSchemaKind::Unknown, qPrintable(kindName));
        const QJsonArray supported = kinds.value(kindName).toObject().value(QStringLiteral("supported_majors")).toArray();
        for (const QJsonValue& major : { QJsonValue(1), QJsonValue(2), QJsonValue(3) })
        {
            const bool expected = std::any_of(supported.cbegin(), supported.cend(), [&](const QJsonValue& value)
                                              { return value.toInt() == major.toInt(); });
            const auto actual = pdf::checkSchemaCompatibilityWithMatrix(kind,
                                                                          { quint16(major.toInt()), 0 },
                                                                          matrix);
            QCOMPARE(actual == pdf::PDFSchemaCompatibility::Compatible, expected);
        }
    }

    QCOMPARE(pdf::checkSchemaCompatibilityWithMatrix(pdf::PDFSchemaKind::PageMasterManifest, { 2, 0 }, matrix),
             pdf::PDFSchemaCompatibility::UnsupportedMajor);
    QCOMPARE(pdf::checkSchemaCompatibilityWithMatrix(pdf::PDFSchemaKind::PageMasterManifest, { 3, 0 }, matrix),
             pdf::PDFSchemaCompatibility::Compatible);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString previousPath = QDir::currentPath();
    QVERIFY(QDir::setCurrent(tempDir.path()));
    QCOMPARE(pdf::checkSchemaCompatibility(pdf::PDFSchemaKind::PageMasterManifest, { 3, 0 }),
             pdf::PDFSchemaCompatibility::Compatible);
    QCOMPARE(pdf::checkSchemaCompatibility(pdf::PDFSchemaKind::PageMasterManifest, { 2, 0 }),
             pdf::PDFSchemaCompatibility::UnsupportedMajor);
    QVERIFY(QDir::setCurrent(previousPath));
}

void SchemaEvolutionTest::unavailableCompatibilityMatrixFailsClosed()
{
    QCOMPARE(pdf::checkSchemaCompatibilityWithMatrix(pdf::PDFSchemaKind::PreflightReport, { 1, 0 }, {}),
             pdf::PDFSchemaCompatibility::UnsupportedMajor);
    QCOMPARE(pdf::checkSchemaCompatibilityWithMatrix(pdf::PDFSchemaKind::PageMasterManifest, { 3, 0 }, {}),
             pdf::PDFSchemaCompatibility::UnsupportedMajor);
    QCOMPARE(pdf::checkSchemaCompatibilityWithMatrix(pdf::PDFSchemaKind::Unknown, { 1, 0 }, {}),
             pdf::PDFSchemaCompatibility::UnknownKind);
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
