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

#include "pdfdocumentbuilder.h"
#include "pdfdocumentsession.h"
#include "pdfpreflightverdict.h"
#include "preflightengine.h"
#include "preflightprofileresolver.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

class PreflightProfileIdentityTest : public QObject
{
    Q_OBJECT

private slots:
    void digestIgnoresFormattingAndDefaults();
    void digestChangesWithThresholds();
    void unknownCheckKeyChangesDigest();
    void importRejectsDigestMismatch();
    void legacyProfileIsProvisional();
    void forkRecordsDerivedFrom();
    void variableWholeValuePreservesNumber();
    void undeclaredVariableIsIncomplete();
    void cliOverridesDefault();
    void outOfRangeBindingFailsClosed();
    void emptyPagesScopeIsIncomplete();
    void unsupportedRegionFailsClosed();
    void bundledProfilesHaveMatchingDigests();
    void bundledDefaultProfileRuns();
    void engineRunAppliesCliBindings();
    void mergedEffectiveProfileOmitsDigest();
    void registryMatchesGeneratedCatalog();
};

namespace
{

QJsonObject baseProfile()
{
    return QJsonObject{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("id"), QStringLiteral("test-profile") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Test") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 300 },
                                        { QStringLiteral("severity"), QStringLiteral("warning") } } } }
    };
}

pdf::PDFDocument emptyPageDocument()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    return builder.build();
}

}   // namespace

void PreflightProfileIdentityTest::digestIgnoresFormattingAndDefaults()
{
    QJsonObject first = baseProfile();
    first.insert(QStringLiteral("description"), QStringLiteral("cosmetic"));
    first.insert(QStringLiteral("authored"), QJsonObject{ { QStringLiteral("by"), QStringLiteral("test") } });
    QJsonObject second = baseProfile();
    QJsonArray checks = second.value(QStringLiteral("checks")).toArray();
    QJsonObject check = checks.at(0).toObject();
    check.insert(QStringLiteral("enabled"), true);
    checks.replace(0, check);
    second.insert(QStringLiteral("checks"), checks);
    QCOMPARE(pdf::computeProfileDigest(first), pdf::computeProfileDigest(second));
}

void PreflightProfileIdentityTest::digestChangesWithThresholds()
{
    const QString baseline = pdf::computeProfileDigest(baseProfile());
    QJsonObject changed = baseProfile();
    QJsonArray checks = changed.value(QStringLiteral("checks")).toArray();
    QJsonObject check = checks.at(0).toObject();
    check.insert(QStringLiteral("min_dpi"), 150);
    checks.replace(0, check);
    changed.insert(QStringLiteral("checks"), checks);
    QVERIFY(pdf::computeProfileDigest(changed) != baseline);
}

void PreflightProfileIdentityTest::unknownCheckKeyChangesDigest()
{
    const QString baseline = pdf::computeProfileDigest(baseProfile());
    QJsonObject changed = baseProfile();
    QJsonArray checks = changed.value(QStringLiteral("checks")).toArray();
    QJsonObject check = checks.at(0).toObject();
    check.insert(QStringLiteral("forward_compat_key"), true);
    checks.replace(0, check);
    changed.insert(QStringLiteral("checks"), checks);
    QVERIFY(pdf::computeProfileDigest(changed) != baseline);
}

void PreflightProfileIdentityTest::importRejectsDigestMismatch()
{
    QJsonObject profile = baseProfile();
    profile.insert(QStringLiteral("digest"), QString(64, QLatin1Char('a')));
    const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(profile);
    QVERIFY(!imported.ok);
    QCOMPARE(imported.errorCode, QStringLiteral("profile-digest-mismatch"));
}

void PreflightProfileIdentityTest::legacyProfileIsProvisional()
{
    QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Legacy") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
                                        { QStringLiteral("id"), QStringLiteral("image-resolution") },
                                        { QStringLiteral("min_dpi"), 300 } } } }
    };
    const pdf::PreflightProfileIdentity identity = pdf::identifyPreflightProfile(profile, QStringLiteral("/tmp/legacy-job.json"));
    QVERIFY(identity.provisional);
    QCOMPARE(identity.version, QStringLiteral("0.0.0"));
    QCOMPARE(identity.id, QStringLiteral("legacy-job"));
}

void PreflightProfileIdentityTest::forkRecordsDerivedFrom()
{
    const QJsonObject forked = pdf::forkPreflightProfile(baseProfile(), QStringLiteral("forked-profile"), QStringLiteral("1.1.0"));
    QCOMPARE(forked.value(QStringLiteral("id")).toString(), QStringLiteral("forked-profile"));
    QCOMPARE(forked.value(QStringLiteral("derived_from")).toObject().value(QStringLiteral("digest")).toString(),
             pdf::computeProfileDigest(baseProfile()));
    QVERIFY(!forked.value(QStringLiteral("digest")).toString().isEmpty());
}

