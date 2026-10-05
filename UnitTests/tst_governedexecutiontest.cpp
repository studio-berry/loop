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
#include "pdfartifactstore.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"
#include "pdfgovernedexecution.h"
#include "pdfoperationhistorystore.h"
#include "pdfrepairoperation.h"
#include "pdfsafefilewriter.h"
#include "pdfsavepolicy.h"

#include <QCryptographicHash>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    // L04-04 / #36 — exact-plan approval binding and revocation
    void approvalExpiryWindowIsEnforced();
    void approverRightsAreAllowlisted();
    void profileBindingRefusesMismatch();
    void revokedApprovalIsRefusedBeforeWrite();
    void revokedApprovalRefusesLaterAttempt();
    void malformedApprovalExpiryRefusesPublication();
    void failedPreviewPreservesCallerFiles();
    void cancelledFinalPageLeavesNoArtifacts();
    void previewRefusesInvalidatedPlan();

    // L04-05 (#37) — one cancellation-safe governed mutation gateway
    void gatewayRefusesMalformedRequestBeforeWrite();
    void gatewayRefusesStaleApprovalBeforeWrite();
    void gatewayRefusesUnauthorizedApprovalBeforeWrite();
    void gatewayRefusesAlreadyTerminalReplay();
    void gatewayDestinationConflictLeavesExistingFileUntouched();
    void gatewayCancelAtCommitSeamLeavesDestinationUntouched();
    void gatewayRevalidationFailureLeavesDestinationUntouched();
    void gatewayHappyPathPublishesExactlyOnce();
    void gatewayRechecksRevocationAtCommit();
    void gatewayRechecksExpiryAtCommit();
    void gatewayReplayBindsPlanAndExecution();
    void gatewayRefusesMissingHistoryArtifactsBeforeWrite();
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

namespace
{

pdf::PDFGovernedExecutionApproval operatorApproval(const QString& planDigest,
                                                   const QString& sourceSha256,
                                                   const QString& candidateSha256,
                                                   const QString& decisionReference)
{
    pdf::PDFGovernedExecutionApproval approval;
    approval.planDigest = planDigest;
    approval.sourceSha256 = sourceSha256;
    approval.candidateSha256 = candidateSha256;
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.decision = QStringLiteral("approve");
    approval.approval.rationale = QStringLiteral("Reviewed the exact plan.");
    approval.approval.decisionReference = decisionReference;
    approval.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    return approval;
}

}   // namespace

void GovernedExecutionTest::approvalExpiryWindowIsEnforced()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    const QString candidateSha256 = sha256Hex(QByteArrayLiteral("governed candidate bytes"));
    pdf::PDFGovernedExecutionApproval approval = operatorApproval(planDigest, sourceSha256, candidateSha256, QStringLiteral("approval:expiry"));

    pdf::PDFApprovalAuthorizationContext context;
    context.evaluatedUtc = now;

    // No declared expiry and no policy requirement: allowed.
    QVERIFY(pdf::resolveApprovalAuthorization(approval, context).allowed);

    // A window that closed before evaluation is refused.
    approval.approval.expiresUtc = now.addSecs(-1);
    QCOMPARE(pdf::resolveApprovalAuthorization(approval, context).code, QStringLiteral("approval-expired"));

    // The boundary is inclusive: an expiry exactly at the evaluation time is expired.
    approval.approval.expiresUtc = now;
    QCOMPARE(pdf::resolveApprovalAuthorization(approval, context).code, QStringLiteral("approval-expired"));

    // A still-open window passes.
    approval.approval.expiresUtc = now.addSecs(60);
    QVERIFY(pdf::resolveApprovalAuthorization(approval, context).allowed);

    // requireExpiry refuses an approval that declares no expiry.
    approval.approval.expiresUtc = QDateTime();
    context.policy.requireExpiry = true;
    QCOMPARE(pdf::resolveApprovalAuthorization(approval, context).code, QStringLiteral("approval-expired"));
}

