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
#include "pdfdocumentwriter.h"
#include "pdfrepairoperation.h"
#include "pdfstandardconversion.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QJsonValue>
#include <QtTest>

namespace
{

class FailingRepair final : public pdf::PDFRepairOperation
{
public:
    QString id() const override { return QStringLiteral("test-failing"); }
    pdf::PDFRepairRisk risk() const override { return pdf::PDFRepairRisk::Low; }
    pdf::PDFRepairDomains domains() const override { return pdf::PDFRepairDomain::Metadata; }

    pdf::PDFOperationResult analyze(const pdf::PDFDocument&, const QJsonObject&, pdf::PDFRepairPlan* plan) const override
    {
        plan->operationId = id();
        plan->expectedChanges.metadata = true;
        return pdf::PDFOperationResult(true);
    }

    pdf::PDFOperationResult apply(pdf::PDFDocument*, const pdf::PDFRepairPlan&, pdf::PDFRepairResult*) const override
    {
        return pdf::PDFOperationResult(QStringLiteral("intentional test failure"));
    }
};

/// Serializes \p document into the bytes a candidate write would produce, so a
/// slot can assert on what a save path did or did not touch.
QByteArray writeSerializedBytes(const pdf::PDFDocument& document)
{
    pdf::PDFDocumentWriter writer(nullptr);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    const pdf::PDFOperationResult result = writer.write(&buffer, &document);
    return result ? buffer.data() : QByteArray();
}

}   // namespace

class RepairOperationTest : public QObject
{
    Q_OBJECT

private slots:
    void builtInOperations_areRegistered();
    void builtInOperations_declareSavePolicies();
    void everyRegisteredOperationDeclaresItsSavePolicy();
    void transactionRejectsAWeakenedSavePolicyBeforeMutation();
    void saveRequestRefusesToWriteOverTheTrustedSource();
    void candidateSaveRefusesToOverwriteTheSourceOnDisk();
    void sourceBytesSurviveSuccessCancelAndFailure();
    void analyze_doesNotMutateSource();
    void unsupportedPrecondition_preventsApply();
    void failedOperation_discardsCandidate();
    void addBleedExpectedChanges_areMeasuredWithoutUnexpectedDiff();
    void standardTargets_areExplicitAndStable();
};

void RepairOperationTest::builtInOperations_areRegistered()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    QVERIFY(registry.find(QStringLiteral("add-bleed")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("downsample-images")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("rgb-to-cmyk")) != nullptr);
    QVERIFY(registry.find(QStringLiteral("standards-convert")) != nullptr);
    QVERIFY(registry.descriptors().size() >= 4);
    for (const QJsonValue& descriptorValue : registry.descriptors())
    {
        const QJsonObject descriptor = descriptorValue.toObject();
        QVERIFY(descriptor.contains(QStringLiteral("preflight_fixup")));
        QVERIFY(descriptor.contains(QStringLiteral("save_policy")));
        const QJsonObject savePolicy = descriptor.value(QStringLiteral("save_policy")).toObject();
        QVERIFY(savePolicy.contains(QStringLiteral("mode")));
        QVERIFY(savePolicy.contains(QStringLiteral("invalidates_signatures")));
        QVERIFY(savePolicy.contains(QStringLiteral("reversible_in_session")));
        if (descriptor.value(QStringLiteral("id")).toString() == QStringLiteral("add-bleed") || descriptor.value(QStringLiteral("id")).toString() == QStringLiteral("downsample-images") || descriptor.value(QStringLiteral("id")).toString() == QStringLiteral("rgb-to-cmyk"))
        {
            QVERIFY(descriptor.value(QStringLiteral("preflight_fixup")).toBool());
        }
    }
}

void RepairOperationTest::builtInOperations_declareSavePolicies()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    const auto policyMode = [&registry](const QString& id)
    {
        return registry.find(id)->descriptor().value(QStringLiteral("save_policy")).toObject().value(QStringLiteral("mode")).toString();
    };

    QCOMPARE(policyMode(QStringLiteral("add-bleed")), QStringLiteral("save-as-new-artifact"));
    QCOMPARE(policyMode(QStringLiteral("downsample-images")), QStringLiteral("full-rewrite"));
    QCOMPARE(policyMode(QStringLiteral("rgb-to-cmyk")), QStringLiteral("save-as-new-artifact"));
    QCOMPARE(policyMode(QStringLiteral("production.validate-wide-format")), QStringLiteral("incremental-append"));
    QCOMPARE(policyMode(QStringLiteral("production.add-contour-bleed")), QStringLiteral("save-as-new-artifact"));
    QCOMPARE(policyMode(QStringLiteral("production.place-grommets")), QStringLiteral("incremental-append"));

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(registry.find(QStringLiteral("add-bleed")), QJsonObject{
                                                                            { QStringLiteral("bleed_mm"), 3.0 },
                                                                            { QStringLiteral("force"), true } }));
    QCOMPARE(transaction.savePolicy().mode, pdf::PDFSaveMode::SaveAsNewArtifact);
    QVERIFY(transaction.savePolicy().invalidatesSignatures);
}

