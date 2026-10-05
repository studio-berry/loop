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

#include "pdfartifactidentity.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"
#include "pdfgovernedexecution.h"
#include "pdfrepairoperation.h"
#include "pdfsafefilewriter.h"
#include "pdfsavepolicy.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

QString sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

/// Cancellation double that only reports cancelled once a test trips it. The
/// preview seam flips it after the first rendered page so the render loop sees
/// the cancel on the next page, exactly like an operator pressing stop.
class TrippedCancelControl final : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override { return m_cancelled; }
    void trip() const { m_cancelled = true; }

private:
    mutable bool m_cancelled = false;
};

bool readsAsValidPdf(const QString& path)
{
    pdf::PDFDocumentReader reader(nullptr, [](bool*)
                                  { return QString(); }, false, false);
    reader.readFromFile(path);
    return reader.getReadingResult() == pdf::PDFDocumentReader::Result::OK;
}

QString fileSha256Hex(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return QString();
    }
    return sha256Hex(file.readAll());
}

QString goldenVectorPath(const QString& fileName)
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/../UnitTests/testdata/canonical-json/%1").arg(fileName);
}

QJsonObject loadGoldenObject(const QString& fileName)
{
    QFile file(goldenVectorPath(fileName));
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.object();
}

}   // namespace

class GovernedExecutionTest final : public QObject
{
    Q_OBJECT

private slots:
    void planDigest_isDeterministicAndSensitive();
    void technicalAndVisualPreviewsStaySeparate();
    void preflightDecisionIsNotOperationApproval();
    void publishRequiresMatchingPlanBoundApproval();
    void publishRejectsPreflightDecisionReference();
    void publishRejectsNoneApprovalKind();
    void publishRejectsExplicitRejectDecision();
    void visualPreviewRequiredForContentChangingRepairs();
    void publishedBytesAreRevalidatedAndSignedOff();

    // D1 — canonical JSON byte format
    void canonicalJson_keyOrderIsStableAcrossInsertion();
    void canonicalJson_goldenVectorsMatchPinnedDigests();
    void canonicalJson_absentKeysDifferFromNull();
    void canonicalJson_scalarRootsAreWrapped();

    // Registry identity bound into the plan digest (L04-01 / #33)
    void registryDigest_isStableLowercaseHex();
    void planDigest_matchesPinnedGoldenVector();

    // D2 — preview has no publication authority
    void previewDoesNotPublishWithoutGovernedGateway();

    // D3 — destination binding separate from semantic plans
    void planDigest_excludesDestinationPath();
    void destinationOverwriteFailIsFailClosed();
    void staleDestinationBindingRejectsPublication();

    // D4 — publication vs durable completion
    void publicationWithoutSignOffIsNotDurableCompletion();

    // D5 — cross-surface identity equality (#656 disposition)
    void crossSurfaceEquality_isPlanIdentityNotIndependentPdfBytes();

    // L04-03 / #35 — isolated, nonpublishing, plan-bound preview
    void previewLeavesSourceBytesUntouched();
    void cancelledPreviewLeavesNoArtifacts();
    void previewRefusesPlanDigestMismatch();
    void failedPreviewPreservesCallerFiles();
    void cancelledFinalPageLeavesNoArtifacts();
    void previewRefusesInvalidatedPlan();
};

void GovernedExecutionTest::planDigest_isDeterministicAndSensitive()
{
    pdf::PDFRepairPlan plan;
    plan.operationId = QStringLiteral("add-bleed");
    plan.parameters = QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 } };
    const QString sourceSha256 = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const pdf::PDFOperationSavePolicy savePolicy = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("test"));

    const QString first = pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy);
    const QString second = pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy);
    QCOMPARE(first, second);

    plan.parameters.insert(QStringLiteral("bleed_mm"), 4.0);
    QVERIFY(pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy) != first);
}