void GovernedExecutionTest::approverRightsAreAllowlisted()
{
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    const QString candidateSha256 = sha256Hex(QByteArrayLiteral("governed candidate bytes"));
    pdf::PDFGovernedExecutionApproval approval = operatorApproval(planDigest, sourceSha256, candidateSha256, QStringLiteral("approval:rights"));

    pdf::PDFApprovalAuthorizationContext context;
    context.policy.authorizedActorIds = QStringList{ QStringLiteral("operator") };
    QVERIFY(pdf::resolveApprovalAuthorization(approval, context).allowed);

    // A non-listed actor is refused even with an affirmative decision.
    approval.approval.actorId = QStringLiteral("Editor");
    const pdf::PDFApprovalAuthorization denied = pdf::resolveApprovalAuthorization(approval, context);
    QVERIFY(!denied.allowed);
    QCOMPARE(denied.code, QStringLiteral("approval-unauthorized"));

    // An approval with no actor can never be authorized.
    context.policy.authorizedActorIds.clear();
    approval.approval.actorId.clear();
    QCOMPARE(pdf::resolveApprovalAuthorization(approval, context).code, QStringLiteral("approval-unauthorized"));

    // A kind outside the declared allowlist is refused.
    approval.approval.actorId = QStringLiteral("operator");
    approval.approval.kind = pdf::PDFApprovalKind::Human;
    context.policy.authorizedKinds = QList<pdf::PDFApprovalKind>{ pdf::PDFApprovalKind::Policy };
    QCOMPARE(pdf::resolveApprovalAuthorization(approval, context).code, QStringLiteral("approval-unauthorized"));
}

void GovernedExecutionTest::profileBindingRefusesMismatch()
{
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    const QString candidateSha256 = sha256Hex(QByteArrayLiteral("governed candidate bytes"));
    pdf::PDFGovernedExecutionApproval approval = operatorApproval(planDigest, sourceSha256, candidateSha256, QStringLiteral("approval:profile"));
    approval.effectiveProfileDigest = QString(64, QLatin1Char('a'));

    pdf::PDFApprovalAuthorizationContext context;
    context.expectedProfileDigest = QString(64, QLatin1Char('a'));
    QVERIFY(pdf::resolveApprovalAuthorization(approval, context).allowed);

    // A different effective profile invalidates the approval.
    context.expectedProfileDigest = QString(64, QLatin1Char('d'));
    QCOMPARE(pdf::resolveApprovalAuthorization(approval, context).code, QStringLiteral("profile-binding"));

    // The caller binds a profile but the approval declares none.
    approval.effectiveProfileDigest.clear();
    QCOMPARE(pdf::resolveApprovalAuthorization(approval, context).code, QStringLiteral("profile-binding"));

    // An empty expectation means the caller does not bind a profile.
    context.expectedProfileDigest.clear();
    QVERIFY(pdf::resolveApprovalAuthorization(approval, context).allowed);
}