void RepairOperationTest::everyRegisteredOperationDeclaresItsSavePolicy()
{
    const pdf::PDFRepairRegistry& registry = pdf::PDFRepairRegistry::instance();
    const QStringList ids = registry.operationIds();
    QVERIFY2(ids.size() >= 7, qPrintable(QString::number(ids.size())));
    for (const QString& id : ids)
    {
        const pdf::PDFRepairOperation* operation = registry.find(id);
        QVERIFY2(operation != nullptr, qPrintable(id));
        const pdf::PDFOperationSavePolicy policy = operation->savePolicy();
        QVERIFY2(!policy.isUndeclared(), qPrintable(id));
        QVERIFY2(!policy.rationale.isEmpty(), qPrintable(id));
        const QJsonObject descriptor = operation->descriptor();
        QCOMPARE(descriptor.value(QStringLiteral("save_policy")).toObject().value(QStringLiteral("mode")).toString(),
                 QString::fromLatin1(pdf::getPDFSaveModeName(policy.mode)));
    }

    // The conservative default is safe for the writer but it is not a
    // declaration, and it must never be reported as one.
    const pdf::PDFOperationSavePolicy undeclared = pdf::PDFOperationSavePolicy::undeclared();
    QVERIFY(undeclared.isUndeclared());
    QCOMPARE(QString::fromLatin1(pdf::getPDFSaveModeName(undeclared.mode)), QStringLiteral("save-as-new-artifact"));
    QVERIFY(undeclared.invalidatesSignatures);
    QVERIFY(!pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("ordinary edit")).isUndeclared());
}

void RepairOperationTest::transactionRejectsAWeakenedSavePolicyBeforeMutation()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();

    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 },
                                         { QStringLiteral("force"), true } }));
    // add-bleed declares save-as-new-artifact; asking for an append is weaker.
    const pdf::PDFOperationResult weakened = transaction.setRequestedSavePolicy(
        pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("caller wants an append")));
    QVERIFY(!weakened);
    QCOMPARE(weakened.getErrorMessage(),
             QStringLiteral("Refused save policy: mode 'incremental-append' is weaker than the operation-declared 'save-as-new-artifact'."));
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Failed);
    QVERIFY(!transaction.analyze());
    QVERIFY(!transaction.apply());

    // Stricter than declared is accepted and does not change the declared policy.
    pdf::PDFRepairTransaction stricter(source);
    QVERIFY(stricter.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("production.validate-wide-format")), QJsonObject{}));
    QVERIFY(stricter.setRequestedSavePolicy(pdf::PDFOperationSavePolicy::fullRewrite(QStringLiteral("caller wants a rewrite"))));
    QCOMPARE(stricter.savePolicy().mode, pdf::PDFSaveMode::IncrementalAppend);

    // The candidate write path applies the same rule: a request refused after
    // the mutation still cannot produce a candidate artifact.
    pdf::PDFRepairTransaction written(source);
    QVERIFY(written.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")),
                        QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 },
                                     { QStringLiteral("force"), true } }));
    QVERIFY(written.analyze());
    QVERIFY(written.apply());
    QVERIFY(!written.setRequestedSavePolicy(pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("caller wants an append"))));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    pdf::PDFDocument reopenedCandidate;
    const pdf::PDFOperationResult serialized = written.serializeCandidate(
        directory.filePath(QStringLiteral("candidate.pdf")), &reopenedCandidate);
    QVERIFY(!serialized);
    QCOMPARE(serialized.getErrorMessage(),
             QStringLiteral("Refused save policy: mode 'incremental-append' is weaker than the operation-declared 'save-as-new-artifact'."));
}