void GovernedExecutionTest::technicalAndVisualPreviewsStaySeparate()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    const QString sourceSha256 = QString::fromLatin1(source.getSourceDataHash().toHex());

    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(registry.find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), sourceSha256, transaction.savePolicy());

    pdf::PDFTechnicalPreview technicalPreview;
    QVERIFY(pdf::buildTechnicalPreview(transaction, candidatePath, planDigest, &technicalPreview));
    pdf::PDFVisualPreview visualPreview;
    pdf::PDFRepairDiffOptions options;
    QVERIFY(pdf::buildVisualPreview(transaction, candidatePath, planDigest, options, &visualPreview));

    QCOMPARE(technicalPreview.toJson().value(QStringLiteral("schema")).toString(), QStringLiteral("loop.technical-preview"));
    QCOMPARE(visualPreview.toJson().value(QStringLiteral("schema")).toString(), QStringLiteral("loop.visual-preview"));
    QVERIFY(technicalPreview.toJson().contains(QStringLiteral("structural_changes")));
    QVERIFY(visualPreview.toJson().contains(QStringLiteral("pages")));
    QVERIFY(!technicalPreview.toJson().contains(QStringLiteral("pages")));
    QVERIFY(!visualPreview.toJson().contains(QStringLiteral("structural_changes")));
    QCOMPARE(technicalPreview.planDigest, planDigest);
    QCOMPARE(visualPreview.planDigest, planDigest);
}

void GovernedExecutionTest::preflightDecisionIsNotOperationApproval()
{
    pdf::PreflightDecision decision;
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("Client approved supplied art.");
    QVERIFY(!pdf::preflightDecisionQualifiesAsOperationApproval(decision));
}

void GovernedExecutionTest::publishRequiresMatchingPlanBoundApproval()
{
    const QByteArray candidateBytes("governed candidate bytes");
    const QString sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateBytes, QCryptographicHash::Sha256).toHex());
    const QString planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = candidateSha256;
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.decision = QStringLiteral("approve");
    approval.approval.rationale = QStringLiteral("Reviewed previews.");
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));

    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          QStringLiteral("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"),
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite));

    QVERIFY(pdf::publishGovernedArtifact(approval,
                                         planDigest,
                                         sourceSha256,
                                         candidateBytes,
                                         outputPath,
                                         pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite));
    QFile output(outputPath);
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), candidateBytes);
}

void GovernedExecutionTest::publishRejectsNoneApprovalKind()
{
    const QByteArray candidateBytes("governed candidate bytes");
    const QString sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateBytes, QCryptographicHash::Sha256).toHex());
    const QString planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = candidateSha256;
    QVERIFY(approval.isValid());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));
    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite));
}

void GovernedExecutionTest::publishRejectsExplicitRejectDecision()
{
    const QByteArray candidateBytes("governed candidate bytes");
    const QString sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateBytes, QCryptographicHash::Sha256).toHex());
    const QString planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = candidateSha256;
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.decision = QStringLiteral("reject");
    approval.approval.rationale = QStringLiteral("Unexpected visual drift.");
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));
    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite));
}