namespace
{

/// Opens a history store with one running execution so an ApprovalRevoked event
/// can be appended for a revocation test.
bool openHistoryForRevocation(const QString& directory,
                              pdf::PDFArtifactStore* artifacts,
                              pdf::PDFOperationHistoryStore* history,
                              QUuid* executionId)
{
    Q_UNUSED(directory);
    const auto input = artifacts->importBytes(QByteArrayLiteral("source"),
                                              { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    if (!input.success)
    {
        return false;
    }
    if (!history->open() || !history->registerArtifact(input.artifact))
    {
        return false;
    }
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("governed.revocation");
    execution.input = input.artifact;
    return bool(history->beginExecution(execution, executionId));
}

void appendRevocation(pdf::PDFOperationHistoryStore& history, const QUuid& executionId, const QString& reference)
{
    pdf::PDFOperationHistoryEvent revoked;
    revoked.executionId = executionId;
    revoked.kind = pdf::PDFOperationHistoryEventKind::ApprovalRevoked;
    revoked.status = pdf::PDFOperationHistoryStatus::Rejected;
    revoked.operatorIdentity = QStringLiteral("operator");
    revoked.approval.decisionReference = reference;
    history.appendEvent(revoked);
}

}   // namespace

void GovernedExecutionTest::revokedApprovalIsRefusedBeforeWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QUuid executionId;
    QVERIFY(openHistoryForRevocation(temporary.path(), &artifacts, &history, &executionId));

    const QByteArray candidateBytes("governed candidate bytes");
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    const QString candidateSha256 = sha256Hex(candidateBytes);
    pdf::PDFGovernedExecutionApproval approval = operatorApproval(planDigest, sourceSha256, candidateSha256, QStringLiteral("approval:race"));

    pdf::PDFApprovalAuthorizationContext context;
    context.evaluatedUtc = QDateTime::currentDateTimeUtc();
    context.history = &history;

    // The approval passes validation while it is still current.
    QVERIFY(pdf::validateGovernedApproval(approval, planDigest, sourceSha256, candidateSha256, context));

    // A later ApprovalRevoked event naming the same reference revokes it.
    appendRevocation(history, executionId, QStringLiteral("approval:race"));

    const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));
    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite,
                                          context));
    QVERIFY(!QFile::exists(outputPath));
}

void GovernedExecutionTest::revokedApprovalRefusesLaterAttempt()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QUuid executionId;
    QVERIFY(openHistoryForRevocation(temporary.path(), &artifacts, &history, &executionId));

    const QByteArray candidateBytes("governed candidate bytes");
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    const QString candidateSha256 = sha256Hex(candidateBytes);
    pdf::PDFGovernedExecutionApproval approval = operatorApproval(planDigest, sourceSha256, candidateSha256, QStringLiteral("approval:ordering"));

    pdf::PDFApprovalAuthorizationContext context;
    context.evaluatedUtc = QDateTime::currentDateTimeUtc();
    context.history = &history;

    const QString outputPath = temporary.filePath(QStringLiteral("published.pdf"));
    QVERIFY(pdf::publishGovernedArtifact(approval,
                                         planDigest,
                                         sourceSha256,
                                         candidateBytes,
                                         outputPath,
                                         pdf::PDFSafeFileWriter::OverwritePolicy::Fail,
                                         context));
    QVERIFY(QFile::exists(outputPath));
    const qint64 bytesAfterPublish = QFileInfo(outputPath).size();

    appendRevocation(history, executionId, QStringLiteral("approval:ordering"));

    // A later attempt through the same gateway is refused and does not rewrite.
    QVERIFY(!pdf::publishGovernedArtifact(approval,
                                          planDigest,
                                          sourceSha256,
                                          candidateBytes,
                                          outputPath,
                                          pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite,
                                          context));
    QCOMPARE(QFileInfo(outputPath).size(), bytesAfterPublish);
}

namespace
{

/// Serializes a minimal, valid PDF candidate the governed profile below accepts.
QByteArray governedGatewayCandidate()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFDocument document = builder.build();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pdf::PDFDocumentWriter writer(nullptr);
    writer.write(&buffer, &document);
    buffer.close();
    return bytes;
}

QJsonObject governedGatewayProfile()
{
    return QJsonObject{
        { QStringLiteral("name"), QStringLiteral("Governed gateway smoke") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("font-integrity") }, { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };
}

pdf::PDFGovernedMutationRequest governedGatewayRequest(const QByteArray& candidate,
                                                       const QString& planDigest,
                                                       const QString& sourceSha256,
                                                       const QString& destination)
{
    pdf::PDFGovernedMutationRequest request;
    request.approval = operatorApproval(planDigest, sourceSha256, sha256Hex(candidate), QStringLiteral("approval:gateway"));
    request.planDigest = planDigest;
    request.sourceSha256 = sourceSha256;
    request.candidateBytes = candidate;
    request.destinationPath = destination;
    request.overwritePolicy = pdf::PDFSafeFileWriter::OverwritePolicy::Fail;
    request.profile = governedGatewayProfile();
    request.signOffActor = QStringLiteral("gateway-test");
    request.signOffPolicy = QStringLiteral("gateway-postflight");
    return request;
}

}   // namespace

