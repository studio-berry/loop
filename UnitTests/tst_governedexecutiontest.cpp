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
#include "pdfgovernedexecution.h"
#include "pdfrepairoperation.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class GovernedExecutionTest final : public QObject
{
    Q_OBJECT

private slots:
    void planDigest_isDeterministicAndSensitive();
    void technicalAndVisualPreviewsStaySeparate();
    void preflightDecisionIsNotOperationApproval();
    void publishRequiresMatchingPlanBoundApproval();
    void publishRejectsPreflightDecisionReference();
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

QTEST_APPLESS_MAIN(GovernedExecutionTest)
#include "tst_governedexecutiontest.moc"
