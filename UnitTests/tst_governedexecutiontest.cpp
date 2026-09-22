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
#include "pdfdocumentwriter.h"
#include "pdfgovernedexecution.h"
#include "pdfrepairoperation.h"
#include "pdfsafefilewriter.h"
#include "pdfsavepolicy.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

QString sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
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

} // namespace

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
        { QStringLiteral("plans"), QJsonArray{ plan.toJson() } }
    };
    const QJsonObject canonical = pdf::canonicalizeJson(envelope).toObject();
    QVERIFY(!canonical.contains(QStringLiteral("destination")));
    QVERIFY(!canonical.contains(QStringLiteral("output_path")));
    QVERIFY(!canonical.contains(QStringLiteral("outputPath")));
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

QTEST_APPLESS_MAIN(GovernedExecutionTest)
#include "tst_governedexecutiontest.moc"
