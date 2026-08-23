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

// End-to-end operator-loop acceptance for MIC-300 (Phase C).
// Drives PdfTool preflight + add-bleed via QProcess (same sidecar contract as the
// Editor plugin) and validates report handling helpers used by LoupePreflightPlugin.
// GUI navigation/overlay/cancel flows are covered in docs/v1-operator-acceptance.md.

#include "pdftoolenvelopeutils.h"
#include "preflightsidecarutils.h"
#include "operatoracceptancehelpers.h"

#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QSizeF>
#include <QTemporaryDir>
#include <QVector>

namespace
{

constexpr qreal POINTS_PER_MM = 72.0 / 25.4;

struct OperatorCorpusEntry
{
    const char* id;
    const char* pdf;
    bool expectPass;
    const char* expectCheckId;
    bool skipPreflightJson;
};

constexpr OperatorCorpusEntry OPERATOR_CORPUS[] = {
    { "clean-adequate-bleed", "bleed-adequate.pdf", true, nullptr, false },
    { "missing-bleed", "bleed-missing.pdf", false, "bleed", false },
    { "live-text-embedded", "font-embedded.pdf", true, nullptr, false },
    { "live-text-not-embedded", "font-not-embedded.pdf", false, "embedded-fonts", false },
    { "image-only-raster", "image-dpi-ok.pdf", true, nullptr, false },
    { "malformed-input", "malformed-not-pdf.pdf", false, nullptr, true },
};

QStringList checkIdsOf(const QJsonObject& report)
{
    QStringList ids;
    for (const QString& section : { QStringLiteral("errors"), QStringLiteral("warnings") })
    {
        for (const QJsonValue& finding : report.value(section).toArray())
        {
            ids << finding.toObject().value(QStringLiteral("check_id")).toString();
        }
    }
    return ids;
}

QJsonObject findingWithCheckId(const QJsonObject& report, const QString& checkId)
{
    for (const QString& section : { QStringLiteral("errors"), QStringLiteral("warnings") })
    {
        for (const QJsonValue& findingValue : report.value(section).toArray())
        {
            const QJsonObject finding = findingValue.toObject();
            if (finding.value(QStringLiteral("check_id")).toString() == checkId)
            {
                return finding;
            }
        }
    }

    return QJsonObject();
}

bool hasAddBleedFixup(const QJsonObject& report, QJsonObject* fixupObject = nullptr)
{
    for (const QJsonValue& fixupValue : report.value(QStringLiteral("fixups_available")).toArray())
    {
        const QJsonObject fixup = fixupValue.toObject();
        if (fixup.value(QStringLiteral("id")).toString() == QStringLiteral("add-bleed"))
        {
            if (fixupObject)
            {
                *fixupObject = fixup;
            }
            return true;
        }
    }
    return false;
}

bool advertisedAddBleedParams(const QJsonObject& report, QString* mode, QString* bleedMm)
{
    QJsonObject fixup;
    if (!hasAddBleedFixup(report, &fixup))
    {
        return false;
    }

    const QJsonObject params = fixup.value(QStringLiteral("params")).toObject();
    const qreal amountPt = params.value(QStringLiteral("amount_pt")).toDouble(fixup.value(QStringLiteral("amount_pt")).toDouble(9.0));

    if (mode)
    {
        *mode = params.value(QStringLiteral("mode")).toString(QStringLiteral("mirror"));
    }

    if (bleedMm)
    {
        *bleedMm = QString::number(amountPt / POINTS_PER_MM, 'f', 3);
    }

    return amountPt > 0.0;
}

bool writeLargeFormatPdf(const QString& path, double widthInches, double heightInches)
{
    const int widthPt = static_cast<int>(widthInches * 72.0 + 0.5);
    const int heightPt = static_cast<int>(heightInches * 72.0 + 0.5);
    QByteArray pdf("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    QVector<int> offsets(5);

    auto appendObject = [&pdf, &offsets](int number, const QByteArray& body)
    {
        offsets[number] = pdf.size();
        pdf.append(QByteArray::number(number));
        pdf.append(" 0 obj\n");
        pdf.append(body);
        if (!body.endsWith('\n'))
        {
            pdf.append('\n');
        }
        pdf.append("endobj\n");
    };

    appendObject(1, "<< /Type /Catalog /Pages 2 0 R >>\n");
    appendObject(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n");
    appendObject(3, QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ") + QByteArray::number(widthPt) + ' ' + QByteArray::number(heightPt) + "] /TrimBox [0 0 " + QByteArray::number(widthPt) + ' ' + QByteArray::number(heightPt) + "] /Resources << >> /Contents 4 0 R >>\n");
    appendObject(4, "<< /Length 0 >>\nstream\n\nendstream\n");

    const int xrefOffset = pdf.size();
    pdf.append("xref\n0 5\n0000000000 65535 f \n");
    for (int number = 1; number < offsets.size(); ++number)
    {
        pdf.append(QByteArray::number(offsets[number]).rightJustified(10, '0'));
        pdf.append(" 00000 n \n");
    }
    pdf.append("trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n");
    pdf.append(QByteArray::number(xrefOffset));
    pdf.append("\n%%EOF\n");

    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(pdf) == pdf.size();
}

}   // namespace

class OperatorAcceptanceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void representativeCorpus_preflight_data();
    void representativeCorpus_preflight();

    void operatorLoop_bleedFixupAndRevalidate();
    void operatorLoop_preservesOriginalBytes();

    void overwriteExplicit_addBleedRequiresOverwriteFlag();
    void addBleedDryRun_largeFormatPlansWithoutRasterOrOutput();

    void unicodeAndSpacePaths_preflightAndAddBleedSucceed();

    void malformedInput_failsWithoutCrash();
    void invalidProfile_returnsActionableError();
    void profileSemanticMismatch_returnsProfileFinding();

    void reportContract_rejectsUnsupportedSchema();
    void reportContract_classifiesVisualOverlays();
    void reportContract_allowedPropertiesMatchSchema();

    void sidecarCancellation_terminatesCleanly();

    void corpus_performanceBaseline();

private:
    bool runPdfTool(const QStringList& arguments,
                    QByteArray* stdOut,
                    QByteArray* stdErr,
                    int* exitCode,
                    qint64* peakChildMemoryKb = nullptr) const;
    bool runPreflight(const QString& pdfPath,
                      const QString& profilePath,
                      QJsonObject* report,
                      int* exitCode,
                      qint64* peakChildMemoryKb = nullptr) const;
    bool runAddBleed(const QString& inputPath,
                     const QString& outputPath,
                     const QString& mode,
                     const QString& bleedMm,
                     int* exitCode) const;
    QString fixturePath(const QString& pdf) const;
    void assertMalformedPreflightFailure(const QString& pdfPath) const;

    QString m_defaultProfilePath;
    QString m_pdfToolPath;
};

void OperatorAcceptanceTest::initTestCase()
{
    m_defaultProfilePath = operatoracceptance::defaultProfilePath();
    QVERIFY2(QFile::exists(m_defaultProfilePath),
             qPrintable(QStringLiteral("Missing default profile at %1").arg(m_defaultProfilePath)));

    m_pdfToolPath = QStringLiteral(PDFTOOL_EXECUTABLE_PATH);
    QVERIFY2(QFileInfo(m_pdfToolPath).isExecutable(),
             qPrintable(QStringLiteral("PdfTool not found or not executable at %1").arg(m_pdfToolPath)));
}

bool OperatorAcceptanceTest::runPdfTool(const QStringList& arguments,
                                        QByteArray* stdOut,
                                        QByteArray* stdErr,
                                        int* exitCode,
                                        qint64* peakChildMemoryKb) const
{
    return operatoracceptance::runPdfTool(m_pdfToolPath, arguments, stdOut, stdErr, exitCode, peakChildMemoryKb);
}

bool OperatorAcceptanceTest::runPreflight(const QString& pdfPath,
                                          const QString& profilePath,
                                          QJsonObject* report,
                                          int* exitCode,
                                          qint64* peakChildMemoryKb) const
{
    QByteArray stdOut;
    if (!runPdfTool({ QStringLiteral("preflight"), pdfPath, QStringLiteral("--profile"), profilePath },
                    &stdOut,
                    nullptr,
                    exitCode,
                    peakChildMemoryKb))
    {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdOut, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return false;
    }

    const QJsonObject envelope = document.object();
    if (!pdfplugin::pdftool::isResultEnvelope(envelope, QStringLiteral("preflight")))
    {
        return false;
    }

    if (report)
    {
        *report = pdfplugin::pdftool::reportFromEnvelope(envelope);
    }

    return true;
}

bool OperatorAcceptanceTest::runAddBleed(const QString& inputPath,
                                         const QString& outputPath,
                                         const QString& mode,
                                         const QString& bleedMm,
                                         int* exitCode) const
{
    return runPdfTool({
                          QStringLiteral("add-bleed"),
                          inputPath,
                          QStringLiteral("--output"),
                          outputPath,
                          QStringLiteral("--mode"),
                          mode,
                          QStringLiteral("--bleed-mm"),
                          bleedMm,
                          QStringLiteral("--force"),
                      },
                      nullptr, nullptr, exitCode);
}

QString OperatorAcceptanceTest::fixturePath(const QString& pdf) const
{
    return operatoracceptance::fixturePath(pdf);
}

void OperatorAcceptanceTest::assertMalformedPreflightFailure(const QString& pdfPath) const
{
    int exitCode = -1;
    QByteArray stdErr;
    QVERIFY(runPdfTool({ QStringLiteral("preflight"), pdfPath, QStringLiteral("--profile"), m_defaultProfilePath },
                       nullptr,
                       &stdErr,
                       &exitCode));
    QVERIFY2(exitCode != 0, "Malformed input must not report a successful preflight run.");
    QVERIFY2(exitCode != 1, "Malformed input must not masquerade as a findings exit code.");
}

void OperatorAcceptanceTest::representativeCorpus_preflight_data()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("pdf");
    QTest::addColumn<bool>("expectPass");
    QTest::addColumn<QString>("expectCheckId");
    QTest::addColumn<bool>("skipPreflightJson");

