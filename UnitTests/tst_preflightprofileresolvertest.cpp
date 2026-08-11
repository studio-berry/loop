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

#include "preflightprofileresolver.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

namespace
{

QJsonObject check(const QString& id, int minDpi, const QString& severity = QStringLiteral("error"))
{
    QJsonObject result{
        { QStringLiteral("id"), id },
        { QStringLiteral("severity"), severity }
    };
    result.insert(id == QStringLiteral("bleed") ? QStringLiteral("amount_pt") : QStringLiteral("min_dpi"), minDpi);
    return result;
}

QJsonObject source(const QString& id,
                  int priority,
                  const QJsonObject& selector,
                  const QJsonArray& checks)
{
    return QJsonObject{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("name"), id },
        { QStringLiteral("profile_id"), id },
        { QStringLiteral("profile_version"), QStringLiteral("1") },
        { QStringLiteral("priority"), priority },
        { QStringLiteral("selector"), selector },
        { QStringLiteral("checks"), checks }
    };
}

bool writeProfile(const QString& directory, const QString& name, const QJsonObject& profile)
{
    QFile file(QDir(directory).filePath(name + QStringLiteral(".json")));
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(QJsonDocument(profile).toJson(QJsonDocument::Compact)) >= 0;
}

} // namespace

class PreflightProfileResolverTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesContextAndMergesByCheckId();
    void sourceOrderDoesNotChangeHash();
    void equalAuthorityConflictFailsClosed();
    void explicitProfileProducesProvenance();
    void rejectsUnknownSelectorField();
};

void PreflightProfileResolverTest::normalizesContextAndMergesByCheckId()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(writeProfile(directory.path(), QStringLiteral("default"), source(
        QStringLiteral("global.default"), 0, {}, QJsonArray{ check(QStringLiteral("image-resolution"), 150) })));
    QVERIFY(writeProfile(directory.path(), QStringLiteral("press"), source(
        QStringLiteral("press.hp"), 100, {{ QStringLiteral("press_id"), QStringLiteral("HP-15K") }},
        QJsonArray{ check(QStringLiteral("image-resolution"), 200), check(QStringLiteral("bleed"), 3) })));
    QVERIFY(writeProfile(directory.path(), QStringLiteral("client"), source(
        QStringLiteral("client.acme"), 200, {{ QStringLiteral("client_id"), QStringLiteral("ACME") }},
        QJsonArray{ check(QStringLiteral("bleed"), 9, QStringLiteral("warning")) })));

    pdf::PreflightProfileSnapshot snapshot;
    QString error;
    QVERIFY(pdf::PreflightProfileStore::loadDirectory(directory.path(), snapshot, error));

    pdf::PreflightJobContext context;
    context.clientId = QStringLiteral(" Acme ");
    context.pressId = QStringLiteral("hp-15k");
    const pdf::PreflightResolvedProfile result = pdf::PreflightProfileResolver().resolve(context, snapshot);
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.normalizedContext.value(QStringLiteral("client_id")).toString(), QStringLiteral("acme"));
    QCOMPARE(result.effectiveProfile.value(QStringLiteral("checks")).toArray().size(), 2);
    const QJsonArray checks = result.effectiveProfile.value(QStringLiteral("checks")).toArray();
    QCOMPARE(checks.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("bleed"));
    QCOMPARE(checks.at(0).toObject().value(QStringLiteral("amount_pt")).toDouble(), 9.0);
    QCOMPARE(checks.at(1).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("image-resolution"));
    QCOMPARE(checks.at(1).toObject().value(QStringLiteral("min_dpi")).toInt(), 200);
    QVERIFY(result.provenance().value(QStringLiteral("decisions")).toArray().size() >= 4);
}

