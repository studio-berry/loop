// Operator acceptance for the closed preflight workflow (#134):
// detect -> fix -> recheck -> sign off -> verify, exercised through the CLI on a
// real fixture, including the fail-closed paths a certified artifact must have.

#include "operatoracceptancehelpers.h"
#include "pdftoolenvelopeutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace
{

QByteArray readPayload(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return file.readAll();
}

QJsonObject parseObject(const QByteArray& payload)
{
    return QJsonDocument::fromJson(payload).object();
}

QString digestOf(const QString& path)
{
    return QString::fromLatin1(operatoracceptance::fileSha256(path).toHex());
}

bool reportHasCheckFinding(const QJsonObject& report, const QString& checkId)
{
    for (const QString& field : { QStringLiteral("errors"), QStringLiteral("warnings") })
    {
        for (const QJsonValue& value : report.value(field).toArray())
        {
            if (value.toObject().value(QStringLiteral("check_id")).toString() == checkId)
            {
                return true;
            }
        }
    }
    return false;
}

}   // namespace

// PdfTool's CLI contract: 0 = success, 1 = findings/refused, other = failure.
class PreflightWorkflowAcceptanceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void workflow_detectFixRecheckSignOffVerify();
    void verifyCertificate_rejectsTamperedArtifact();
    void verifyCertificate_rejectsUncertifiedArtifact();
    void verifyCertificate_failsClosedWithoutAuditHistory();
    void certify_refusesAFailingReport();
    void certificateIsInvalidatedByALaterFix();

private:
    int runPdfTool(const QStringList& arguments, QByteArray* stdOut = nullptr);

    /// Runs detect -> fix -> recheck -> sign off and returns the published artifact
    /// and the certificate issued for it.
    bool buildCertifiedArtifact(QTemporaryDir& directory,
                                QString* source,
                                QString* artifact,
                                QString* certificate);

    QString m_pdfToolPath;
    QString m_profilePath;
};

void PreflightWorkflowAcceptanceTest::initTestCase()
{
    m_pdfToolPath = QStringLiteral(PDFTOOL_EXECUTABLE_PATH);
    QVERIFY2(QFile::exists(m_pdfToolPath), qPrintable(m_pdfToolPath));
    m_profilePath = operatoracceptance::defaultProfilePath();
    QVERIFY2(QFile::exists(m_profilePath), qPrintable(m_profilePath));
}

int PreflightWorkflowAcceptanceTest::runPdfTool(const QStringList& arguments, QByteArray* stdOut)
{
    QByteArray capturedOutput;
    QByteArray capturedError;
    int exitCode = -1;
    if (!operatoracceptance::runPdfTool(m_pdfToolPath, arguments, &capturedOutput, &capturedError, &exitCode))
    {
        return -1;
    }
    if (stdOut)
    {
        *stdOut = capturedOutput;
    }
    return exitCode;
}