    for (const OperatorCorpusEntry& entry : OPERATOR_CORPUS)
    {
        QTest::newRow(entry.id) << QString::fromLatin1(entry.id)
                                << QString::fromLatin1(entry.pdf)
                                << entry.expectPass
                                << (entry.expectCheckId ? QString::fromLatin1(entry.expectCheckId) : QString())
                                << entry.skipPreflightJson;
    }
}

void OperatorAcceptanceTest::representativeCorpus_preflight()
{
    QFETCH(QString, pdf);
    QFETCH(bool, expectPass);
    QFETCH(QString, expectCheckId);
    QFETCH(bool, skipPreflightJson);

    const QString pdfPath = fixturePath(pdf);
    QVERIFY2(QFile::exists(pdfPath), qPrintable(QStringLiteral("Missing fixture %1").arg(pdfPath)));

    if (skipPreflightJson)
    {
        assertMalformedPreflightFailure(pdfPath);
        return;
    }

    QJsonObject report;
    int exitCode = -1;
    QVERIFY(runPreflight(pdfPath, m_defaultProfilePath, &report, &exitCode));

    QVERIFY(pdfplugin::preflight::validateNormalizedReport(report));
    QCOMPARE(report.value(QStringLiteral("pass")).toBool(), expectPass);
    QCOMPARE(exitCode, expectPass ? 0 : 1);

    if (!expectCheckId.isEmpty())
    {
        QVERIFY(checkIdsOf(report).contains(expectCheckId));
    }
}