void GovernedExecutionTest::publishedBytesAreRevalidatedAndSignedOff()
{
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Governed smoke") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("font-integrity") }, { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString publishedPath = temporary.filePath(QStringLiteral("published.pdf"));
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    QVERIFY(writer.write(publishedPath, &document, true));
    QFile publishedFile(publishedPath);
    QVERIFY(publishedFile.open(QIODevice::ReadOnly));
    const QByteArray publishedBytes = publishedFile.readAll();
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(publishedBytes, QCryptographicHash::Sha256).toHex());

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    approval.sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    approval.candidateSha256 = candidateSha256;
    approval.approval.kind = pdf::PDFApprovalKind::Policy;
    approval.approval.actorId = QStringLiteral("test-policy");
    approval.approval.decision = QStringLiteral("approve");
    approval.approval.policyId = QStringLiteral("test");
    approval.approval.rationale = QStringLiteral("The governed candidate was reviewed.");
    approval.approval.evidenceSha256 = approval.planDigest;
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    pdf::PDFGovernedExecutionRevalidation revalidation;
    pdf::PDFGovernedExecutionSignOff signOff;
    const pdf::PDFOperationResult finalizeResult = pdf::finalizeGovernedPublication(approval,
                                                                                    approval.planDigest,
                                                                                    approval.sourceSha256,
                                                                                    approval.candidateSha256,
                                                                                    publishedPath,
                                                                                    profile,
                                                                                    QStringLiteral("test-certificate"),
                                                                                    QStringLiteral("test-revalidation"),
                                                                                    &revalidation,
                                                                                    &signOff);
    QVERIFY2(finalizeResult, qPrintable(finalizeResult.getErrorMessage()));
    QVERIFY(revalidation.bytesVerified);
    QVERIFY(revalidation.isSignOffEligible());
    QCOMPARE(revalidation.artifactSha256, candidateSha256);
    QVERIFY(signOff.isValid());
    QCOMPARE(signOff.publishedSha256, candidateSha256);
    QVERIFY(pdf::validateGovernedSignOff(signOff,
                                         approval,
                                         revalidation,
                                         approval.planDigest,
                                         approval.sourceSha256,
                                         approval.candidateSha256));
}

void GovernedExecutionTest::visualPreviewRequiredForContentChangingRepairs()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    const QString sourceSha256 = QString::fromLatin1(source.getSourceDataHash().toHex());

    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(registry.find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(pdf::repairPlansMutatePageContent(transaction.plans()));
    QVERIFY(transaction.apply());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), sourceSha256, transaction.savePolicy());

    pdf::PDFVisualPreview visualPreview;
    pdf::PDFRepairDiffOptions options;
    QVERIFY(pdf::buildVisualPreview(transaction, candidatePath, planDigest, options, &visualPreview));
    QCOMPARE(visualPreview.planDigest, planDigest);
    QVERIFY(!visualPreview.pages.isEmpty());
    QVERIFY(visualPreview.status == pdf::PDFRepairDiffStatus::Complete ||
            visualPreview.status == pdf::PDFRepairDiffStatus::CompleteWithWarnings);
}

void GovernedExecutionTest::publishRejectsPreflightDecisionReference()
{
    const QByteArray candidateBytes("governed candidate bytes");
    const QString sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const QString candidateSha256 = QString::fromLatin1(QCryptographicHash::hash(candidateBytes, QCryptographicHash::Sha256).toHex());
    const QString planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = candidateSha256;
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.decision = QStringLiteral("approve");
    approval.approval.decisionReference = QStringLiteral("preflight-decision:a3f19c22");
    approval.approval.rationale = QStringLiteral("Waived finding.");
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));
    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite));
}

void GovernedExecutionTest::canonicalJson_keyOrderIsStableAcrossInsertion()
{
    const QJsonObject first{ { QStringLiteral("z"), 1 }, { QStringLiteral("a"), 2 }, { QStringLiteral("m"), 3 } };
    const QJsonObject second{ { QStringLiteral("m"), 3 }, { QStringLiteral("a"), 2 }, { QStringLiteral("z"), 1 } };
    QCOMPARE(pdf::canonicalJson(first), pdf::canonicalJson(second));
    QCOMPARE(sha256Hex(pdf::canonicalJson(first)), sha256Hex(pdf::canonicalJson(second)));
}

void GovernedExecutionTest::canonicalJson_goldenVectorsMatchPinnedDigests()
{
    QFile expectedFile(goldenVectorPath(QStringLiteral("expected-digests.json")));
    QVERIFY2(expectedFile.open(QIODevice::ReadOnly), "expected-digests.json must exist");
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    QVERIFY(!expected.isEmpty());

    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
    {
        const QJsonObject vector = loadGoldenObject(it.key());
        QVERIFY2(!vector.isEmpty(), qPrintable(it.key()));
        const QByteArray canonical = pdf::canonicalJson(vector);
        QCOMPARE(sha256Hex(canonical), it.value().toString());
    }
}

