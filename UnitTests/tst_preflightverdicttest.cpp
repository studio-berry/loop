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

#include "pdfpreflightverdict.h"

#include <QtTest>

class PreflightVerdictTest : public QObject
{
    Q_OBJECT

private slots:
    void emptyCompleteInspection_isPass();
    void blockingFinding_isFail();
    void budgetExceededWithoutFindings_isIncomplete();
    void activeWaiver_isPassAndRecorded();
    void engineError_isError();
    void unsupportedScopeErrorCode_isIncomplete();
    void unresolvedVariableErrorCode_isIncomplete();
    void cancelledErrorCode_isIncomplete();
    void budgetExceededErrorCode_isIncomplete();
    void reportPassIsDerivedFromVerdict();
    void incompleteInspectionWithoutFindings_isNotPass();
    void cancellationMarkedIncomplete_isNotPass();
    void requiredCheckMissingStatus_isIncomplete();
};

namespace
{

pdf::PreflightFinding blockingFinding()
{
    pdf::PreflightFinding finding;
    finding.scope = QStringLiteral("page");
    finding.page = 1;
    finding.type = QStringLiteral("color-mode");
    finding.severity = QStringLiteral("error");
    finding.checkId = QStringLiteral("color-mode");
    finding.message = QStringLiteral("RGB content is not allowed.");
    return finding;
}

} // namespace

void PreflightVerdictTest::emptyCompleteInspection_isPass()
{
    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(pdf::PreflightResult());
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Pass);
    QCOMPARE(verdict.reasonCode, QStringLiteral("no-blocking-findings"));
}

void PreflightVerdictTest::blockingFinding_isFail()
{
    pdf::PreflightResult result;
    result.errors.append(blockingFinding());

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Fail);
    QCOMPARE(verdict.blockingFindingIds.size(), 1);
    QVERIFY(verdict.waivedFindingIds.isEmpty());
}

void PreflightVerdictTest::budgetExceededWithoutFindings_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.checkStatuses.append({
        QStringLiteral("ink-coverage"),
        QStringLiteral("incomplete"),
        QStringLiteral("budget-exceeded"),
        QStringLiteral("raster-pixels"),
        100,
        101,
        QStringLiteral("page 1")
    });

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("budget-exceeded"));
}

void PreflightVerdictTest::activeWaiver_isPassAndRecorded()
{
    const QString documentDigest(64, QLatin1Char('a'));
    const QString profileDigest(64, QLatin1Char('b'));
    pdf::PreflightResult result;
    result.documentRevisionDigest = documentDigest;
    result.effectiveProfileDigest = profileDigest;
    result.errors.append(blockingFinding());

    pdf::PreflightDecision decision;
    decision.findingId = result.errors.first().stableId();
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("Approved by the client.");
    decision.operatorIdentity = QStringLiteral("operator");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest;
    result.decisions.append(decision);

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Pass);
    QCOMPARE(verdict.reasonCode, QStringLiteral("blocking-findings-waived"));
    QCOMPARE(verdict.waivedFindingIds, QStringList{decision.findingId});
}

void PreflightVerdictTest::engineError_isError()
{
    pdf::PreflightResult result;
    result.errorCode = QStringLiteral("profile-invalid");
    result.errorMessage = QStringLiteral("Profile is malformed.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Error);
    QCOMPARE(verdict.reasonCode, QStringLiteral("profile-invalid"));
    QCOMPARE(verdict.reason, result.errorMessage);
}

void PreflightVerdictTest::unsupportedScopeErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("unsupported-scope");
    result.errorMessage = QStringLiteral("Profile scope is empty or unsupported.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("unsupported-scope"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::unresolvedVariableErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("unresolved-variable");
    result.errorMessage = QStringLiteral("Profile variable 'stock' is unresolved.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("unresolved-variable"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::cancelledErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("cancelled");
    result.errorMessage = QStringLiteral("Preflight was cancelled.");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("cancelled"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::budgetExceededErrorCode_isIncomplete()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.errorCode = QStringLiteral("budget-exceeded");
    result.errorMessage = QStringLiteral("RasterTile");

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("budget-exceeded"));
    QCOMPARE(verdict.reason, QStringLiteral("RasterTile"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::reportPassIsDerivedFromVerdict()
{
    pdf::PreflightResult result;
    result.pass = true;
    result.inspectionComplete = false;
    const QJsonObject report = result.toJson();

    QCOMPARE(report.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString(),
             QStringLiteral("incomplete"));
    QVERIFY(!report.value(QStringLiteral("pass")).toBool());
}

void PreflightVerdictTest::incompleteInspectionWithoutFindings_isNotPass()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.pass = true;

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("inspection-incomplete"));
    QVERIFY(!verdict.isPass());
    QVERIFY(!result.toJson().value(QStringLiteral("pass")).toBool());
}

void PreflightVerdictTest::cancellationMarkedIncomplete_isNotPass()
{
    pdf::PreflightResult result;
    result.inspectionComplete = false;
    result.checkStatuses.append({
        QStringLiteral("image-resolution"),
        QStringLiteral("incomplete"),
        QStringLiteral("cancelled"),
        QString(),
        0,
        0,
        QStringLiteral("operator cancel")
    });

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("cancelled"));
    QVERIFY(!verdict.isPass());
}

void PreflightVerdictTest::requiredCheckMissingStatus_isIncomplete()
{
    pdf::PreflightProfileData profile;
    pdf::PreflightCheckConfig check;
    check.id = QStringLiteral("image-resolution");
    check.required = true;
    check.enabled = true;
    profile.checks.append(check);

    pdf::PreflightResult result;
    result.inspectionComplete = true;

    const pdf::PreflightVerdict verdict = pdf::reducePreflightVerdict(result, &profile);
    QCOMPARE(verdict.state, pdf::PreflightVerdictState::Incomplete);
    QCOMPARE(verdict.reasonCode, QStringLiteral("required-check-not-run"));
    QVERIFY(!verdict.isPass());
}

QTEST_APPLESS_MAIN(PreflightVerdictTest)

#include "tst_preflightverdicttest.moc"
