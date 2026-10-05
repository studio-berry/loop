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
