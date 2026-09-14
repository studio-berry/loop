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
    void currentVersionFailsClosedWithoutAMatrixEntry();
    void everySchemaKindIsCoveredByTheCompatibilityMatrix();
    void prepareFailsClosedWhenNeitherDocumentNorCallerIdentifiesTheKind();
    void currentAndPreviousReportGoldensRoundTrip();
    void migrateIsPure();
    void v2GoldenMigratesToV3Deterministically();
    void compatibleSchemaWithoutMigratorFailsClosed();
    void incompleteV2MigrationPreservesInspectionIncomplete();
    void unknownFieldsSurviveOnCompatibleMinor();
    void everyJsonKindRoundTripsItsCurrentAndPreviousGolden();
    void newerMinorPassesThroughAndReportsTheDocumentVersion();
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
    QFile resource(QStringLiteral(":/loop/schema-compatibility.json"));
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
        QFile file(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/schemas/") + name);
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

void SchemaEvolutionTest::v2GoldenMigratesToV3Deterministically()
{
    QFile file(QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/schemas/preflight-report-v2.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject source = QJsonDocument::fromJson(file.readAll()).object();

    const pdf::PDFSchemaMigrationResult first = pdf::prepareSchemaDocument(pdf::PDFSchemaKind::PreflightReport, source);
    QVERIFY(first.migrated);
    QCOMPARE(int(first.toVersion.major), 3);
    QCOMPARE(first.document.value(QStringLiteral("schema_version")).toInt(), 3);
    QVERIFY(first.document.contains(QStringLiteral("verdict")));
    QVERIFY(first.document.value(QStringLiteral("inspection_complete")).toBool());

    const pdf::PDFSchemaMigrationResult second = pdf::prepareSchemaDocument(pdf::PDFSchemaKind::PreflightReport, source);
    QCOMPARE(QJsonDocument(second.document).toJson(QJsonDocument::Compact),
             QJsonDocument(first.document).toJson(QJsonDocument::Compact));
}

void SchemaEvolutionTest::compatibleSchemaWithoutMigratorFailsClosed()
{
    QJsonObject document{
        { QStringLiteral("schema_kind"), QStringLiteral("history-db") },
        { QStringLiteral("schema_version"), 2 },
        { QStringLiteral("payload"), QStringLiteral("must-not-be-relabeled") },
    };

    QCOMPARE(pdf::checkSchemaCompatibility(pdf::PDFSchemaKind::HistoryDb, { 2, 0 }),
             pdf::PDFSchemaCompatibility::Compatible);

    const pdf::PDFSchemaMigrationResult prepared =
        pdf::prepareSchemaDocument(pdf::PDFSchemaKind::HistoryDb, document);
    QVERIFY(prepared.document.isEmpty());
    QVERIFY(!prepared.migrated);
    const pdf::PDFSchemaVersion expectedVersion{ 2, 0 };
    QCOMPARE(prepared.fromVersion, expectedVersion);
    // The document was not migrated, so it is still a 2.0 document -- the point
    // of the slot: it must not be relabelled to the matrix target.
    QCOMPARE(prepared.toVersion, expectedVersion);
}

void SchemaEvolutionTest::incompleteV2MigrationPreservesInspectionIncomplete()
{
    QJsonObject document{
        { QStringLiteral("schema_kind"), QStringLiteral("preflight-report") },
        { QStringLiteral("schema_version"), 2 },
        { QStringLiteral("pass"), false },
        { QStringLiteral("profile"), QStringLiteral("golden") },
        { QStringLiteral("errors"), QJsonArray{} },
        { QStringLiteral("warnings"), QJsonArray{} },
        { QStringLiteral("fixups_available"), QJsonArray{} }
    };

    const pdf::PDFSchemaMigrationResult prepared = pdf::prepareSchemaDocument(pdf::PDFSchemaKind::PreflightReport, document);
    QVERIFY(prepared.migrated);
    QCOMPARE(prepared.document.value(QStringLiteral("inspection_complete")).toBool(), false);
    QCOMPARE(prepared.document.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(),
             QStringLiteral("incomplete"));
}

void SchemaEvolutionTest::unknownFieldsSurviveOnCompatibleMinor()
{
    QJsonObject document{
        { QStringLiteral("schema_kind"), QStringLiteral("preflight-report") },
        { QStringLiteral("schema_version"), 2 },
        { QStringLiteral("pass"), true },
        { QStringLiteral("profile"), QStringLiteral("golden") },
        { QStringLiteral("errors"), QJsonArray{} },
        { QStringLiteral("warnings"), QJsonArray{} },
        { QStringLiteral("fixups_available"), QJsonArray{} },
        { QStringLiteral("future_field"), QStringLiteral("preserved") }
    };

    const pdf::PDFSchemaMigrationResult prepared = pdf::prepareSchemaDocument(pdf::PDFSchemaKind::PreflightReport, document);
    QVERIFY(prepared.migrated);
    QCOMPARE(prepared.document.value(QStringLiteral("future_field")).toString(), QStringLiteral("preserved"));
}

void SchemaEvolutionTest::currentVersionFailsClosedWithoutAMatrixEntry()
{
    // No matrix entry means no known current version. A guessed default would
    // silently relabel a document as current.
    QVERIFY(!pdf::currentSchemaVersionWithMatrix(pdf::PDFSchemaKind::PreflightReport, {}).isValid());
    QCOMPARE(pdf::currentSchemaVersionWithMatrix(pdf::PDFSchemaKind::PreflightReport, {}), pdf::PDFSchemaVersion{});

    const QJsonObject matrix{ { QStringLiteral("kinds"),
                                QJsonObject{ { QStringLiteral("preflight-report"),
                                               QJsonObject{ { QStringLiteral("current"), QStringLiteral("3.0") },
                                                            { QStringLiteral("supported_majors"), QJsonArray{ 3 } } } } } } };
    QCOMPARE(pdf::currentSchemaVersionWithMatrix(pdf::PDFSchemaKind::PreflightReport, matrix).toString(),
             QStringLiteral("3.0"));
    QVERIFY(!pdf::currentSchemaVersionWithMatrix(pdf::PDFSchemaKind::Certificate, matrix).isValid());
}

void SchemaEvolutionTest::everySchemaKindIsCoveredByTheCompatibilityMatrix()
{
    QFile resource(QStringLiteral(":/loop/schema-compatibility.json"));
    QVERIFY(resource.open(QIODevice::ReadOnly));
    const QJsonObject matrix = QJsonDocument::fromJson(resource.readAll()).object();
    const QJsonObject kinds = matrix.value(QStringLiteral("kinds")).toObject();

    QCOMPARE(kinds.keys().size(), int(pdf::AllSchemaKinds.size()));
    for (const pdf::PDFSchemaKind kind : pdf::AllSchemaKinds)
    {
        const QString name = pdf::pdfSchemaKindToString(kind);
        QVERIFY2(name != QStringLiteral("unknown"), qPrintable(name));

        const QJsonObject entry = kinds.value(name).toObject();
        QVERIFY2(!entry.isEmpty(), qPrintable(QStringLiteral("matrix is missing kind %1").arg(name)));

        const pdf::PDFSchemaVersion current = pdf::currentSchemaVersion(kind);
        QVERIFY2(current.isValid(), qPrintable(name));
        QCOMPARE(current.toString(), entry.value(QStringLiteral("current")).toString());

        bool supported = false;
        for (const QJsonValue& major : entry.value(QStringLiteral("supported_majors")).toArray())
        {
            supported = supported || (major.toInt() == int(current.major));
        }
        QVERIFY2(supported, qPrintable(QStringLiteral("supported_majors of %1 omits its current major").arg(name)));

        bool previousOk = false;
        const pdf::PDFSchemaVersion previous =
            pdf::PDFSchemaVersion::fromJsonValue(entry.value(QStringLiteral("previous")), &previousOk);
        QVERIFY2(previousOk && previous.major <= current.major,
                 qPrintable(QStringLiteral("%1 declares no usable previous version").arg(name)));
    }
}

void SchemaEvolutionTest::prepareFailsClosedWhenNeitherDocumentNorCallerIdentifiesTheKind()
{
    QJsonObject document{
        { QStringLiteral("schema_version"), 3 },
        { QStringLiteral("pass"), true },
        { QStringLiteral("payload"), QStringLiteral("must-not-be-interpreted-as-a-report") },
    };

    const pdf::PDFSchemaMigrationResult prepared = pdf::prepareSchemaDocument(pdf::PDFSchemaKind::Unknown, document);
    QVERIFY(prepared.document.isEmpty());
    QVERIFY(!prepared.migrated);
}

namespace
{

/// SQLite carries its own migrations in PDFOperationHistoryStore and has no JSON
/// golden; it is covered by UnitTestsOperationHistory instead.
const QStringList& nonJsonSchemaKinds()
{
    static const QStringList kinds{ QStringLiteral("history-db") };
    return kinds;
}

QString schemaGoldenPath(const QString& kindName, const QString& versionText)
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/schemas/") + kindName +
           QStringLiteral("-v%1.json").arg(versionText.section(QLatin1Char('.'), 0, 0));
}

QJsonObject loadSchemaGolden(const QString& path, bool* opened)
{
    QFile file(path);
    *opened = file.open(QIODevice::ReadOnly);
    if (!*opened)
    {
        return QJsonObject();
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

}   // namespace

void SchemaEvolutionTest::everyJsonKindRoundTripsItsCurrentAndPreviousGolden()
{
    QFile resource(QStringLiteral(":/loop/schema-compatibility.json"));
    QVERIFY(resource.open(QIODevice::ReadOnly));
    const QJsonObject kinds = QJsonDocument::fromJson(resource.readAll()).object().value(QStringLiteral("kinds")).toObject();

    for (const pdf::PDFSchemaKind kind : pdf::AllSchemaKinds)
    {
        const QString name = pdf::pdfSchemaKindToString(kind);
        if (nonJsonSchemaKinds().contains(name))
        {
            continue;
        }

        const QJsonObject entry = kinds.value(name).toObject();
        QVERIFY2(!entry.isEmpty(), qPrintable(name));

        QStringList versions{ entry.value(QStringLiteral("current")).toString() };
        const QString previous = entry.value(QStringLiteral("previous")).toString();
        if (!previous.isEmpty() && !versions.contains(previous))
        {
            versions.append(previous);
        }

        for (const QString& versionText : versions)
        {
            const QString path = schemaGoldenPath(name, versionText);
            bool opened = false;
            const QJsonObject golden = loadSchemaGolden(path, &opened);
            QVERIFY2(opened, qPrintable(QStringLiteral("missing golden %1").arg(path)));

            const pdf::PDFSchemaEnvelope envelope = pdf::readSchemaEnvelope(golden);
            QCOMPARE(envelope.kind, kind);
            QVERIFY2(envelope.version.isValid(), qPrintable(path));
            QCOMPARE(envelope.version.toString(), versionText);
            QCOMPARE(pdf::checkSchemaCompatibility(envelope.kind, envelope.version),
                     pdf::PDFSchemaCompatibility::Compatible);

            // A golden at the current major is already at the target; an older
            // golden migrates deterministically and keeps its own fields.
            const pdf::PDFSchemaMigrationResult prepared = pdf::prepareSchemaDocument(kind, golden);
            QVERIFY2(!prepared.document.isEmpty(), qPrintable(path));
            if (envelope.version.major == pdf::currentSchemaVersion(kind).major)
            {
                QVERIFY2(!prepared.migrated, qPrintable(path));
                QCOMPARE(prepared.document, golden);
            }
            else
            {
                QVERIFY2(prepared.migrated, qPrintable(path));
                QCOMPARE(prepared.toVersion.toString(), pdf::currentSchemaVersion(kind).toString());
            }

            const pdf::PDFSchemaMigrationResult again = pdf::prepareSchemaDocument(kind, golden);
            QCOMPARE(QJsonDocument(again.document).toJson(QJsonDocument::Compact),
                     QJsonDocument(prepared.document).toJson(QJsonDocument::Compact));
        }
    }
}

void SchemaEvolutionTest::newerMinorPassesThroughAndReportsTheDocumentVersion()
{
    QJsonObject document{
        { QStringLiteral("schema_kind"), QStringLiteral("preflight-report") },
        { QStringLiteral("schema_version"), QStringLiteral("3.1") },
        { QStringLiteral("pass"), true },
        { QStringLiteral("profile"), QStringLiteral("golden") },
        { QStringLiteral("errors"), QJsonArray{} },
        { QStringLiteral("warnings"), QJsonArray{} },
        { QStringLiteral("fixups_available"), QJsonArray{} },
        { QStringLiteral("future_additive_field"), QStringLiteral("preserved") },
    };

    QCOMPARE(pdf::checkSchemaCompatibility(pdf::PDFSchemaKind::PreflightReport, { 3, 1 }),
             pdf::PDFSchemaCompatibility::Compatible);

    const pdf::PDFSchemaMigrationResult prepared = pdf::prepareSchemaDocument(pdf::PDFSchemaKind::PreflightReport, document);
    QVERIFY(!prepared.document.isEmpty());
    QVERIFY(!prepared.migrated);
    QCOMPARE(prepared.document.value(QStringLiteral("schema_version")).toString(), QStringLiteral("3.1"));
    QCOMPARE(prepared.document.value(QStringLiteral("future_additive_field")).toString(), QStringLiteral("preserved"));
    QCOMPARE(prepared.fromVersion.toString(), QStringLiteral("3.1"));
    // A caller that is told "3.0" would believe it holds a target-version payload.
    QCOMPARE(prepared.toVersion.toString(), QStringLiteral("3.1"));
}

QTEST_APPLESS_MAIN(SchemaEvolutionTest)
#include "tst_schemaevolutiontest.moc"
