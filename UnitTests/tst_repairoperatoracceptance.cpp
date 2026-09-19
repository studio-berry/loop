// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "operatoracceptancehelpers.h"
#include "pdfdocumentbuilder.h"
#include "pdfrepairdiff.h"

#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

class RepairOperatorAcceptanceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void repairOperation_addBleedIsFailClosedAndAtomic();
    void repairOperation_missingProfileNeverPublishesAnUnvalidatedCandidate();
    void repairOperation_introducedFindingNeverPublishes();
    void repairOperation_malformedProfileNeverPublishes();
    void repairOperation_unicodeAndSpacePaths_addBleedPassesWithoutUnexpectedChange();

private:
    QString m_defaultProfilePath;
    QString m_pdfToolPath;
};

void RepairOperatorAcceptanceTest::initTestCase()
{
    m_defaultProfilePath = operatoracceptance::defaultProfilePath();
    QVERIFY2(QFile::exists(m_defaultProfilePath),
             qPrintable(QStringLiteral("Missing default profile at %1").arg(m_defaultProfilePath)));

    m_pdfToolPath = QStringLiteral(PDFTOOL_EXECUTABLE_PATH);
    QVERIFY2(QFileInfo(m_pdfToolPath).isExecutable(),
             qPrintable(QStringLiteral("PdfTool not found or not executable at %1").arg(m_pdfToolPath)));
}