void OperatorAcceptanceTest::operatorLoop_bleedFixupAndRevalidate()
{
    const QString pdfPath = fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    QJsonObject initialReport;
    int initialExitCode = -1;
    QVERIFY(runPreflight(pdfPath, m_defaultProfilePath, &initialReport, &initialExitCode));
    QCOMPARE(initialExitCode, 1);
    QVERIFY(pdfplugin::preflight::validateNormalizedReport(initialReport));

    QString mode;
    QString bleedMm;
    QVERIFY(advertisedAddBleedParams(initialReport, &mode, &bleedMm));

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString outputPath = temporaryDirectory.filePath(QStringLiteral("bleed-fixed.pdf"));

    int addBleedExitCode = -1;
    QVERIFY(runAddBleed(pdfPath, outputPath, mode, bleedMm, &addBleedExitCode));
    QCOMPARE(addBleedExitCode, 0);
    QVERIFY(QFile::exists(outputPath));

    QJsonObject postReport;
    int postExitCode = -1;
    QVERIFY(runPreflight(outputPath, m_defaultProfilePath, &postReport, &postExitCode));
    QCOMPARE(postExitCode, 0);

    const QJsonObject filteredPostReport = pdfplugin::preflight::filterAdvertisedFixups(postReport);
    QVERIFY(pdfplugin::preflight::validateNormalizedReport(filteredPostReport));
    QVERIFY(filteredPostReport.value(QStringLiteral("pass")).toBool());
    QVERIFY(!checkIdsOf(filteredPostReport).contains(QStringLiteral("bleed")));
}