void RepairOperationTest::saveRequestRefusesToWriteOverTheTrustedSource()
{
    // The guard compares real files: create both paths so the check has
    // something to resolve.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("received.pdf"));
    const QString candidatePath = directory.filePath(QStringLiteral("received-candidate.pdf"));
    for (const QString& path : { sourcePath, candidatePath })
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArrayLiteral("%PDF-1.7\n%%EOF\n")) > 0);
        file.close();
    }

    const pdf::PDFOperationSavePolicy required = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("production correction"));
    pdf::PDFSaveRequest request;
    request.sourcePath = sourcePath;
    request.outputPath = sourcePath;
    request.required = required;
    request.requested = required;
    request.requestedExplicitly = true;
    const pdf::PDFOperationResult refused = pdf::validateSaveRequest(request);
    QVERIFY(!refused);
    QCOMPARE(refused.getErrorMessage(),
             QStringLiteral("Refused save: 'received.pdf' is the trusted input artifact; write the candidate to a new path."));

    // A distinct output path is fine, and an in-place append is the point of
    // that mode.
    request.outputPath = candidatePath;
    QVERIFY(pdf::validateSaveRequest(request));

    pdf::PDFSaveRequest append;
    append.sourcePath = sourcePath;
    append.outputPath = sourcePath;
    append.required = pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("annotation edit"));
    append.requested = append.required;
    append.requestedExplicitly = true;
    append.appendInPlace = true;
    QVERIFY(pdf::validateSaveRequest(append));

    // A weakened request is refused by the same call.
    pdf::PDFSaveRequest weakened = request;
    weakened.requested = pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("caller wants an append"));
    QCOMPARE(pdf::validateSaveRequest(weakened).getErrorMessage(),
             QStringLiteral("Refused save policy: mode 'incremental-append' is weaker than the operation-declared 'save-as-new-artifact'."));
}

void RepairOperationTest::candidateSaveRefusesToOverwriteTheSourceOnDisk()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("received.pdf"));

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();

    const QByteArray sourceBytes = writeSerializedBytes(source);
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QCOMPARE(sourceFile.write(sourceBytes), qint64(sourceBytes.size()));
    sourceFile.close();
    const QByteArray digestBefore = QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256);

    pdf::PDFRepairTransactionOptions options;
    options.sourcePath = sourcePath;
    pdf::PDFRepairTransaction transaction(source, options);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")),
                            QJsonObject{ { QStringLiteral("bleed_mm"), 3.0 },
                                         { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.apply());

    // The reopened candidate is a required out-parameter of the write API, and
    // passing none would fail the call for an unrelated reason: this has to be
    // a real refusal to write over the source, not a failed call.
    pdf::PDFDocument reopenedCandidate;
    const pdf::PDFOperationResult refusedWrite = transaction.serializeCandidate(sourcePath, &reopenedCandidate);
    QVERIFY(!refusedWrite);
    QCOMPARE(refusedWrite.getErrorMessage(),
             QStringLiteral("Refused save: 'received.pdf' is the trusted input artifact; write the candidate to a new path."));
    QFile untouched(sourcePath);
    QVERIFY(untouched.open(QIODevice::ReadOnly));
    const QByteArray digestAfter = QCryptographicHash::hash(untouched.readAll(), QCryptographicHash::Sha256);
    untouched.close();
    QCOMPARE(digestAfter, digestBefore);
}

void RepairOperationTest::sourceBytesSurviveSuccessCancelAndFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("received.pdf"));
    const QString candidatePath = directory.filePath(QStringLiteral("candidate.pdf"));

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    const QByteArray sourceBytes = writeSerializedBytes(source);
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QCOMPARE(sourceFile.write(sourceBytes), qint64(sourceBytes.size()));
    sourceFile.close();
    const QByteArray digest = QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256);

    const auto sourceDigestNow = [&sourcePath]()
    {
        QFile file(sourcePath);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray();
        }
        const QByteArray digest = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        file.close();
        return digest;
    };

    pdf::PDFRepairTransactionOptions options;
    options.sourcePath = sourcePath;
    const QJsonObject parameters{ { QStringLiteral("bleed_mm"), 3.0 }, { QStringLiteral("force"), true } };

    // success
    {
        pdf::PDFRepairTransaction transaction(source, options);
        QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")), parameters));
        QVERIFY(transaction.analyze());
        QVERIFY(transaction.apply());
        pdf::PDFDocument reopenedCandidate;
        const pdf::PDFOperationResult serialized = transaction.serializeCandidate(candidatePath, &reopenedCandidate);
        QVERIFY2(serialized, qPrintable(serialized.getErrorMessage()));
        QCOMPARE(sourceDigestNow(), digest);
    }

    // cancel: analyze, then stop without applying
    {
        pdf::PDFRepairTransaction transaction(source, options);
        QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")), parameters));
        QVERIFY(transaction.analyze());
        QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Planned);
        QCOMPARE(sourceDigestNow(), digest);
    }

    // failure: an operation whose precondition is unsupported (rgb-to-cmyk
    // needs a document with color images) is refused before any mutation. A
    // failing operation is reported as Unsupported rather than as a failed
    // analyze(), so the failure leg pins the status as well as the bytes.
    {
        pdf::PDFRepairTransaction transaction(source, options);
        QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("rgb-to-cmyk")), QJsonObject()));
        QVERIFY(transaction.analyze());
        QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Unsupported);
        QVERIFY(!transaction.apply());
        QVERIFY(transaction.status() != pdf::PDFRepairStatus::Applied);
        QCOMPARE(sourceDigestNow(), digest);
    }
}

