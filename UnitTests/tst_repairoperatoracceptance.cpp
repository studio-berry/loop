// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#include "operatoracceptancehelpers.h"

#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

class RepairOperatorAcceptanceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void repairOperation_addBleedIsFailClosedAndAtomic();

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
    QCOMPARE(report.value(QStringLiteral("schema")).toString(), QStringLiteral("loupe.repair-operation"));
    QCOMPARE(report.value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    QCOMPARE(report.value(QStringLiteral("diff")).toObject().value(QStringLiteral("summary")).toObject().value(QStringLiteral("unexpected_structural_changes")).toInt(),
             0);
    QVERIFY(!report.value(QStringLiteral("output")).toObject().value(QStringLiteral("sha256")).toString().isEmpty());
}

QTEST_GUILESS_MAIN(RepairOperatorAcceptanceTest)

#if __has_include("tst_repairoperatoracceptance.moc")
#include "tst_repairoperatoracceptance.moc"
#endif