void PreflightProfileResolverTest::sourceOrderDoesNotChangeHash()
{
    const QJsonObject defaultProfile = source(QStringLiteral("global.default"), 0, {}, QJsonArray{ check(QStringLiteral("bleed"), 3) });
    const QJsonObject pressProfile = source(QStringLiteral("press.hp"), 100,
                                             {{ QStringLiteral("press_id"), QStringLiteral("hp") }},
                                             QJsonArray{ check(QStringLiteral("bleed"), 6) });

    QString error;
    // Build snapshots through the public store boundary so all source metadata
    // (identity, selector, and content hash) is exercised by the test.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(writeProfile(directory.path(), QStringLiteral("default"), defaultProfile));
    QVERIFY(writeProfile(directory.path(), QStringLiteral("press"), pressProfile));
    pdf::PreflightProfileSnapshot snapshot;
    QVERIFY(pdf::PreflightProfileStore::loadDirectory(directory.path(), snapshot, error));
    QVERIFY(snapshot.sources.size() == 2);

    pdf::PreflightJobContext context;
    context.pressId = QStringLiteral("HP");
    const pdf::PreflightResolvedProfile firstResult = pdf::PreflightProfileResolver().resolve(context, snapshot);
    std::reverse(snapshot.sources.begin(), snapshot.sources.end());
    const pdf::PreflightResolvedProfile secondResult = pdf::PreflightProfileResolver().resolve(context, snapshot);
    QVERIFY(firstResult.ok);
    QVERIFY(secondResult.ok);
    QCOMPARE(firstResult.effectiveHash, secondResult.effectiveHash);
    QCOMPARE(pdf::canonicalPreflightJson(firstResult.effectiveProfile),
             pdf::canonicalPreflightJson(secondResult.effectiveProfile));
}

void PreflightProfileResolverTest::equalAuthorityConflictFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(writeProfile(directory.path(), QStringLiteral("a"), source(
        QStringLiteral("client.a"), 100, {{ QStringLiteral("client_id"), QStringLiteral("acme") }},
        QJsonArray{ check(QStringLiteral("bleed"), 3) })));
    QVERIFY(writeProfile(directory.path(), QStringLiteral("b"), source(
        QStringLiteral("client.b"), 100, {{ QStringLiteral("client_id"), QStringLiteral("acme") }},
        QJsonArray{ check(QStringLiteral("bleed"), 9) })));

    pdf::PreflightProfileSnapshot snapshot;
    QString error;
    QVERIFY(pdf::PreflightProfileStore::loadDirectory(directory.path(), snapshot, error));
    pdf::PreflightJobContext context;
    context.clientId = QStringLiteral("acme");
    const pdf::PreflightResolvedProfile result = pdf::PreflightProfileResolver().resolve(context, snapshot);
    QVERIFY(!result.ok);
    QCOMPARE(result.errorCode, QStringLiteral("ambiguous-profile"));
}

void PreflightProfileResolverTest::explicitProfileProducesProvenance()
{
    const QJsonObject profile = source(QStringLiteral("ci.profile"), 0, {}, QJsonArray{ check(QStringLiteral("bleed"), 3) });
    const pdf::PreflightResolvedProfile result = pdf::PreflightProfileResolver().resolveExplicitProfile(profile, QStringLiteral("ci"), QStringLiteral("42"));
    QVERIFY(result.ok);
    QCOMPARE(result.resolutionMode, QStringLiteral("explicit"));
    QCOMPARE(result.matchedSources.size(), 1);
    QCOMPARE(result.matchedSources.first().id, QStringLiteral("ci"));
    QCOMPARE(result.matchedSources.first().version, QStringLiteral("42"));
    QCOMPARE(result.provenance().value(QStringLiteral("mode")).toString(), QStringLiteral("explicit"));
    QVERIFY(!result.effectiveHash.isEmpty());
}

void PreflightProfileResolverTest::rejectsUnknownSelectorField()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(writeProfile(directory.path(), QStringLiteral("invalid"), source(
        QStringLiteral("invalid"), 0, {{ QStringLiteral("arbitrary_expression"), QStringLiteral("x") }},
        QJsonArray{ check(QStringLiteral("bleed"), 3) })));

    pdf::PreflightProfileSnapshot snapshot;
    QString error;
    QVERIFY(!pdf::PreflightProfileStore::loadDirectory(directory.path(), snapshot, error));
    QVERIFY(error.contains(QStringLiteral("not supported")));
}

QTEST_APPLESS_MAIN(PreflightProfileResolverTest)

#include "tst_preflightprofileresolvertest.moc"