bool PreflightWorkflowAcceptanceTest::buildCertifiedArtifact(QTemporaryDir& directory,
                                                             QString* source,
                                                             QString* artifact,
                                                             QString* certificate)
{
    *source = directory.filePath(QStringLiteral("job.pdf"));
    if (!QFile::copy(operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf")), *source))
    {
        return false;
    }

    *artifact = directory.filePath(QStringLiteral("job-bleed.pdf"));
    const QString repairReport = directory.filePath(QStringLiteral("repair-report.json"));
    if (runPdfTool({ QStringLiteral("repair"),
                     *source,
                     QStringLiteral("--operation"),
                     QStringLiteral("add-bleed"),
                     QStringLiteral("--param"),
                     QStringLiteral("bleed_mm=3"),
                     QStringLiteral("--param"),
                     QStringLiteral("mode=mirror"),
                     QStringLiteral("--param"),
                     QStringLiteral("force=true"),
                     QStringLiteral("--profile"),
                     m_profilePath,
                     QStringLiteral("--output"),
                     *artifact,
                     QStringLiteral("--report-file"),
                     repairReport,
                     QStringLiteral("--console-format"),
                     QStringLiteral("json") }) != 0)
    {
        return false;
    }

    *certificate = directory.filePath(QStringLiteral("job-bleed.certificate.json"));
    return runPdfTool({ QStringLiteral("preflight"),
                        *artifact,
                        QStringLiteral("--profile"),
                        m_profilePath,
                        QStringLiteral("--certify"),
                        *certificate,
                        QStringLiteral("--console-format"),
                        QStringLiteral("json") }) == 0 &&
           QFile::exists(*certificate);
}

void PreflightWorkflowAcceptanceTest::workflow_detectFixRecheckSignOffVerify()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString source = directory.filePath(QStringLiteral("job.pdf"));
    QVERIFY(QFile::copy(operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf")), source));
    const QByteArray sourceDigest = operatoracceptance::fileSha256(source);

    // 1. Detect: the report names the bleed defect and fails the verdict.
    QByteArray detectOutput;
    QCOMPARE(runPdfTool({ QStringLiteral("preflight"),
                          source,
                          QStringLiteral("--profile"),
                          m_profilePath,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") },
                        &detectOutput),
             1);
    const QJsonObject detectEnvelope = parseObject(detectOutput);
    QVERIFY(pdfplugin::pdftool::isResultEnvelope(detectEnvelope, QStringLiteral("preflight")));
    const QJsonObject detectReport = pdfplugin::pdftool::reportFromEnvelope(detectEnvelope);
    QCOMPARE(detectReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(),
             QStringLiteral("fail"));
    QVERIFY(reportHasCheckFinding(detectReport, QStringLiteral("bleed")));

    // 2. Fix: the repair publishes the artifact and rechecks it before publication.
    const QString artifact = directory.filePath(QStringLiteral("job-bleed.pdf"));
    const QString repairReportPath = directory.filePath(QStringLiteral("repair-report.json"));
    QCOMPARE(runPdfTool({ QStringLiteral("repair"),
                          source,
                          QStringLiteral("--operation"),
                          QStringLiteral("add-bleed"),
                          QStringLiteral("--param"),
                          QStringLiteral("bleed_mm=3"),
                          QStringLiteral("--param"),
                          QStringLiteral("mode=mirror"),
                          QStringLiteral("--param"),
                          QStringLiteral("force=true"),
                          QStringLiteral("--profile"),
                          m_profilePath,
                          QStringLiteral("--output"),
                          artifact,
                          QStringLiteral("--report-file"),
                          repairReportPath,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") }),
             0);
    QVERIFY(QFile::exists(artifact));

    const QJsonObject repairReport = parseObject(readPayload(repairReportPath));
    QCOMPARE(repairReport.value(QStringLiteral("status")).toString(), QStringLiteral("passed"));
    const QJsonObject postflight = repairReport.value(QStringLiteral("postflight")).toObject();
    QCOMPARE(postflight.value(QStringLiteral("pass")).toBool(), true);
    QCOMPARE(postflight.value(QStringLiteral("inspection_complete")).toBool(), true);

    // The recheck reports what the fix cleared and that it introduced nothing.
    const QJsonObject delta = repairReport.value(QStringLiteral("finding_delta")).toObject();
    QCOMPARE(delta.value(QStringLiteral("compared")).toBool(), true);
    QVERIFY(!delta.value(QStringLiteral("resolved")).toArray().isEmpty());
    QVERIFY(delta.value(QStringLiteral("introduced")).toArray().isEmpty());
    QVERIFY(delta.value(QStringLiteral("incomplete")).toArray().isEmpty());

    // The trusted source is never mutated by a repair.
    QCOMPARE(operatoracceptance::fileSha256(source), sourceDigest);

    // 3. Sign off: certification binds the artifact bytes and the effective profile.
    const QString certificatePath = directory.filePath(QStringLiteral("job-bleed.certificate.json"));
    QCOMPARE(runPdfTool({ QStringLiteral("preflight"),
                          artifact,
                          QStringLiteral("--profile"),
                          m_profilePath,
                          QStringLiteral("--certify"),
                          certificatePath,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") }),
             0);
    QVERIFY(QFile::exists(certificatePath));

    const QJsonObject certificate = parseObject(readPayload(certificatePath));
    QCOMPARE(certificate.value(QStringLiteral("type")).toString(), QStringLiteral("loop-certified-preflight"));
    QCOMPARE(certificate.value(QStringLiteral("document_revision_digest")).toString(), digestOf(artifact));
    QVERIFY(!certificate.value(QStringLiteral("effective_profile_digest")).toString().isEmpty());
    QVERIFY(!certificate.value(QStringLiteral("report_digest")).toString().isEmpty());
    QVERIFY(!certificate.value(QStringLiteral("audit_chain_head_event_id")).toString().isEmpty());

    // 4. Verify: a fresh process re-reads the artifact, certificate and audit history.
    QByteArray verifyOutput;
    QCOMPARE(runPdfTool({ QStringLiteral("verify-certificate"),
                          certificatePath,
                          artifact,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") },
                        &verifyOutput),
             0);
    const QJsonObject verification = parseObject(verifyOutput)
                                         .value(QStringLiteral("data"))
                                         .toObject()
                                         .value(QStringLiteral("verification"))
                                         .toObject();
    QCOMPARE(verification.value(QStringLiteral("valid")).toBool(), true);
    QCOMPARE(verification.value(QStringLiteral("state")).toString(), QStringLiteral("valid"));
}

void PreflightWorkflowAcceptanceTest::verifyCertificate_rejectsTamperedArtifact()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString source;
    QString artifact;
    QString certificate;
    QVERIFY(buildCertifiedArtifact(directory, &source, &artifact, &certificate));

    QFile file(artifact);
    QVERIFY(file.open(QIODevice::Append));
    QVERIFY(file.write("% tampered after certification\n") > 0);
    file.close();

    QByteArray verifyOutput;
    QCOMPARE(runPdfTool({ QStringLiteral("verify-certificate"),
                          certificate,
                          artifact,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") },
                        &verifyOutput),
             1);
    const QJsonObject verification = parseObject(verifyOutput)
                                         .value(QStringLiteral("data"))
                                         .toObject()
                                         .value(QStringLiteral("verification"))
                                         .toObject();
    QCOMPARE(verification.value(QStringLiteral("valid")).toBool(), false);
    QCOMPARE(verification.value(QStringLiteral("state")).toString(), QStringLiteral("invalid-document-changed"));
}

void PreflightWorkflowAcceptanceTest::verifyCertificate_rejectsUncertifiedArtifact()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString source;
    QString artifact;
    QString certificate;
    QVERIFY(buildCertifiedArtifact(directory, &source, &artifact, &certificate));

    QByteArray verifyOutput;
    QCOMPARE(runPdfTool({ QStringLiteral("verify-certificate"),
                          certificate,
                          source,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") },
                        &verifyOutput),
             1);
    const QJsonObject verification = parseObject(verifyOutput)
                                         .value(QStringLiteral("data"))
                                         .toObject()
                                         .value(QStringLiteral("verification"))
                                         .toObject();
    QCOMPARE(verification.value(QStringLiteral("valid")).toBool(), false);
    QCOMPARE(verification.value(QStringLiteral("state")).toString(), QStringLiteral("invalid-document-changed"));
}

void PreflightWorkflowAcceptanceTest::verifyCertificate_failsClosedWithoutAuditHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString source;
    QString artifact;
    QString certificate;
    QVERIFY(buildCertifiedArtifact(directory, &source, &artifact, &certificate));

    // Same bytes, no audit history beside them: verification must not fall back to
    // trusting the certificate payload alone.
    const QString relocated = directory.filePath(QStringLiteral("elsewhere/artifact.pdf"));
    QVERIFY(QDir().mkpath(QFileInfo(relocated).absolutePath()));
    QVERIFY(QFile::copy(artifact, relocated));
    QCOMPARE(operatoracceptance::fileSha256(relocated), operatoracceptance::fileSha256(artifact));

    QByteArray verifyOutput;
    QCOMPARE(runPdfTool({ QStringLiteral("verify-certificate"),
                          certificate,
                          relocated,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") },
                        &verifyOutput),
             1);
    const QJsonObject verification = parseObject(verifyOutput)
                                         .value(QStringLiteral("data"))
                                         .toObject()
                                         .value(QStringLiteral("verification"))
                                         .toObject();
    QCOMPARE(verification.value(QStringLiteral("valid")).toBool(), false);
    QCOMPARE(verification.value(QStringLiteral("state")).toString(), QStringLiteral("invalid-audit-chain-broken"));
    QVERIFY(!verification.value(QStringLiteral("reason")).toString().isEmpty());
}

void PreflightWorkflowAcceptanceTest::certify_refusesAFailingReport()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("job.pdf"));
    QVERIFY(QFile::copy(operatoracceptance::fixturePath(QStringLiteral("bleed-missing.pdf")), source));
    const QString certificatePath = directory.filePath(QStringLiteral("job.certificate.json"));

    // The unrepaired document fails the profile: no certificate may be issued, and
    // no partial certificate file may be left behind.
    QVERIFY(runPdfTool({ QStringLiteral("preflight"),
                         source,
                         QStringLiteral("--profile"),
                         m_profilePath,
                         QStringLiteral("--certify"),
                         certificatePath,
                         QStringLiteral("--console-format"),
                         QStringLiteral("json") }) != 0);
    QVERIFY(!QFile::exists(certificatePath));
}

void PreflightWorkflowAcceptanceTest::certificateIsInvalidatedByALaterFix()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString source;
    QString artifact;
    QString certificate;
    QVERIFY(buildCertifiedArtifact(directory, &source, &artifact, &certificate));

    // A later fix on the certified artifact changes the revision, which must
    // invalidate the certificate recorded for the previous revision.
    const QString secondArtifact = directory.filePath(QStringLiteral("job-bleed-2.pdf"));
    QCOMPARE(runPdfTool({ QStringLiteral("repair"),
                          artifact,
                          QStringLiteral("--operation"),
                          QStringLiteral("add-bleed"),
                          QStringLiteral("--param"),
                          QStringLiteral("bleed_mm=4"),
                          QStringLiteral("--param"),
                          QStringLiteral("mode=mirror"),
                          QStringLiteral("--param"),
                          QStringLiteral("force=true"),
                          QStringLiteral("--profile"),
                          m_profilePath,
                          QStringLiteral("--output"),
                          secondArtifact,
                          QStringLiteral("--report-file"),
                          directory.filePath(QStringLiteral("repair-report-2.json")),
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") }),
             0);
    QVERIFY(QFile::exists(secondArtifact));

    QByteArray verifyOutput;
    QCOMPARE(runPdfTool({ QStringLiteral("verify-certificate"),
                          certificate,
                          artifact,
                          QStringLiteral("--console-format"),
                          QStringLiteral("json") },
                        &verifyOutput),
             1);
    const QJsonObject verification = parseObject(verifyOutput)
                                         .value(QStringLiteral("data"))
                                         .toObject()
                                         .value(QStringLiteral("verification"))
                                         .toObject();
    QCOMPARE(verification.value(QStringLiteral("valid")).toBool(), false);
    QCOMPARE(verification.value(QStringLiteral("state")).toString(), QStringLiteral("invalid-certificate"));
    QVERIFY(verification.value(QStringLiteral("reason")).toString().contains(QStringLiteral("revision changed after a fix")));
}

QTEST_MAIN(PreflightWorkflowAcceptanceTest)

#if __has_include("tst_preflightworkflowacceptance.moc")
#include "tst_preflightworkflowacceptance.moc"
#endif