void PreflightProfileIdentityTest::variableWholeValuePreservesNumber()
{
    QJsonObject profile = baseProfile();
    profile.insert(QStringLiteral("variables"), QJsonObject{
                                                    { QStringLiteral("min_dpi"), QJsonObject{
                                                                                     { QStringLiteral("type"), QStringLiteral("number") },
                                                                                     { QStringLiteral("default"), 300 },
                                                                                     { QStringLiteral("min"), 72 },
                                                                                     { QStringLiteral("max"), 1200 } } } });
    QJsonArray checks = profile.value(QStringLiteral("checks")).toArray();
    QJsonObject check = checks.at(0).toObject();
    check.insert(QStringLiteral("min_dpi"), QStringLiteral("${min_dpi}"));
    checks.replace(0, check);
    profile.insert(QStringLiteral("checks"), checks);

    const pdf::PreflightVariableBindResult bound = pdf::bindPreflightProfileVariables(profile, {}, QJsonObject{ { QStringLiteral("min_dpi"), 150 } });
    QVERIFY2(bound.ok, qPrintable(bound.errorMessage));
    const QJsonObject boundCheck = bound.profile.value(QStringLiteral("checks")).toArray().at(0).toObject();
    QVERIFY(boundCheck.value(QStringLiteral("min_dpi")).isDouble());
    QCOMPARE(boundCheck.value(QStringLiteral("min_dpi")).toInt(), 150);
}

void PreflightProfileIdentityTest::undeclaredVariableIsIncomplete()
{
    QJsonObject profile = baseProfile();
    QJsonArray checks = profile.value(QStringLiteral("checks")).toArray();
    QJsonObject check = checks.at(0).toObject();
    check.insert(QStringLiteral("min_dpi"), QStringLiteral("${missing}"));
    checks.replace(0, check);
    profile.insert(QStringLiteral("checks"), checks);

    pdf::PDFDocument document = emptyPageDocument();
    pdf::PDFDocumentSession session(&document);
    const pdf::PreflightResult result = pdf::PreflightEngine(&session).run(profile);
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(result.errorCode, QStringLiteral("unresolved-variable"));
    QVERIFY(!verdict.isPass());
}

void PreflightProfileIdentityTest::cliOverridesDefault()
{
    QJsonObject profile = baseProfile();
    profile.insert(QStringLiteral("variables"), QJsonObject{
                                                    { QStringLiteral("min_dpi"), QJsonObject{
                                                                                     { QStringLiteral("type"), QStringLiteral("number") },
                                                                                     { QStringLiteral("default"), 300 } } } });
    QJsonArray checks = profile.value(QStringLiteral("checks")).toArray();
    QJsonObject check = checks.at(0).toObject();
    check.insert(QStringLiteral("min_dpi"), QStringLiteral("${min_dpi}"));
    checks.replace(0, check);
    profile.insert(QStringLiteral("checks"), checks);

    const pdf::PreflightVariableBindResult bound = pdf::bindPreflightProfileVariables(profile,
                                                                                      QJsonObject{ { QStringLiteral("min_dpi"), 200 } },
                                                                                      QJsonObject{ { QStringLiteral("min_dpi"), 120 } });
    QVERIFY(bound.ok);
    QCOMPARE(bound.profile.value(QStringLiteral("checks")).toArray().at(0).toObject().value(QStringLiteral("min_dpi")).toInt(), 120);
    QCOMPARE(bound.bindings.at(0).toObject().value(QStringLiteral("source")).toString(), QStringLiteral("cli"));
}

void PreflightProfileIdentityTest::outOfRangeBindingFailsClosed()
{
    QJsonObject profile = baseProfile();
    profile.insert(QStringLiteral("variables"), QJsonObject{
                                                    { QStringLiteral("min_dpi"), QJsonObject{
                                                                                     { QStringLiteral("type"), QStringLiteral("number") },
                                                                                     { QStringLiteral("default"), 300 },
                                                                                     { QStringLiteral("max"), 600 } } } });
    const pdf::PreflightVariableBindResult bound = pdf::bindPreflightProfileVariables(profile, {}, QJsonObject{ { QStringLiteral("min_dpi"), 9000 } });
    QVERIFY(!bound.ok);
    QCOMPARE(bound.errorCode, QStringLiteral("unresolved-variable"));
}

void PreflightProfileIdentityTest::emptyPagesScopeIsIncomplete()
{
    QJsonObject profile = baseProfile();
    profile.insert(QStringLiteral("restrictions"), QJsonObject{ { QStringLiteral("pages"), QStringLiteral("9") } });
    pdf::PDFDocument document = emptyPageDocument();
    pdf::PDFDocumentSession session(&document);
    const pdf::PreflightResult result = pdf::PreflightEngine(&session).run(profile);
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(result.errorCode, QStringLiteral("unsupported-scope"));
}