void GovernedExecutionTest::canonicalJson_absentKeysDifferFromNull()
{
    const QJsonObject withNull{ { QStringLiteral("value"), QJsonValue() } };
    const QJsonObject withoutKey;
    QVERIFY(pdf::canonicalJson(withNull) != pdf::canonicalJson(withoutKey));
}

void GovernedExecutionTest::canonicalJson_scalarRootsAreWrapped()
{
    const QByteArray canonical = pdf::canonicalJson(QJsonValue(true));
    QCOMPARE(canonical, QByteArrayLiteral("[true]"));
}

void GovernedExecutionTest::registryDigest_isStableLowercaseHex()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    const QString first = registry.digest();
    QCOMPARE(first, registry.digest());
    QCOMPARE(first.size(), 64);
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(first).hasMatch());

    // Registry identity is content-derived: two empty registries agree, and an
    // empty registry differs from the built-in set.
    pdf::PDFRepairRegistry isolated;
    pdf::PDFRepairRegistry another;
    QCOMPARE(isolated.digest(), another.digest());
    QVERIFY(isolated.digest() != first);
}

void GovernedExecutionTest::planDigest_matchesPinnedGoldenVector()
{
    // Reproducibility contract for expected-plan-digests.json: two version-1
    // plans — add-bleed {"bleed_mm": 3.0, "force": true} and
    // production.validate-wide-format {"geometry": {}} — source SHA-256
    // "aa...a" (64 characters), save policy saveAsNewArtifact("golden vector")
    // on both plans and the envelope, and the built-in registry digest.
    pdf::PDFRepairPlan primitive;
    primitive.operationId = QStringLiteral("add-bleed");
    primitive.parameters = QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 },
                                        { QStringLiteral("force"), true } };
    pdf::PDFRepairPlan production;
    production.operationId = QStringLiteral("production.validate-wide-format");
    production.parameters = QJsonObject{ { QStringLiteral("geometry"), QJsonObject() } };
    const QString sourceSha256 = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const pdf::PDFOperationSavePolicy savePolicy = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("golden vector"));
    primitive.savePolicy = savePolicy;
    production.savePolicy = savePolicy;

    const QJsonObject expected = loadGoldenObject(QStringLiteral("expected-plan-digests.json"));
    QVERIFY2(!expected.isEmpty(), "expected-plan-digests.json must exist");
    const QString pinned = expected.value(QStringLiteral("plan_digest")).toString();
    QVERIFY(!pinned.isEmpty());

    QCOMPARE(pdf::computeOperationPlanDigest({ primitive, production }, sourceSha256, savePolicy), pinned);
}

void GovernedExecutionTest::previewDoesNotPublishWithoutGovernedGateway()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    const QString sourceSha256 = QString::fromLatin1(source.getSourceDataHash().toHex());

    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(registry.find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));
    const QString publicationPath = temporary.filePath(QStringLiteral("published.pdf"));
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), sourceSha256, transaction.savePolicy());

    pdf::PDFTechnicalPreview technicalPreview;
    QVERIFY(pdf::buildTechnicalPreview(transaction, candidatePath, planDigest, &technicalPreview));
    QVERIFY(QFile::exists(candidatePath));
    QVERIFY(!QFile::exists(publicationPath));

    const QByteArray candidateBytes = [&candidatePath]()
    {
        QFile file(candidatePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        return file.readAll();
    }();
    QVERIFY(!candidateBytes.isEmpty());

    pdf::PDFGovernedExecutionApproval missingApproval;
    QVERIFY(!pdf::publishGovernedArtifact(missingApproval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          publicationPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Fail));
    QVERIFY(!QFile::exists(publicationPath));
}