void GovernedExecutionTest::gatewayRefusesMalformedRequestBeforeWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = temporary.filePath(QStringLiteral("malformed.pdf"));

    // No plan digest and no approval: the request is malformed.
    pdf::PDFGovernedMutationRequest request;
    request.candidateBytes = governedGatewayCandidate();
    request.destinationPath = destination;
    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.status, QStringLiteral("refused"));
    QCOMPARE(receipt.reasonCode, QStringLiteral("malformed-request"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(receipt.publishedSha256.isEmpty());
    QVERIFY(!QFile::exists(destination));
}

void GovernedExecutionTest::gatewayRefusesStaleApprovalBeforeWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = temporary.filePath(QStringLiteral("stale.pdf"));
    const QByteArray candidate = governedGatewayCandidate();
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));

    pdf::PDFGovernedMutationRequest request = governedGatewayRequest(candidate, planDigest, sourceSha256, destination);
    // The approval names a different plan than the one requested.
    request.approval.planDigest = QString(64, QLatin1Char('d'));

    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.status, QStringLiteral("refused"));
    QCOMPARE(receipt.reasonCode, QStringLiteral("approval-stale"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(!QFile::exists(destination));
}

void GovernedExecutionTest::gatewayRefusesUnauthorizedApprovalBeforeWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = temporary.filePath(QStringLiteral("unauthorized.pdf"));
    const QByteArray candidate = governedGatewayCandidate();
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));

    pdf::PDFGovernedMutationRequest request = governedGatewayRequest(candidate, planDigest, sourceSha256, destination);
    request.authorization.policy.authorizedActorIds = QStringList{ QStringLiteral("some-other-operator") };

    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.status, QStringLiteral("refused"));
    QCOMPARE(receipt.reasonCode, QStringLiteral("approval-unauthorized"));
    QVERIFY(!QFile::exists(destination));
}

void GovernedExecutionTest::gatewayRefusesAlreadyTerminalReplay()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    const auto input = artifacts.importBytes(QByteArrayLiteral("source"),
                                             { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    QVERIFY(input.success);
    QVERIFY(history.open());
    QVERIFY(history.registerArtifact(input.artifact));
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("governed.gateway.replay");
    execution.input = input.artifact;
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));

    const QByteArray candidate = governedGatewayCandidate();
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    const auto output = artifacts.importBytes(candidate,
                                              { QStringLiteral("application/pdf"), QStringLiteral("candidate.pdf") });
    QVERIFY(output.success);

    // A terminal Accepted event already records this plan's published candidate.
    pdf::PDFOperationHistoryEvent accepted;
    accepted.executionId = executionId;
    accepted.kind = pdf::PDFOperationHistoryEventKind::FixApplied;
    accepted.status = pdf::PDFOperationHistoryStatus::Accepted;
    accepted.operatorIdentity = QStringLiteral("operator");
    accepted.output = output.artifact;
    accepted.resultSummary = QJsonObject{ { QStringLiteral("approval"), QJsonObject{ { QStringLiteral("plan_digest"), planDigest } } } };
    QVERIFY(history.appendEvent(accepted));

    const QString destination = temporary.filePath(QStringLiteral("replay.pdf"));
    pdf::PDFGovernedMutationRequest request = governedGatewayRequest(candidate, planDigest, sourceSha256, destination);
    request.history = &history;
    request.operationId = QStringLiteral("governed.gateway.replay");
    request.inputArtifact = input.artifact;
    request.outputArtifact = output.artifact;

    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.status, QStringLiteral("refused"));
    QCOMPARE(receipt.reasonCode, QStringLiteral("already-terminal"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(receipt.publishedSha256.isEmpty());
    QVERIFY(!QFile::exists(destination));
}

void GovernedExecutionTest::gatewayDestinationConflictLeavesExistingFileUntouched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = temporary.filePath(QStringLiteral("conflict.pdf"));
    QFile existing(destination);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    const QByteArray priorBytes = QByteArrayLiteral("prior artifact bytes");
    QCOMPARE(existing.write(priorBytes), priorBytes.size());
    existing.close();
    const QString priorSha = sha256Hex(priorBytes);

    const QByteArray candidate = governedGatewayCandidate();
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    pdf::PDFGovernedMutationRequest request = governedGatewayRequest(candidate, planDigest, sourceSha256, destination);

    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.status, QStringLiteral("failed"));
    QCOMPARE(receipt.reasonCode, QStringLiteral("destination-conflict"));
    QVERIFY(!receipt.destinationTouched);
    QCOMPARE(fileSha256Hex(destination), priorSha);
}