void OperatorAcceptanceTest::operatorLoop_preservesOriginalBytes()
{
    const QString pdfPath = fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    QJsonObject initialReport;
    int initialExitCode = -1;
    QVERIFY(runPreflight(pdfPath, m_defaultProfilePath, &initialReport, &initialExitCode));

    QString mode;
    QString bleedMm;
    QVERIFY(advertisedAddBleedParams(initialReport, &mode, &bleedMm));

    const QByteArray beforeHash = operatoracceptance::fileSha256(pdfPath);
    QVERIFY(!beforeHash.isEmpty());

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString outputPath = temporaryDirectory.filePath(QStringLiteral("bleed-fixed.pdf"));

    int addBleedExitCode = -1;
    QVERIFY(runAddBleed(pdfPath, outputPath, mode, bleedMm, &addBleedExitCode));
    QCOMPARE(addBleedExitCode, 0);

    QCOMPARE(operatoracceptance::fileSha256(pdfPath), beforeHash);
    QVERIFY(!operatoracceptance::fileSha256(outputPath).isEmpty());
    QVERIFY(operatoracceptance::fileSha256(outputPath) != beforeHash);
}

void OperatorAcceptanceTest::overwriteExplicit_addBleedRequiresOverwriteFlag()
{
    // Overwrite-explicit contract (MIC-310): a second add-bleed run writing to an
    // existing output must be refused unless --overwrite is passed, and the
    // existing file must survive the refused run.
    const QString pdfPath = fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString outputPath = temporaryDirectory.filePath(QStringLiteral("bleed-fixed.pdf"));

    int firstExitCode = -1;
    QVERIFY(runPdfTool({ QStringLiteral("add-bleed"), pdfPath, QStringLiteral("--output"), outputPath,
                         QStringLiteral("--mode"), QStringLiteral("mirror"), QStringLiteral("--bleed-mm"), QStringLiteral("5") },
                       nullptr, nullptr, &firstExitCode));
    QCOMPARE(firstExitCode, 0);
    QVERIFY(QFile::exists(outputPath));
    const QByteArray firstHash = operatoracceptance::fileSha256(outputPath);
    QVERIFY(!firstHash.isEmpty());

    int refusedExitCode = -1;
    QByteArray refusedError;
    QVERIFY(runPdfTool({ QStringLiteral("add-bleed"), pdfPath, QStringLiteral("--output"), outputPath,
                         QStringLiteral("--mode"), QStringLiteral("mirror"), QStringLiteral("--bleed-mm"), QStringLiteral("5") },
                       nullptr, &refusedError, &refusedExitCode));
    QVERIFY2(refusedExitCode != 0, "add-bleed must not overwrite the existing output without --overwrite.");
    QVERIFY(!refusedError.trimmed().isEmpty());
    QCOMPARE(operatoracceptance::fileSha256(outputPath), firstHash);

    int overwriteExitCode = -1;
    QVERIFY(runPdfTool({ QStringLiteral("add-bleed"), pdfPath, QStringLiteral("--output"), outputPath,
                         QStringLiteral("--mode"), QStringLiteral("mirror"), QStringLiteral("--bleed-mm"), QStringLiteral("5"),
                         QStringLiteral("--overwrite") },
                       nullptr, nullptr, &overwriteExitCode));
    QCOMPARE(overwriteExitCode, 0);
    QVERIFY(QFile::exists(outputPath));
    // add-bleed is deterministic, so a re-run of the same input legitimately
    // yields the same bytes; the contract is that --overwrite unblocks the write
    // (exit 0 versus the refusal above), not that the bytes must differ.
    QVERIFY(!operatoracceptance::fileSha256(outputPath).isEmpty());
}