void GovernedExecutionTest::planDigest_excludesDestinationPath()
{
    pdf::PDFRepairPlan plan;
    plan.operationId = QStringLiteral("add-bleed");
    plan.parameters = QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 } };
    const QString sourceSha256 = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const pdf::PDFOperationSavePolicy savePolicy = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("test"));

    const QString digest = pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy);
    const QJsonObject envelope{
        { QStringLiteral("schema_kind"), QStringLiteral("operation-plan") },
        { QStringLiteral("schema_version"), QStringLiteral("1.0") },
        { QStringLiteral("source_sha256"), sourceSha256 },
        { QStringLiteral("save_policy"), savePolicy.toJson() },
        { QStringLiteral("registry_digest"), pdf::PDFRepairRegistry::instance().digest() },
        { QStringLiteral("plans"), QJsonArray{ plan.toJson() } }
    };
    const QJsonObject canonical = pdf::canonicalizeJson(envelope).toObject();
    QVERIFY(!canonical.contains(QStringLiteral("destination")));
    QVERIFY(!canonical.contains(QStringLiteral("output_path")));
    QVERIFY(!canonical.contains(QStringLiteral("outputPath")));
    QVERIFY(canonical.contains(QStringLiteral("registry_digest")));
    QCOMPARE(digest, sha256Hex(pdf::canonicalJson(envelope)));

    // Destination is an execution input only: two destinations share one plan digest.
    QCOMPARE(digest, pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy));
}

void GovernedExecutionTest::destinationOverwriteFailIsFailClosed()
{
    const QByteArray candidateBytes("governed candidate bytes");
    const QString sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const QString candidateSha256 = sha256Hex(candidateBytes);
    const QString planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = candidateSha256;
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.decision = QStringLiteral("approve");
    approval.approval.rationale = QStringLiteral("Reviewed.");
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));
    {
        QFile existing(outputPath);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write(QByteArrayLiteral("existing")), qint64(8));
    }

    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Fail));
    QFile untouched(outputPath);
    QVERIFY(untouched.open(QIODevice::ReadOnly));
    QCOMPARE(untouched.readAll(), QByteArrayLiteral("existing"));
}

void GovernedExecutionTest::staleDestinationBindingRejectsPublication()
{
    const QByteArray candidateBytes("governed candidate bytes");
    const QString sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const QString candidateSha256 = sha256Hex(candidateBytes);
    const QString planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = QStringLiteral("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.decision = QStringLiteral("approve");
    approval.approval.rationale = QStringLiteral("Stale candidate binding.");
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));
    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Fail));
    QVERIFY(!QFile::exists(outputPath));
    Q_UNUSED(candidateSha256);
}

void GovernedExecutionTest::publicationWithoutSignOffIsNotDurableCompletion()
{
    const QByteArray candidateBytes("%PDF-1.7\n%%EOF\n");
    const QString sourceSha256 = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const QString candidateSha256 = sha256Hex(candidateBytes);
    const QString planDigest = QStringLiteral("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");

    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = candidateSha256;
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.decision = QStringLiteral("approve");
    approval.approval.rationale = QStringLiteral("Publish only.");
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath = temporary.filePath(QStringLiteral("published.pdf"));
    QVERIFY(pdf::publishGovernedArtifact(approval,
                                         planDigest,
                                         sourceSha256,
                                         candidateBytes,
                                         outputPath,
                                         pdf::PDFSafeFileWriter::OverwritePolicy::Fail));
    QVERIFY(QFile::exists(outputPath));

    // Published bytes are durable on disk, but without finalizeGovernedPublication
    // there is no sign-off certificate — D4 separates these states.
    pdf::PDFGovernedExecutionSignOff emptySignOff;
    QVERIFY(!emptySignOff.isValid());
    QFile published(outputPath);
    QVERIFY(published.open(QIODevice::ReadOnly));
    QCOMPARE(published.readAll(), candidateBytes);
}