void RepairOperatorAcceptanceTest::repairOperation_addBleedIsFailClosedAndAtomic()
{
    const QString pdfPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    const QByteArray beforeHash = operatoracceptance::fileSha256(pdfPath);
    QVERIFY(!beforeHash.isEmpty());

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString outputPath = temporaryDirectory.filePath(QStringLiteral("repair-bleed-fixed.pdf"));
    const QString reportPath = temporaryDirectory.filePath(QStringLiteral("repair-report.json"));

    QByteArray stdOut;
    QByteArray stdErr;
    int exitCode = -1;
    QVERIFY(operatoracceptance::runPdfTool(m_pdfToolPath,
                                           { QStringLiteral("repair"),
                                             pdfPath,
                                             QStringLiteral("--operation"),
                                             QStringLiteral("add-bleed"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("bleed_mm=3"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("mode=mirror"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("force=true"),
                                             QStringLiteral("--profile"),
                                             m_defaultProfilePath,
                                             QStringLiteral("--output"),
                                             outputPath,
                                             QStringLiteral("--report-file"),
                                             reportPath,
                                             QStringLiteral("--console-format"),
                                             QStringLiteral("json") },
                                           &stdOut,
                                           &stdErr,
                                           &exitCode));
    QCOMPARE(exitCode, 0);
    QVERIFY2(stdErr.trimmed().isEmpty(), qPrintable(QString::fromUtf8(stdErr)));
    QVERIFY(QFile::exists(outputPath));
    QVERIFY(QFile::exists(reportPath));
    QCOMPARE(operatoracceptance::fileSha256(pdfPath), beforeHash);

    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument reportDocument = QJsonDocument::fromJson(reportFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(reportDocument.isObject());
    const QJsonObject report = reportDocument.object();
    QCOMPARE(report.value(QStringLiteral("schema")).toString(), QStringLiteral("loop.repair-operation"));
    QCOMPARE(report.value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    const QJsonArray results = report.value(QStringLiteral("results")).toArray();
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().toObject().value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    const QJsonArray validations = results.first().toObject().value(QStringLiteral("validation")).toArray();
    QCOMPARE(validations.size(), 2);
    QCOMPARE(validations.at(0).toObject().value(QStringLiteral("validator")).toString(), QStringLiteral("structural-integrity"));
    QCOMPARE(validations.at(0).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    QCOMPARE(validations.at(1).toObject().value(QStringLiteral("validator")).toString(), QStringLiteral("normal-preflight"));
    QCOMPARE(validations.at(1).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    QCOMPARE(results.first().toObject().value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("pass"));
    QCOMPARE(report.value(QStringLiteral("diff")).toObject().value(QStringLiteral("summary")).toObject().value(QStringLiteral("unexpected_structural_changes")).toInt(),
             0);
    const QJsonObject findingDelta = report.value(QStringLiteral("finding_delta")).toObject();
    QVERIFY(!findingDelta.isEmpty());
    QVERIFY(!findingDelta.value(QStringLiteral("resolved")).toArray().isEmpty());
    QVERIFY(findingDelta.value(QStringLiteral("introduced")).toArray().isEmpty());
    QVERIFY(findingDelta.value(QStringLiteral("incomplete")).toArray().isEmpty());
    QVERIFY(!report.value(QStringLiteral("output")).toObject().value(QStringLiteral("sha256")).toString().isEmpty());
}

void RepairOperatorAcceptanceTest::repairOperation_missingProfileNeverPublishesAnUnvalidatedCandidate()
{
    const QString source = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    const QByteArray originalSha256 = operatoracceptance::fileSha256(source);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString output = directory.filePath(QStringLiteral("unvalidated.pdf"));
    const QString reportPath = directory.filePath(QStringLiteral("unvalidated-report.json"));

    QByteArray out;
    QByteArray err;
    int exitCode = -1;
    QVERIFY(operatoracceptance::runPdfTool(
        m_pdfToolPath,
        { QStringLiteral("repair"), source,
          QStringLiteral("--operation"), QStringLiteral("add-bleed"),
          QStringLiteral("--param"), QStringLiteral("bleed_mm=3"),
          QStringLiteral("--param"), QStringLiteral("mode=mirror"),
          QStringLiteral("--param"), QStringLiteral("force=true"),
          QStringLiteral("--output"), output,
          QStringLiteral("--allow-incomplete"),
          QStringLiteral("--report-file"), reportPath,
          QStringLiteral("--console-format"), QStringLiteral("json") },
        &out, &err, &exitCode));

    QVERIFY(exitCode != 0);
    QVERIFY(!QFile::exists(output));
    QVERIFY(QFile::exists(reportPath));
    QCOMPARE(operatoracceptance::fileSha256(source), originalSha256);
    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    const QJsonObject report = QJsonDocument::fromJson(reportFile.readAll()).object();
    QCOMPARE(report.value(QStringLiteral("status")).toString(), QStringLiteral("incomplete"));
    const QJsonObject operation = report.value(QStringLiteral("results")).toArray().first().toObject();
    QCOMPARE(operation.value(QStringLiteral("status")).toString(), QStringLiteral("incomplete"));
    QVERIFY(!operation.value(QStringLiteral("incomplete_reasons")).toArray().isEmpty());
    QCOMPARE(operation.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(),
             QStringLiteral("incomplete"));
    QVERIFY(!operation.value(QStringLiteral("validation")).toArray().isEmpty());
}

void RepairOperatorAcceptanceTest::repairOperation_introducedFindingNeverPublishes()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    const pdf::PDFDocument source = builder.build();
    const QString sourcePath = temporaryDirectory.filePath(QStringLiteral("source.pdf"));
    pdf::PDFDocument reopenedSource;
    QVERIFY(pdf::PDFRepairDiffEngine::buildSerializedCandidate(
        source,
        [](pdf::PDFDocument*)
        { return pdf::PDFOperationResult(true); },
        sourcePath,
        &reopenedSource,
        nullptr));

    const QString profilePath = temporaryDirectory.filePath(QStringLiteral("introduced-profile.json"));
    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly));
    const QJsonObject profile{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("id"), QStringLiteral("introduced-finding-test") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("name"), QStringLiteral("Introduced finding test") },
        { QStringLiteral("checks"), QJsonArray{
                                        QJsonObject{
                                            { QStringLiteral("id"), QStringLiteral("page-size") },
                                            { QStringLiteral("expected_width_pt"), 100.0 },
                                            { QStringLiteral("expected_height_pt"), 100.0 },
                                            { QStringLiteral("tolerance_pt"), 0.0 },
                                            { QStringLiteral("severity"), QStringLiteral("error") } } } }
    };
    QVERIFY(profileFile.write(QJsonDocument(profile).toJson(QJsonDocument::Indented)) > 0);
    profileFile.close();

    const QString outputPath = temporaryDirectory.filePath(QStringLiteral("must-not-exist.pdf"));
    const QString reportPath = temporaryDirectory.filePath(QStringLiteral("introduced-report.json"));
    QByteArray stdOut;
    QByteArray stdErr;
    int exitCode = -1;
    QVERIFY(operatoracceptance::runPdfTool(m_pdfToolPath,
                                           { QStringLiteral("repair"),
                                             sourcePath,
                                             QStringLiteral("--operation"),
                                             QStringLiteral("add-bleed"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("bleed_mm=3"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("force=true"),
                                             QStringLiteral("--profile"),
                                             profilePath,
                                             QStringLiteral("--output"),
                                             outputPath,
                                             QStringLiteral("--report-file"),
                                             reportPath,
                                             QStringLiteral("--console-format"),
                                             QStringLiteral("json") },
                                           &stdOut,
                                           &stdErr,
                                           &exitCode));

    QVERIFY(exitCode != 0);
    QVERIFY(!QFile::exists(outputPath));
    QVERIFY(QFile::exists(reportPath));

    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument reportDocument = QJsonDocument::fromJson(reportFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    const QJsonObject report = reportDocument.object();
    QCOMPARE(report.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    const QJsonObject findingDelta = report.value(QStringLiteral("finding_delta")).toObject();
    QVERIFY(!findingDelta.value(QStringLiteral("introduced")).toArray().isEmpty());
}

void RepairOperatorAcceptanceTest::repairOperation_malformedProfileNeverPublishes()
{
    const QString pdfPath = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString malformedProfilePath = temporaryDirectory.filePath(QStringLiteral("malformed-profile.json"));
    QFile malformedProfile(malformedProfilePath);
    QVERIFY(malformedProfile.open(QIODevice::WriteOnly));
    QVERIFY(malformedProfile.write("{") > 0);

    const QString outputPath = temporaryDirectory.filePath(QStringLiteral("must-not-exist.pdf"));
    const QString reportPath = temporaryDirectory.filePath(QStringLiteral("malformed-report.json"));
    QByteArray stdOut;
    QByteArray stdErr;
    int exitCode = -1;
    QVERIFY(operatoracceptance::runPdfTool(m_pdfToolPath,
                                           { QStringLiteral("repair"),
                                             pdfPath,
                                             QStringLiteral("--operation"),
                                             QStringLiteral("add-bleed"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("bleed_mm=3"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("force=true"),
                                             QStringLiteral("--profile"),
                                             malformedProfilePath,
                                             QStringLiteral("--output"),
                                             outputPath,
                                             QStringLiteral("--report-file"),
                                             reportPath,
                                             QStringLiteral("--console-format"),
                                             QStringLiteral("json") },
                                           &stdOut,
                                           &stdErr,
                                           &exitCode));

    QVERIFY(exitCode != 0);
    QVERIFY(!QFile::exists(outputPath));
}

void RepairOperatorAcceptanceTest::repairOperation_unicodeAndSpacePaths_addBleedPassesWithoutUnexpectedChange()
{
    const QString sourcePdf = operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFile::exists(sourcePdf));

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QString nestedDir = temporaryDirectory.path() + QStringLiteral("/shop files");
    QVERIFY(QDir().mkpath(nestedDir));

    const QString targetPdf = nestedDir + QStringLiteral("/café poster.pdf");
    QVERIFY(QFile::copy(sourcePdf, targetPdf));
    QVERIFY(QFile::exists(targetPdf));

    const QString outputPath = nestedDir + QStringLiteral("/café poster_bleed.pdf");
    const QString reportPath = nestedDir + QStringLiteral("/repair-report.json");

    QByteArray stdOut;
    QByteArray stdErr;
    int exitCode = -1;
    QVERIFY(operatoracceptance::runPdfTool(m_pdfToolPath,
                                           { QStringLiteral("repair"),
                                             targetPdf,
                                             QStringLiteral("--operation"),
                                             QStringLiteral("add-bleed"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("bleed_mm=3"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("mode=mirror"),
                                             QStringLiteral("--param"),
                                             QStringLiteral("force=true"),
                                             QStringLiteral("--profile"),
                                             m_defaultProfilePath,
                                             QStringLiteral("--output"),
                                             outputPath,
                                             QStringLiteral("--report-file"),
                                             reportPath,
                                             QStringLiteral("--console-format"),
                                             QStringLiteral("json") },
                                           &stdOut,
                                           &stdErr,
                                           &exitCode));
    QCOMPARE(exitCode, 0);
    QVERIFY2(stdErr.trimmed().isEmpty(), qPrintable(QString::fromUtf8(stdErr)));
    QVERIFY(QFile::exists(outputPath));
    QVERIFY(QFile::exists(reportPath));

    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument reportDocument = QJsonDocument::fromJson(reportFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(reportDocument.isObject());
    const QJsonObject report = reportDocument.object();
    QCOMPARE(report.value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    QCOMPARE(report.value(QStringLiteral("diff")).toObject().value(QStringLiteral("summary")).toObject().value(QStringLiteral("unexpected_structural_changes")).toInt(),
             0);
}

QTEST_GUILESS_MAIN(RepairOperatorAcceptanceTest)

#if __has_include("tst_repairoperatoracceptance.moc")
#include "tst_repairoperatoracceptance.moc"
#endif