void OperatorAcceptanceTest::addBleedDryRun_largeFormatPlansWithoutRasterOrOutput()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    qint64 peakMemoryKb = -1;
    for (const auto& size : { QSizeF(48.0, 96.0), QSizeF(240.0, 60.0) })
    {
        const QString inputPath = temporaryDirectory.filePath(
            QStringLiteral("large-%1x%2.pdf").arg(size.width()).arg(size.height()));
        const QString outputPath = temporaryDirectory.filePath(
            QStringLiteral("should-not-be-written-%1x%2.pdf").arg(size.width()).arg(size.height()));
        QVERIFY(writeLargeFormatPdf(inputPath, size.width(), size.height()));

        QByteArray stdOut;
        QByteArray stdErr;
        int exitCode = -1;
        qint64 childMemoryKb = -1;
        QVERIFY(runPdfTool({ QStringLiteral("add-bleed"),
                             inputPath,
                             QStringLiteral("--output"),
                             outputPath,
                             QStringLiteral("--dry-run"),
                             QStringLiteral("--console-format"),
                             QStringLiteral("json"),
                             QStringLiteral("--bleed-mm"),
                             QStringLiteral("3"),
                             QStringLiteral("--dpi"),
                             QStringLiteral("300") },
                           &stdOut,
                           &stdErr,
                           &exitCode,
                           &childMemoryKb));
        QCOMPARE(exitCode, 0);
        QVERIFY2(stdErr.trimmed().isEmpty(), qPrintable(QString::fromUtf8(stdErr)));
        QVERIFY(!QFile::exists(outputPath));
        peakMemoryKb = qMax(peakMemoryKb, childMemoryKb);

        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(stdOut, &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QVERIFY(json.isObject());
        const QJsonObject envelope = json.object();
        QCOMPARE(envelope.value(QStringLiteral("command")).toString(), QStringLiteral("add-bleed"));
        QCOMPARE(envelope.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        QVERIFY(QString::fromUtf8(stdOut).contains(QStringLiteral("dry-run")));

        const QJsonArray outputs = envelope.value(QStringLiteral("outputs")).toArray();
        QCOMPARE(outputs.size(), 1);
        QCOMPARE(outputs.first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("planned"));
    }

#ifdef Q_OS_LINUX
    if (peakMemoryKb > 0)
    {
        QVERIFY2(peakMemoryKb < 1024 * 1024,
                 qPrintable(QStringLiteral("large-format dry-run peak VmHWM was %1 kB").arg(peakMemoryKb)));
    }
#endif
}

void OperatorAcceptanceTest::unicodeAndSpacePaths_preflightAndAddBleedSucceed()
{
    const QString sourcePdf = fixturePath(QStringLiteral("bleed-missing.pdf"));
    QVERIFY(QFile::exists(sourcePdf));

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const QString nestedDir = temporaryDirectory.path() + QStringLiteral("/shop files");
    QVERIFY(QDir().mkpath(nestedDir));

    const QString targetPdf = nestedDir + QStringLiteral("/café poster.pdf");
    QVERIFY(QFile::copy(sourcePdf, targetPdf));
    QVERIFY(QFile::exists(targetPdf));

    QJsonObject report;
    int exitCode = -1;
    QVERIFY(runPreflight(targetPdf, m_defaultProfilePath, &report, &exitCode));
    QCOMPARE(exitCode, 1);

    QString mode;
    QString bleedMm;
    QVERIFY(advertisedAddBleedParams(report, &mode, &bleedMm));

    const QString outputPdf = nestedDir + QStringLiteral("/café poster_bleed.pdf");
    int addBleedExitCode = -1;
    QVERIFY(runAddBleed(targetPdf, outputPdf, mode, bleedMm, &addBleedExitCode));
    QCOMPARE(addBleedExitCode, 0);
    QVERIFY(QFile::exists(outputPdf));

    QJsonObject postReport;
    int postExitCode = -1;
    QVERIFY(runPreflight(outputPdf, m_defaultProfilePath, &postReport, &postExitCode));
    QCOMPARE(postExitCode, 0);
    QVERIFY(postReport.value(QStringLiteral("pass")).toBool());
}

void OperatorAcceptanceTest::malformedInput_failsWithoutCrash()
{
    assertMalformedPreflightFailure(fixturePath(QStringLiteral("malformed-not-pdf.pdf")));
}

void OperatorAcceptanceTest::invalidProfile_returnsActionableError()
{
    const QString pdfPath = fixturePath(QStringLiteral("bleed-adequate.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString badProfilePath = temporaryDirectory.filePath(QStringLiteral("broken-profile.json"));

    QFile badProfileFile(badProfilePath);
    QVERIFY(badProfileFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    badProfileFile.write("{ not valid json");
    badProfileFile.close();

    // preflight defaults to JSON console output (see main.cpp), and
    // PDFConsole::setDiagnosticSink() intentionally captures error output as
    // structured diagnostics in that envelope instead of writing to stderr - so
    // the actionable error lands in the JSON envelope on stdout, not stderr.
    int exitCode = -1;
    QByteArray stdOut;
    QVERIFY(runPdfTool({ QStringLiteral("preflight"), pdfPath, QStringLiteral("--profile"), badProfilePath },
                       &stdOut,
                       nullptr,
                       &exitCode));
    QVERIFY(exitCode != 0);
    QVERIFY(exitCode != 1);

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(stdOut, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(json.isObject());
    const QJsonArray diagnostics = json.object().value(QStringLiteral("diagnostics")).toArray();
    QVERIFY(!diagnostics.isEmpty());
    QVERIFY(!diagnostics.first().toObject().value(QStringLiteral("message")).toString().isEmpty());
}

void OperatorAcceptanceTest::profileSemanticMismatch_returnsProfileFinding()
{
    const QString pdfPath = fixturePath(QStringLiteral("bleed-adequate.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString profilePath = temporaryDirectory.filePath(QStringLiteral("empty-checks-profile.json"));

    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Broken profile") },
        { QStringLiteral("checks"), QJsonArray() },
    };

    QFile profileFile(profilePath);
    QVERIFY(profileFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    profileFile.write(QJsonDocument(profile).toJson(QJsonDocument::Compact));
    profileFile.close();

    QJsonObject report;
    int exitCode = -1;
    QVERIFY(runPreflight(pdfPath, profilePath, &report, &exitCode));
    QCOMPARE(exitCode, 1);
    QVERIFY(!report.value(QStringLiteral("pass")).toBool());

    const QJsonArray errors = report.value(QStringLiteral("errors")).toArray();
    QVERIFY(!errors.isEmpty());
    const QJsonObject profileFinding = errors.first().toObject();
    QCOMPARE(profileFinding.value(QStringLiteral("type")).toString(), QStringLiteral("profile"));
    QCOMPARE(profileFinding.value(QStringLiteral("scope")).toString(), QStringLiteral("document"));
    QVERIFY(!profileFinding.value(QStringLiteral("message")).toString().isEmpty());

    const QJsonObject filteredReport = pdfplugin::preflight::filterAdvertisedFixups(report);
    QVERIFY(pdfplugin::preflight::validateNormalizedReport(filteredReport));
}

void OperatorAcceptanceTest::reportContract_rejectsUnsupportedSchema()
{
    QJsonObject report;
    report.insert(QStringLiteral("schema_version"), 99);
    report.insert(QStringLiteral("pass"), true);
    report.insert(QStringLiteral("profile"), QStringLiteral("Loupe Default"));
    report.insert(QStringLiteral("errors"), QJsonArray());
    report.insert(QStringLiteral("warnings"), QJsonArray());
    report.insert(QStringLiteral("fixups_available"), QJsonArray());

    QString validationError;
    QVERIFY(!pdfplugin::preflight::validateNormalizedReport(report, &validationError));
    QVERIFY(validationError.contains(QStringLiteral("schema_version")));

    report.insert(QStringLiteral("schema_version"), 2);
    QJsonObject documentFinding;
    documentFinding.insert(QStringLiteral("scope"), QStringLiteral("document"));
    documentFinding.insert(QStringLiteral("page"), 1);
    documentFinding.insert(QStringLiteral("type"), QStringLiteral("encrypted"));
    documentFinding.insert(QStringLiteral("severity"), QStringLiteral("error"));
    documentFinding.insert(QStringLiteral("message"), QStringLiteral("Document is password-protected"));
    documentFinding.insert(QStringLiteral("check_id"), QStringLiteral("document-access"));

    QJsonArray errors;
    errors.append(documentFinding);
    report.insert(QStringLiteral("errors"), errors);
    report.insert(QStringLiteral("pass"), false);

    validationError.clear();
    QVERIFY(!pdfplugin::preflight::validateNormalizedReport(report, &validationError));
    QVERIFY(validationError.contains(QStringLiteral("page")));
}

void OperatorAcceptanceTest::reportContract_allowedPropertiesMatchSchema()
{
    // Guards against producer/validator drift: PreflightResult::toJson() /
    // findingToJson() and the validator allow-lists in preflightsidecarutils.h
    // must agree with the shipped JSON schema's property lists, or a real engine
    // run can start emitting a key the validator silently rejects. This happened
    // twice: coverage_scope/profile_identity/variable_bindings/error at the report
    // level, and evidence_ids on findings emitted by evidence-graph-based checks.
    const QString schemaPath = QDir(operatoracceptance::sourceDir()).filePath(QStringLiteral("schemas/report.schema.json"));
    QFile schemaFile(schemaPath);
    QVERIFY2(schemaFile.open(QIODevice::ReadOnly), qPrintable(QStringLiteral("Missing report schema at %1").arg(schemaPath)));

    QJsonParseError parseError;
    const QJsonDocument schemaDoc = QJsonDocument::fromJson(schemaFile.readAll(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));

    const auto schemaPropertyKeys = [](const QJsonObject& schemaObject) -> QSet<QString>
    {
        const QJsonObject properties = schemaObject.value(QStringLiteral("properties")).toObject();
        QSet<QString> keys;
        for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
        {
            keys.insert(it.key());
        }
        return keys;
    };

    // Returns a diagnostic message describing any mismatch, or an empty string when
    // the schema and validator allow-lists agree. QVERIFY2 must run at the top level
    // of the test slot (not inside this helper) so a failure actually aborts the test.
    const auto mismatchMessage = [](const QString& label, const QSet<QString>& schemaKeys, const QSet<QString>& validatorKeys) -> QString
    {
        if (schemaKeys.isEmpty())
        {
            return QStringLiteral("%1: schema definition was empty or missing").arg(label);
        }
        const QSet<QString> missingFromValidator = schemaKeys - validatorKeys;
        const QSet<QString> extraInValidator = validatorKeys - schemaKeys;
        if (missingFromValidator.isEmpty() && extraInValidator.isEmpty())
        {
            return QString();
        }
        QStringList parts;
        if (!missingFromValidator.isEmpty())
        {
            parts << QStringLiteral("missing from allow-list: %1").arg(QStringList(missingFromValidator.values()).join(QStringLiteral(", ")));
        }
        if (!extraInValidator.isEmpty())
        {
            parts << QStringLiteral("extra in allow-list: %1").arg(QStringList(extraInValidator.values()).join(QStringLiteral(", ")));
        }
        return QStringLiteral("%1: %2").arg(label, parts.join(QStringLiteral("; ")));
    };

    const QJsonObject defs = schemaDoc.object().value(QStringLiteral("$defs")).toObject();

    const QString reportMismatch = mismatchMessage(QStringLiteral("validateNormalizedReport"),
                                                   schemaPropertyKeys(schemaDoc.object()),
                                                   pdfplugin::preflight::normalizedReportAllowedProperties());
    QVERIFY2(reportMismatch.isEmpty(), qPrintable(reportMismatch));

    const QString findingV1Mismatch = mismatchMessage(QStringLiteral("validateFindingV1"),
                                                      schemaPropertyKeys(defs.value(QStringLiteral("finding_v1")).toObject()),
                                                      pdfplugin::preflight::findingV1AllowedProperties());
    QVERIFY2(findingV1Mismatch.isEmpty(), qPrintable(findingV1Mismatch));

    const QString findingV2Mismatch = mismatchMessage(QStringLiteral("validateFindingV2"),
                                                      schemaPropertyKeys(defs.value(QStringLiteral("finding_v2")).toObject()),
                                                      pdfplugin::preflight::findingV2AllowedProperties());
    QVERIFY2(findingV2Mismatch.isEmpty(), qPrintable(findingV2Mismatch));
}

void OperatorAcceptanceTest::reportContract_classifiesVisualOverlays()
{
    const QString bleedPath = fixturePath(QStringLiteral("bleed-missing.pdf"));
    const QString fontPath = fixturePath(QStringLiteral("font-not-embedded.pdf"));
    QVERIFY(QFile::exists(bleedPath));
    QVERIFY(QFile::exists(fontPath));

    QJsonObject bleedReport;
    int bleedExitCode = -1;
    QVERIFY(runPreflight(bleedPath, m_defaultProfilePath, &bleedReport, &bleedExitCode));
    QCOMPARE(bleedExitCode, 1);

    const QJsonObject bleedFinding = findingWithCheckId(bleedReport, QStringLiteral("bleed"));
    QVERIFY(!bleedFinding.isEmpty());
    QVERIFY(pdfplugin::preflight::findingHasVisualOverlay(bleedFinding, 2));

    QJsonObject fontReport;
    int fontExitCode = -1;
    QVERIFY(runPreflight(fontPath, m_defaultProfilePath, &fontReport, &fontExitCode));
    QCOMPARE(fontExitCode, 1);

    const QJsonObject fontFinding = findingWithCheckId(fontReport, QStringLiteral("embedded-fonts"));
    QVERIFY(!fontFinding.isEmpty());
    QVERIFY(!pdfplugin::preflight::findingHasVisualOverlay(fontFinding, 2));
    QCOMPARE(fontFinding.value(QStringLiteral("scope")).toString(), QStringLiteral("object"));
}

void OperatorAcceptanceTest::sidecarCancellation_terminatesCleanly()
{
    const QString pdfPath = fixturePath(QStringLiteral("bleed-adequate.pdf"));
    QVERIFY(QFile::exists(pdfPath));

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    process.start(m_pdfToolPath,
                  { QStringLiteral("preflight"), pdfPath, QStringLiteral("--profile"), m_defaultProfilePath });
    QVERIFY(process.waitForStarted(10000));
    QVERIFY(process.waitForReadyRead(5000) || process.state() == QProcess::Running);

    process.kill();
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.state(), QProcess::NotRunning);
    QVERIFY(process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0);
}

void OperatorAcceptanceTest::corpus_performanceBaseline()
{
    QElapsedTimer timer;
    timer.start();
    qint64 peakChildMemoryKb = -1;

    int ranCases = 0;
    for (const OperatorCorpusEntry& entry : OPERATOR_CORPUS)
    {
        if (entry.skipPreflightJson)
        {
            continue;
        }

        const QString pdfPath = fixturePath(QString::fromLatin1(entry.pdf));
        QVERIFY2(QFile::exists(pdfPath),
                 qPrintable(QStringLiteral("Missing corpus fixture %1").arg(QString::fromLatin1(entry.pdf))));

        QJsonObject report;
        int exitCode = -1;
        qint64 casePeakMemoryKb = -1;
        QVERIFY(runPreflight(pdfPath, m_defaultProfilePath, &report, &exitCode, &casePeakMemoryKb));
        QCOMPARE(report.value(QStringLiteral("pass")).toBool(), entry.expectPass);
        QCOMPARE(exitCode, entry.expectPass ? 0 : 1);
        ++ranCases;

        if (casePeakMemoryKb > peakChildMemoryKb)
        {
            peakChildMemoryKb = casePeakMemoryKb;
        }
    }

    const qint64 elapsedMs = timer.elapsed();
    QCOMPARE(ranCases, 5);

#ifdef Q_OS_LINUX
    qInfo("MIC-300 baseline: %d corpus preflights in %lld ms; peak PdfTool VmHWM ~%lld KiB (Linux child process, informational only).",
          ranCases,
          static_cast<long long>(elapsedMs),
          static_cast<long long>(peakChildMemoryKb));
#else
    qInfo("MIC-300 baseline: %d corpus preflights in %lld ms; child peak memory not sampled on this platform (informational wall time only).",
          ranCases,
          static_cast<long long>(elapsedMs));
    Q_UNUSED(peakChildMemoryKb);
#endif
}

QTEST_APPLESS_MAIN(OperatorAcceptanceTest)

#include "tst_operatoracceptance.moc"