void GovernedExecutionTest::crossSurfaceEquality_isPlanIdentityNotIndependentPdfBytes()
{
    // D5 / #656 disposition: independent PDFDocumentWriter passes are allowed to
    // differ in clock- or random-derived fields. Cross-surface equality is the
    // governed plan digest under pinned semantic inputs, not raw writer bytes.
    pdf::PDFRepairPlan plan;
    plan.operationId = QStringLiteral("add-bleed");
    plan.parameters = QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 } };
    const QString sourceSha256 = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const pdf::PDFOperationSavePolicy savePolicy = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("cli"));
    const QString cliDigest = pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy);
    const QString editorDigest = pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy);
    const QString headlessDigest = pdf::computeOperationPlanDigest({ plan }, sourceSha256, savePolicy);
    QCOMPARE(cliDigest, editorDigest);
    QCOMPARE(cliDigest, headlessDigest);

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument first = builder.build();
    const pdf::PDFDocument second = builder.build();
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstPath = temporary.filePath(QStringLiteral("a.pdf"));
    const QString secondPath = temporary.filePath(QStringLiteral("b.pdf"));
    pdf::PDFDocumentWriter writer(nullptr);
    QVERIFY(writer.write(firstPath, &first, true));
    QVERIFY(writer.write(secondPath, &second, true));
    // Structural identity is what fail-closed proofs should assert when writer
    // bytes may drift; do not require byte-identical serialization here.
    QCOMPARE(first.getCatalog()->getPageCount(), second.getCatalog()->getPageCount());
}

void GovernedExecutionTest::previewLeavesSourceBytesUntouched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = temporary.filePath(QStringLiteral("received.pdf"));
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    QVERIFY(writer.write(sourcePath, &source, true));
    const QString digestBefore = fileSha256Hex(sourcePath);
    QVERIFY(!digestBefore.isEmpty());

    pdf::PDFRepairTransactionOptions transactionOptions;
    transactionOptions.sourcePath = sourcePath;
    pdf::PDFRepairTransaction transaction(source, transactionOptions);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy());

    pdf::PDFTechnicalPreview technicalPreview;
    QVERIFY(pdf::buildTechnicalPreview(transaction, candidatePath, planDigest, &technicalPreview));
    QCOMPARE(fileSha256Hex(candidatePath), technicalPreview.candidateSha256);
    QVERIFY(readsAsValidPdf(candidatePath));

    pdf::PDFVisualPreview visualPreview;
    pdf::PDFRepairDiffOptions options;
    QVERIFY(pdf::buildVisualPreview(transaction, candidatePath, planDigest, options, &visualPreview));
    QCOMPARE(fileSha256Hex(candidatePath), visualPreview.candidateSha256);

    // The trusted source file is byte-identical after both previews.
    QCOMPARE(fileSha256Hex(sourcePath), digestBefore);

    // P1 fidelity vocabulary: technical is Exact, visual is Simulated.
    QCOMPARE(technicalPreview.fidelity, pdf::PDFRepairPreviewFidelity::Exact);
    QCOMPARE(visualPreview.fidelity, pdf::PDFRepairPreviewFidelity::Simulated);
    QCOMPARE(technicalPreview.toJson().value(QStringLiteral("fidelity_mode")).toString(), QStringLiteral("exact"));
    QCOMPARE(visualPreview.toJson().value(QStringLiteral("fidelity_mode")).toString(), QStringLiteral("simulated"));
    QVERIFY(technicalPreview.toJson().contains(QStringLiteral("fidelity_mode")));
    QVERIFY(visualPreview.toJson().contains(QStringLiteral("fidelity_mode")));
}