void RepairOperationTest::analyze_doesNotMutateSource()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    const qreal originalWidth = source.getCatalog()->getPage(0)->getMediaBox().width();

    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")), QJsonObject{
                                                                                                      { QStringLiteral("bleed_mm"), 3.0 },
                                                                                                      { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), originalWidth);
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Planned);
    QVERIFY(!transaction.plans().front().targets.isEmpty());

    const QByteArray first = QJsonDocument(transaction.plans().front().toJson()).toJson(QJsonDocument::Compact);
    pdf::PDFRepairTransaction second(source);
    QVERIFY(second.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")), QJsonObject{
                                                                                                 { QStringLiteral("bleed_mm"), 3.0 },
                                                                                                 { QStringLiteral("force"), true } }));
    QVERIFY(second.analyze());
    QCOMPARE(first, QJsonDocument(second.plans().front().toJson()).toJson(QJsonDocument::Compact));
}

void RepairOperationTest::unsupportedPrecondition_preventsApply()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();

    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("rgb-to-cmyk")), QJsonObject()));
    QVERIFY(transaction.analyze());
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Unsupported);
    QVERIFY(!transaction.apply());
    QVERIFY(transaction.candidate() == nullptr);
}

void RepairOperationTest::failedOperation_discardsCandidate()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();

    FailingRepair operation;
    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(&operation, QJsonObject()));
    QVERIFY(transaction.analyze());
    QVERIFY(!transaction.apply());
    QCOMPARE(transaction.status(), pdf::PDFRepairStatus::Failed);
    QVERIFY(transaction.candidate() == nullptr);
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
}

void RepairOperationTest::addBleedExpectedChanges_areMeasuredWithoutUnexpectedDiff()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();

    pdf::PDFRepairTransaction transaction(source);
    QVERIFY(transaction.add(pdf::PDFRepairRegistry::instance().find(QStringLiteral("add-bleed")), QJsonObject{
                                                                                                      { QStringLiteral("bleed_mm"), 3.0 },
                                                                                                      { QStringLiteral("mode"), QStringLiteral("mirror") },
                                                                                                      { QStringLiteral("force"), true } }));
    QVERIFY(transaction.analyze());
    QVERIFY(transaction.plans().first().expectedChanges.metadata);
    QVERIFY(transaction.plans().first().expectedChanges.pageBoxes);
    QVERIFY(transaction.plans().first().expectedChanges.pageContent);
    QVERIFY(transaction.plans().first().expectedChanges.images);
    QVERIFY(transaction.apply());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    pdf::PDFRepairDiffOptions options;
    options.renderVisualDiff = !pdf::repairPlansMutatePageContent(transaction.plans());
    pdf::PDFRepairDiffReport report;
    QVERIFY(transaction.compareCandidate(directory.filePath(QStringLiteral("candidate.pdf")), options, &report));
    QVERIFY(report.status == pdf::PDFRepairDiffStatus::Complete || report.status == pdf::PDFRepairDiffStatus::CompleteWithWarnings);
    for (const pdf::PDFRepairStructuralChange& change : report.structuralChanges)
    {
        QVERIFY2(change.classification != pdf::PDFRepairChangeClass::Unexpected,
                 qPrintable(QStringLiteral("unexpected change: %1").arg(change.path)));
    }
    QCOMPARE(source.getCatalog()->getPage(0)->getMediaBox().width(), 100.0);
}

void RepairOperationTest::standardTargets_areExplicitAndStable()
{
    QCOMPARE(pdf::supportedPDFStandardTargets(), (QStringList{
                                                     QStringLiteral("PDF/X-1a:2001"), QStringLiteral("PDF/X-3:2002"),
                                                     QStringLiteral("PDF/X-4"), QStringLiteral("PDF/A-2b") }));
    pdf::PDFStandardTarget target = pdf::PDFStandardTarget::PDFX4;
    QVERIFY(pdf::pdfStandardTargetFromString(QStringLiteral("PDF/X-3:2002"), &target));
    QCOMPARE(target, pdf::PDFStandardTarget::PDFX3_2002);
    QCOMPARE(pdf::pdfStandardTargetToString(pdf::PDFStandardTarget::PDFA2b), QStringLiteral("PDF/A-2b"));
}

QTEST_GUILESS_MAIN(RepairOperationTest)

#if __has_include("tst_repairoperationtest.moc")
#include "tst_repairoperationtest.moc"
#endif