void PreflightProfileIdentityTest::unsupportedRegionFailsClosed()
{
    QJsonObject profile = baseProfile();
    profile.insert(QStringLiteral("restrictions"), QJsonObject{
                                                       { QStringLiteral("regions"), QJsonArray{ QJsonObject{
                                                                                        { QStringLiteral("name"), QStringLiteral("slug") },
                                                                                        { QStringLiteral("rect_pt"), QJsonArray{ 0, 0, 10, 10 } } } } } });
    pdf::PDFDocument document = emptyPageDocument();
    pdf::PDFDocumentSession session(&document);
    const pdf::PreflightResult result = pdf::PreflightEngine(&session).run(profile);
    QCOMPARE(pdf::reducePreflightVerdict(result).state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(result.errorCode, QStringLiteral("unsupported-scope"));
}

void PreflightProfileIdentityTest::bundledProfilesHaveMatchingDigests()
{
    const QDir directory(QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/profiles"));
    const QFileInfoList files = directory.entryInfoList({ QStringLiteral("*.json") }, QDir::Files);
    QVERIFY(!files.isEmpty());
    for (const QFileInfo& info : files)
    {
        QFile file(info.absoluteFilePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
        const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(profile, info.absoluteFilePath());
        QVERIFY2(imported.ok, qPrintable(info.fileName() + QLatin1Char(' ') + imported.errorMessage));
        QVERIFY2(!imported.identity.digest.isEmpty(), qPrintable(info.fileName()));
        if (profile.contains(QStringLiteral("digest")))
        {
            QCOMPARE(profile.value(QStringLiteral("digest")).toString(), imported.identity.digest);
        }
    }
}

void PreflightProfileIdentityTest::bundledDefaultProfileRuns()
{
    QFile file(QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/profiles/loupe-default.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
    pdf::PDFDocument document = emptyPageDocument();
    pdf::PDFDocumentSession session(&document);
    const pdf::PreflightResult result = pdf::PreflightEngine(&session).run(profile);
    QVERIFY(result.errorCode != QStringLiteral("profile-digest-mismatch"));
    QCOMPARE(result.profileIdentity.value(QStringLiteral("digest")).toString(), profile.value(QStringLiteral("digest")).toString());
}

void PreflightProfileIdentityTest::engineRunAppliesCliBindings()
{
    QJsonObject profile = baseProfile();
    profile.insert(QStringLiteral("variables"), QJsonObject{
                                                    { QStringLiteral("min_dpi"), QJsonObject{
                                                                                     { QStringLiteral("type"), QStringLiteral("number") },
                                                                                     { QStringLiteral("default"), 300 } } } });
    QJsonArray checks = profile.value(QStringLiteral("checks")).toArray();
    QJsonObject check = checks.at(0).toObject();
    check.insert(QStringLiteral("min_dpi"), QStringLiteral("${min_dpi}"));
    checks.replace(0, check);
    profile.insert(QStringLiteral("checks"), checks);

    pdf::PDFDocument document = emptyPageDocument();
    pdf::PDFDocumentSession session(&document);
    const pdf::PreflightResult result = pdf::PreflightEngine(&session).run(profile, {}, QJsonObject{ { QStringLiteral("min_dpi"), 150 } });
    QVERIFY(result.errorCode != QStringLiteral("unresolved-variable"));
    QVERIFY(result.errorCode != QStringLiteral("invalid-explicit-profile"));
    QCOMPARE(result.variableBindings.at(0).toObject().value(QStringLiteral("source")).toString(), QStringLiteral("cli"));
    QCOMPARE(result.variableBindings.at(0).toObject().value(QStringLiteral("value")).toInt(), 150);
}

void PreflightProfileIdentityTest::mergedEffectiveProfileOmitsDigest()
{
    QFile file(QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/profiles/loupe-default.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject profile = QJsonDocument::fromJson(file.readAll()).object();
    QVERIFY(profile.contains(QStringLiteral("digest")));
    const pdf::PreflightResolvedProfile resolved = pdf::PreflightProfileResolver().resolveExplicitProfile(profile, QStringLiteral("loupe-default"), QStringLiteral("1.0.0"));
    QVERIFY2(resolved.ok, qPrintable(resolved.errorMessage));
    QVERIFY(!resolved.effectiveProfile.contains(QStringLiteral("digest")));
}

void PreflightProfileIdentityTest::registryMatchesGeneratedCatalog()
{
    QFile file(QStringLiteral(LOUPE_SOURCE_DIR "/docs/generated/preflight-check-catalog.json"));
    QVERIFY2(file.open(QIODevice::ReadOnly), "generated preflight check catalog is missing");
    const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject checks = catalog.value(QStringLiteral("checks")).toObject();
    QVERIFY(!checks.isEmpty());
    pdf::PreflightEngine engine(nullptr);
    for (auto it = checks.constBegin(); it != checks.constEnd(); ++it)
    {
        QVERIFY2(engine.hasCheck(it.key()), qPrintable(it.key()));
    }
    for (const QJsonValue& id : catalog.value(QStringLiteral("registry")).toArray())
    {
        QVERIFY2(checks.contains(id.toString()), qPrintable(id.toString()));
    }
}

QTEST_APPLESS_MAIN(PreflightProfileIdentityTest)
#include "tst_preflightprofileidentitytest.moc"