void GovernedExecutionTest::cancelledPreviewLeavesNoArtifacts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));
    const QString renderDirectory = temporary.filePath(QStringLiteral("renders"));
    QVERIFY(QDir().mkpath(renderDirectory));

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();

    TrippedCancelControl control;
    pdf::PDFRepairTransactionOptions transactionOptions;
    transactionOptions.operationControl = &control;
    pdf::PDFRepairTransaction transaction(source, transactionOptions);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy());

    pdf::PDFRepairDiffOptions options;
    options.renderDirectory = renderDirectory;
    options.operationControl = &control;
    // Cancel after the first rendered page: the render loop polls the control
    // once per page, so tripping it here lands on the second page's check.
    options.previewStageHook = [&control](const QString& stage)
    {
        if (stage == QStringLiteral("visual-page"))
        {
            control.trip();
        }
    };

    pdf::PDFVisualPreview preview;
    const pdf::PDFOperationResult result = pdf::buildVisualPreview(transaction, candidatePath, planDigest, options, &preview);
    QVERIFY2(result, qPrintable(result.getErrorMessage()));
    QCOMPARE(preview.status, pdf::PDFRepairDiffStatus::Incomplete);
    QVERIFY(preview.incompleteReasons.contains(QStringLiteral("cancelled")));
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Incomplete);

    // No externally visible artifact survives the cancelled preview.
    QVERIFY(!QFile::exists(candidatePath));
    QCOMPARE(QDir(renderDirectory).entryList(QDir::Files).size(), 0);
    QVERIFY(QDir(temporary.path()).entryList(QDir::Files).isEmpty());
    QVERIFY(!QFile::exists(temporary.filePath(QStringLiteral("approval.json"))));
}

void GovernedExecutionTest::previewRefusesPlanDigestMismatch()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy());

    pdf::PDFTechnicalPreview technicalPreview;
    QVERIFY(!pdf::buildTechnicalPreview(transaction, candidatePath, QString(), &technicalPreview));
    QVERIFY(!pdf::buildTechnicalPreview(transaction, candidatePath, QStringLiteral("not-a-digest"), &technicalPreview));
    QVERIFY(!pdf::buildTechnicalPreview(transaction, candidatePath, QString(64, QLatin1Char('a')), &technicalPreview));

    pdf::PDFVisualPreview visualPreview;
    pdf::PDFRepairDiffOptions options;
    QVERIFY(!pdf::buildVisualPreview(transaction, candidatePath, QString(), options, &visualPreview));
    QVERIFY(!pdf::buildVisualPreview(transaction, candidatePath, QStringLiteral("deadbeef"), options, &visualPreview));
    QVERIFY(!pdf::buildVisualPreview(transaction, candidatePath, QString(64, QLatin1Char('b')), options, &visualPreview));

    // Every refusal happens before any candidate write.
    QVERIFY(!QFile::exists(candidatePath));

    // The matching digest is accepted and produces the candidate.
    QVERIFY(pdf::buildTechnicalPreview(transaction, candidatePath, planDigest, &technicalPreview));
    QVERIFY(QFile::exists(candidatePath));
}

void GovernedExecutionTest::failedPreviewPreservesCallerFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourcePath = temporary.filePath(QStringLiteral("source.pdf"));
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    QVERIFY(writer.write(sourcePath, &source, true));
    const QString sourceDigest = fileSha256Hex(sourcePath);
    QVERIFY(!sourceDigest.isEmpty());

    TrippedCancelControl control;
    pdf::PDFRepairTransactionOptions transactionOptions;
    transactionOptions.sourcePath = sourcePath;
    transactionOptions.operationControl = &control;
    pdf::PDFRepairTransaction transaction(source, transactionOptions);
    const pdf::PDFRepairOperation* bleed = pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed"));
    QVERIFY(transaction.add(bleed, QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy());
    pdf::PDFTechnicalPreview technical;
    QVERIFY(!pdf::buildTechnicalPreview(transaction, sourcePath, planDigest, &technical));
    QCOMPARE(fileSha256Hex(sourcePath), sourceDigest);
    pdf::PDFVisualPreview visual;
    QVERIFY(!pdf::buildVisualPreview(transaction, sourcePath, planDigest, {}, &visual));
    QCOMPARE(fileSha256Hex(sourcePath), sourceDigest);

    pdf::PDFRepairTransaction incremental(source, transactionOptions);
    QVERIFY(incremental.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("production.validate-wide-format")),
                            QJsonObject{ { QStringLiteral("geometry"), QJsonObject() } }));
    QVERIFY(incremental.analyze());
    QVERIFY(incremental.apply());
    const QString incrementalDigest = pdf::computeOperationPlanDigest(incremental.plans(), incremental.sourceSha256(), incremental.savePolicy());
    QVERIFY(!pdf::buildTechnicalPreview(incremental, sourcePath, incrementalDigest, &technical));
    QCOMPARE(fileSha256Hex(sourcePath), sourceDigest);

    const QString existingPath = temporary.filePath(QStringLiteral("existing.pdf"));
    QVERIFY(QFile::copy(sourcePath, existingPath));
    control.trip();
    QVERIFY(pdf::buildTechnicalPreview(transaction, existingPath, planDigest, &technical));
    QCOMPARE(technical.status, pdf::PDFRepairDiffStatus::Incomplete);
    QCOMPARE(fileSha256Hex(existingPath), sourceDigest);
    QVERIFY(pdf::buildVisualPreview(transaction, existingPath, planDigest, {}, &visual));
    QCOMPARE(visual.status, pdf::PDFRepairDiffStatus::Incomplete);
    QCOMPARE(fileSha256Hex(existingPath), sourceDigest);
}