void GovernedExecutionTest::gatewayCancelAtCommitSeamLeavesDestinationUntouched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = temporary.filePath(QStringLiteral("cancelled.pdf"));
    const QByteArray candidate = governedGatewayCandidate();
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    pdf::PDFGovernedMutationRequest request = governedGatewayRequest(candidate, planDigest, sourceSha256, destination);

    TrippedCancelControl cancel;
    request.operationControl = &cancel;
    request.beforeCommit = [&cancel]()
    { cancel.trip(); };

    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.status, QStringLiteral("cancelled"));
    QCOMPARE(receipt.reasonCode, QStringLiteral("cancelled"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(receipt.publishedSha256.isEmpty());
    QVERIFY(!QFile::exists(destination));
}

void GovernedExecutionTest::gatewayRevalidationFailureLeavesDestinationUntouched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = temporary.filePath(QStringLiteral("revalidation.pdf"));
    // Bytes that stage and hash, but cannot be reopened as a PDF: the staged-bytes
    // finalize fails and the destination is never committed.
    const QByteArray candidate = QByteArrayLiteral("not a reopenable governed artifact");
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    pdf::PDFGovernedMutationRequest request = governedGatewayRequest(candidate, planDigest, sourceSha256, destination);

    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.status, QStringLiteral("failed"));
    QCOMPARE(receipt.reasonCode, QStringLiteral("revalidation-failed"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(!QFile::exists(destination));
}

void GovernedExecutionTest::gatewayHappyPathPublishesExactlyOnce()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destination = temporary.filePath(QStringLiteral("published.pdf"));
    const QByteArray candidate = governedGatewayCandidate();
    const QString candidateSha256 = sha256Hex(candidate);
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    pdf::PDFGovernedMutationRequest request = governedGatewayRequest(candidate, planDigest, sourceSha256, destination);

    pdf::PDFGovernedMutationReceipt receipt;
    const pdf::PDFOperationResult result = pdf::executeGovernedMutation(request, &receipt);
    QVERIFY2(result, qPrintable(result.getErrorMessage()));
    QCOMPARE(receipt.status, QStringLiteral("published"));
    QVERIFY(receipt.isPublished());
    QVERIFY(receipt.reasonCode.isEmpty());
    QVERIFY(receipt.destinationTouched);
    QCOMPARE(receipt.publishedSha256, candidateSha256);
    QVERIFY(receipt.revalidation.isSignOffEligible());
    QVERIFY(receipt.signOff.isValid());
    QCOMPARE(receipt.signOff.publishedSha256, candidateSha256);
    // Exactly one artifact lands at the destination, and no staging residue survives.
    QVERIFY(QFile::exists(destination));
    QCOMPARE(fileSha256Hex(destination), candidateSha256);
    const QDir destinationDirectory(QFileInfo(destination).absolutePath());
    QVERIFY(destinationDirectory.entryList(QStringList{ QStringLiteral("*.loop-staging-*") }, QDir::Files).isEmpty());
}

