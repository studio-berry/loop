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

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

namespace
{

struct ToolRun
{
    int exitCode = -1;
    QByteArray stdoutData;
    QByteArray stderrData;
    QJsonObject json;
};

ToolRun runPdfTool(const QStringList& arguments)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    process.setProgram(QStringLiteral(PDFTOOL_EXECUTABLE_PATH));
    process.setArguments(arguments);
    process.start();

    ToolRun run;
    if (!process.waitForFinished(120000))
    {
        run.stderrData = process.errorString().toUtf8();
        return run;
    }

    run.exitCode = process.exitCode();
    run.stdoutData = process.readAllStandardOutput();
    run.stderrData = process.readAllStandardError();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(run.stdoutData, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject())
    {
        run.json = document.object();
    }

    return run;
}

void verifyEnvelope(const ToolRun& run, int expectedExitCode, const QString& command)
{
    QCOMPARE(run.exitCode, expectedExitCode);
    QVERIFY2(!run.json.isEmpty(), qPrintable(QStringLiteral("stdout was not one JSON object: %1").arg(QString::fromUtf8(run.stdoutData))));
    QCOMPARE(run.json.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(run.json.value(QStringLiteral("command")).toString(), command);
    QCOMPARE(run.json.value(QStringLiteral("exit_code")).toInt(), expectedExitCode);
    QVERIFY(!run.json.value(QStringLiteral("diagnostics")).isNull());
    QVERIFY(!run.json.value(QStringLiteral("outputs")).isNull());
    QVERIFY(run.json.value(QStringLiteral("data")).isObject());
    QVERIFY2(run.stderrData.isEmpty(), qPrintable(QStringLiteral("JSON mode wrote stderr: %1").arg(QString::fromUtf8(run.stderrData))));
}

class PdfToolContractTest : public QObject
{
    Q_OBJECT

private slots:
    void helpIsWrapped();
    void equalsFormIsDetected();
    void unknownCommandIsInvalidInvocation();
    void malformedInvocationIsWrapped();
    void preflightKeepsNestedReportBoundary();
};

void PdfToolContractTest::helpIsWrapped()
{
    const ToolRun run = runPdfTool({QStringLiteral("help"), QStringLiteral("--console-format"), QStringLiteral("json")});
    verifyEnvelope(run, 0, QStringLiteral("help"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
}

void PdfToolContractTest::equalsFormIsDetected()
{
    const ToolRun run = runPdfTool({QStringLiteral("help"), QStringLiteral("--console-format=json")});
    verifyEnvelope(run, 0, QStringLiteral("help"));
}

void PdfToolContractTest::unknownCommandIsInvalidInvocation()
{
    const ToolRun run = runPdfTool({QStringLiteral("frobnicate"), QStringLiteral("--console-format"), QStringLiteral("json")});
    verifyEnvelope(run, 2, QStringLiteral("frobnicate"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("invalid-invocation"));
    const QJsonArray diagnostics = run.json.value(QStringLiteral("diagnostics")).toArray();
    QVERIFY(!diagnostics.isEmpty());
    QCOMPARE(diagnostics.front().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("cli.unknown-command"));
}

void PdfToolContractTest::malformedInvocationIsWrapped()
{
    const ToolRun run = runPdfTool({QStringLiteral("help"), QStringLiteral("--console-format"), QStringLiteral("json"), QStringLiteral("--not-an-option")});
    verifyEnvelope(run, 2, QStringLiteral("help"));
    QCOMPARE(run.json.value(QStringLiteral("status")).toString(), QStringLiteral("invalid-invocation"));
}

void PdfToolContractTest::preflightKeepsNestedReportBoundary()
{
    const ToolRun run = runPdfTool({QStringLiteral("preflight"), QStringLiteral("--console-format"), QStringLiteral("json")});
    verifyEnvelope(run, 3, QStringLiteral("preflight"));
    QVERIFY(run.json.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).isUndefined());
}

} // namespace

QTEST_MAIN(PdfToolContractTest)
#include "tst_pdftoolcontract.moc"