void GovernedExecutionTest::cancelledFinalPageLeavesNoArtifacts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));
    const QString renders = temporary.filePath(QStringLiteral("renders"));
    QVERIFY(QDir().mkpath(renders));
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    TrippedCancelControl control;
    pdf::PDFRepairTransactionOptions transactionOptions;
    transactionOptions.operationControl = &control;
    pdf::PDFRepairTransaction transaction(source, transactionOptions);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());
    const QString planDigest = pdf::computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy());
    pdf::PDFRepairDiffOptions options;
    options.renderDirectory = renders;
    options.previewStageHook = [&control](const QString& stage)
    {
        if (stage == QStringLiteral("visual-page"))
            control.trip();
    };
    pdf::PDFVisualPreview preview;
    QVERIFY(pdf::buildVisualPreview(transaction, candidatePath, planDigest, options, &preview));
    QCOMPARE(preview.status, pdf::PDFRepairDiffStatus::Incomplete);
    QVERIFY(preview.incompleteReasons.contains(QStringLiteral("cancelled")));
    QVERIFY(!QFile::exists(candidatePath));
    QVERIFY(QDir(renders).entryList(QDir::Files).isEmpty());
}

void GovernedExecutionTest::previewRefusesInvalidatedPlan()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString candidatePath = temporary.filePath(QStringLiteral("candidate.pdf"));
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFRepairTransaction transaction(source);
    const pdf::PDFRepairOperation* bleed = pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed"));
    QVERIFY(transaction.add(bleed, QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());
    const QString oldDigest = pdf::computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy());
    QVERIFY(transaction.add(bleed, QJsonObject{ { QStringLiteral("bleed_mm"), 6.0 } }));
    pdf::PDFTechnicalPreview technical;
    pdf::PDFVisualPreview visual;
    QVERIFY(!pdf::buildTechnicalPreview(transaction, candidatePath, oldDigest, &technical));
    QVERIFY(!pdf::buildVisualPreview(transaction, candidatePath, oldDigest, {}, &visual));
    QVERIFY(!QFile::exists(candidatePath));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());
    const QString currentDigest = pdf::computeOperationPlanDigest(transaction.plans(), transaction.sourceSha256(), transaction.savePolicy());
    QVERIFY(currentDigest != oldDigest);
    QVERIFY(!pdf::buildTechnicalPreview(transaction, candidatePath, oldDigest, &technical));
    QVERIFY(pdf::buildTechnicalPreview(transaction, candidatePath, currentDigest, &technical));
    QVERIFY(QFile::exists(candidatePath));
}

QTEST_APPLESS_MAIN(GovernedExecutionTest)
#include "tst_governedexecutiontest.moc"