void GovernedExecutionTest::gatewayRechecksRevocationAtCommit()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(temporary.filePath(QStringLiteral("history.sqlite3")));
    QUuid executionId;
    QVERIFY(openHistoryForRevocation(temporary.path(), &artifacts, &history, &executionId));
    const QByteArray candidate = governedGatewayCandidate();
    auto request = governedGatewayRequest(candidate, QString(64, QLatin1Char('c')), sha256Hex(QByteArrayLiteral("source")),
                                          temporary.filePath(QStringLiteral("revoked.pdf")));
    const auto output = artifacts.importBytes(candidate, { QStringLiteral("application/pdf"), QStringLiteral("candidate.pdf") });
    QVERIFY(output.success);
    request.history = &history;
    request.executionId = executionId;
    request.outputArtifact = output.artifact;
    request.beforeCommit = [&]()
    { appendRevocation(history, executionId, request.approval.approval.decisionReference); };
    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.reasonCode, QStringLiteral("approval-revoked"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(!QFile::exists(request.destinationPath));
    QVERIFY(history.verify().verified);
}

void GovernedExecutionTest::gatewayRechecksExpiryAtCommit()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    auto request = governedGatewayRequest(governedGatewayCandidate(), QString(64, QLatin1Char('c')), QString(64, QLatin1Char('b')),
                                          temporary.filePath(QStringLiteral("expired.pdf")));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    request.authorization.evaluatedUtc = now.addSecs(-60);
    request.approval.approval.expiresUtc = now.addSecs(-1);
    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.reasonCode, QStringLiteral("approval-expired"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(!QFile::exists(request.destinationPath));
}

void GovernedExecutionTest::gatewayReplayBindsPlanAndExecution()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(temporary.filePath(QStringLiteral("history.sqlite3")));
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    QVERIFY(input.success);
    QVERIFY(history.open());
    QVERIFY(history.registerArtifact(input.artifact));
    const QByteArray candidate = governedGatewayCandidate();
    const auto output = artifacts.importBytes(candidate, { QStringLiteral("application/pdf"), QStringLiteral("candidate.pdf") });
    QVERIFY(output.success);
    auto request = governedGatewayRequest(candidate, QString(64, QLatin1Char('c')), input.artifact.sha256,
                                          temporary.filePath(QStringLiteral("first.pdf")));
    request.history = &history;
    request.operationId = QStringLiteral("gateway.replay");
    request.inputArtifact = input.artifact;
    request.outputArtifact = output.artifact;
    pdf::PDFGovernedMutationReceipt receipt;
    const auto published = pdf::executeGovernedMutation(request, &receipt);
    QVERIFY2(published, qPrintable(published.getErrorMessage()));
    const QUuid executionId = receipt.executionId;

    request.destinationPath = temporary.filePath(QStringLiteral("replay.pdf"));
    request.candidateBytes += '\n';
    request.approval.candidateSha256 = sha256Hex(request.candidateBytes);
    const auto revisedOutput = artifacts.importBytes(request.candidateBytes, { QStringLiteral("application/pdf"), QStringLiteral("revised.pdf") });
    QVERIFY(revisedOutput.success);
    request.outputArtifact = revisedOutput.artifact;
    request.approval.approval.decisionReference = QStringLiteral("approval:another-review");
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.reasonCode, QStringLiteral("already-terminal"));
    QVERIFY(!QFile::exists(request.destinationPath));

    request.candidateBytes = candidate;
    request.outputArtifact = output.artifact;
    request.approval.candidateSha256 = sha256Hex(candidate);
    request.planDigest = QString(64, QLatin1Char('d'));
    request.approval.planDigest = request.planDigest;
    request.executionId = executionId;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.reasonCode, QStringLiteral("already-terminal"));
    QVERIFY(!QFile::exists(request.destinationPath));

    request.executionId = QUuid();
    const auto distinctPlan = pdf::executeGovernedMutation(request, &receipt);
    QVERIFY2(distinctPlan, qPrintable(distinctPlan.getErrorMessage()));
    QVERIFY(receipt.isPublished());
    QCOMPARE(fileSha256Hex(request.destinationPath), sha256Hex(candidate));
    request.planDigest = QString(64, QLatin1Char('e'));
    request.approval.planDigest = request.planDigest;
    request.destinationPath = temporary.filePath(QStringLiteral("outer.pdf"));
    auto concurrent = request;
    concurrent.destinationPath = temporary.filePath(QStringLiteral("inner.pdf"));
    pdf::PDFGovernedMutationReceipt innerReceipt;
    request.beforeCommit = [&]()
    { pdf::executeGovernedMutation(concurrent, &innerReceipt); };
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QVERIFY(innerReceipt.isPublished());
    QCOMPARE(receipt.reasonCode, QStringLiteral("already-terminal"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(!QFile::exists(request.destinationPath));
    QVERIFY(history.verify().verified);
}

void GovernedExecutionTest::gatewayRefusesMissingHistoryArtifactsBeforeWrite()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    QVERIFY(input.success);
    pdf::PDFOperationHistoryStore history(temporary.filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());
    QVERIFY(history.registerArtifact(input.artifact));
    auto request = governedGatewayRequest(governedGatewayCandidate(), QString(64, QLatin1Char('c')), input.artifact.sha256,
                                          temporary.filePath(QStringLiteral("missing-artifacts.pdf")));
    request.history = &history;
    request.operationId = QStringLiteral("gateway.missing-output");
    request.inputArtifact = input.artifact;
    pdf::PDFGovernedMutationReceipt receipt;
    QVERIFY(!pdf::executeGovernedMutation(request, &receipt));
    QCOMPARE(receipt.reasonCode, QStringLiteral("malformed-request"));
    QVERIFY(!receipt.destinationTouched);
    QVERIFY(!QFile::exists(request.destinationPath));
    QVERIFY(history.events().isEmpty());
}

void GovernedExecutionTest::malformedApprovalExpiryRefusesPublication()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QByteArray candidateBytes("candidate");
    const QString planDigest(64, QLatin1Char('c'));
    const QString sourceSha256(64, QLatin1Char('b'));
    const auto approval = operatorApproval(planDigest, sourceSha256, sha256Hex(candidateBytes), QStringLiteral("approval:expiry"));
    const QList<QJsonValue> invalidExpiries{ QStringLiteral("not-a-date"), 123, true, QJsonObject{} };
    for (const QJsonValue& expiry : invalidExpiries)
    {
        QJsonObject json = approval.toJson();
        QJsonObject record = json.value(QStringLiteral("approval")).toObject();
        record.insert(QStringLiteral("expiresUtc"), expiry);
        json.insert(QStringLiteral("approval"), record);
        const auto parsed = pdf::PDFGovernedExecutionApproval::fromJson(json);
        QVERIFY(!parsed.isValid());
        QVERIFY(!pdf::resolveApprovalAuthorization(parsed, {}).allowed);
        const QString outputPath = temporary.filePath(QStringLiteral("output.pdf"));
        QVERIFY(!pdf::publishGovernedArtifact(parsed, planDigest, sourceSha256, candidateBytes,
                                              outputPath, pdf::PDFSafeFileWriter::OverwritePolicy::Fail));
        QVERIFY(!QFile::exists(outputPath));
    }
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

QTEST_GUILESS_MAIN(GovernedExecutionTest)
#include "tst_governedexecutiontest.moc"
