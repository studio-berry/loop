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

#include "loopstatevisual.h"
#include "preflightengine.h"

#include <QtTest>

using pdfquick::tokens::ColorRole;
using pdfquick::tokens::LoopStateVisual;
using pdfquick::tokens::resolveStateVisual;
using pdfquick::tokens::StateIcon;
using pdfquick::tokens::StateKind;

Q_DECLARE_METATYPE(StateKind)
Q_DECLARE_METATYPE(ColorRole)
Q_DECLARE_METATYPE(StateIcon)

namespace
{

// 64 hex characters -- the only shape PreflightDecision::resolveState()
// accepts as a digest. The two decisions below are otherwise identical; only
// which of these two digests they were recorded against differs.
QString documentDigestA()
{
    return QString(64, QLatin1Char('a'));
}

QString documentDigestB()
{
    return QString(64, QLatin1Char('b'));
}

QString profileDigest()
{
    return QString(64, QLatin1Char('c'));
}

pdf::PreflightFinding findingWithSeverity(const QString& severity)
{
    pdf::PreflightFinding finding;
    finding.scope = QStringLiteral("page");
    finding.page = 1;
    finding.type = QStringLiteral("color-mode");
    finding.severity = severity;
    finding.checkId = QStringLiteral("color-mode");
    finding.message = QStringLiteral("test finding");
    return finding;
}

pdf::PreflightCheckStatus statusWith(const QString& status)
{
    pdf::PreflightCheckStatus checkStatus;
    checkStatus.id = QStringLiteral("color-mode");
    checkStatus.status = status;
    return checkStatus;
}

pdf::PreflightDecision waiveDecision(const QString& documentDigest)
{
    pdf::PreflightDecision decision;
    decision.findingId = QStringLiteral("finding-1");
    decision.kind = pdf::PreflightDecisionKind::Waive;
    decision.justification = QStringLiteral("accepted for this release");
    decision.operatorIdentity = QStringLiteral("qa@example.com");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest();
    return decision;
}

pdf::PreflightDecision decisionOfKind(pdf::PreflightDecisionKind kind)
{
    pdf::PreflightDecision decision = waiveDecision(documentDigestA());
    decision.kind = kind;
    return decision;
}

}   // namespace

class LoopStateVisualTest : public QObject
{
    Q_OBJECT

private slots:
    void severityMapping_data();
    void severityMapping();

    void checkStatusMapping_data();
    void checkStatusMapping();

    void notChecked_whenNothingProvided();

    void activeWaive_overridesSeverity();
    void staleWaive_fallsThroughToSeverity();
    void nonWaiveDecision_doesNotWaive_data();
    void nonWaiveDecision_doesNotWaive();

    void incompleteNeverResolvesToPassed_data();
    void incompleteNeverResolvesToPassed();

    void waivedNeverResolvesToPassed();
};

void LoopStateVisualTest::severityMapping_data()
{
    QTest::addColumn<QString>("severity");
    QTest::addColumn<StateKind>("expectedKind");
    QTest::addColumn<ColorRole>("expectedRole");
    QTest::addColumn<StateIcon>("expectedIcon");

    QTest::newRow("error") << QStringLiteral("error") << StateKind::Error << ColorRole::SeverityError << StateIcon::FilledCircle;
    QTest::newRow("warning") << QStringLiteral("warning") << StateKind::Warning << ColorRole::SeverityWarning << StateIcon::FilledTriangle;
    QTest::newRow("info") << QStringLiteral("info") << StateKind::Info << ColorRole::SeverityInfo << StateIcon::FilledSquare;
    // profile.schema.json admits only error/warning/info; anything else is
    // unrecognised and must not be silently treated as a pass.
    QTest::newRow("unrecognised severity") << QStringLiteral("catastrophic") << StateKind::Incomplete << ColorRole::StateIncomplete << StateIcon::Hatched;
    QTest::newRow("empty severity") << QString() << StateKind::Incomplete << ColorRole::StateIncomplete << StateIcon::Hatched;
}

void LoopStateVisualTest::severityMapping()
{
    QFETCH(QString, severity);
    QFETCH(StateKind, expectedKind);
    QFETCH(ColorRole, expectedRole);
    QFETCH(StateIcon, expectedIcon);

    const pdf::PreflightFinding finding = findingWithSeverity(severity);
    const LoopStateVisual visual = resolveStateVisual(&finding, nullptr, nullptr);

    QCOMPARE(visual.kind, expectedKind);
    QCOMPARE(visual.colorRole, expectedRole);
    QCOMPARE(visual.icon, expectedIcon);
}

void LoopStateVisualTest::checkStatusMapping_data()
{
    QTest::addColumn<QString>("status");
    QTest::addColumn<StateKind>("expectedKind");

    QTest::newRow("ok") << QStringLiteral("ok") << StateKind::Passed;
    QTest::newRow("failed") << QStringLiteral("failed") << StateKind::Incomplete;
    QTest::newRow("warning status") << QStringLiteral("warning") << StateKind::Incomplete;
    QTest::newRow("skipped") << QStringLiteral("skipped") << StateKind::Incomplete;
    QTest::newRow("incomplete") << QStringLiteral("incomplete") << StateKind::Incomplete;
    QTest::newRow("unsupported") << QStringLiteral("unsupported") << StateKind::Incomplete;
}

void LoopStateVisualTest::checkStatusMapping()
{
    QFETCH(QString, status);
    QFETCH(StateKind, expectedKind);

    const pdf::PreflightCheckStatus checkStatus = statusWith(status);
    const LoopStateVisual visual = resolveStateVisual(nullptr, &checkStatus, nullptr);

    QCOMPARE(visual.kind, expectedKind);
    if (expectedKind == StateKind::Passed)
    {
        QCOMPARE(visual.colorRole, ColorRole::Success);
        QCOMPARE(visual.icon, StateIcon::Checkmark);
    }
    else
    {
        QCOMPARE(visual.colorRole, ColorRole::StateIncomplete);
        QCOMPARE(visual.icon, StateIcon::Hatched);
    }
}

void LoopStateVisualTest::notChecked_whenNothingProvided()
{
    const LoopStateVisual visual = resolveStateVisual(nullptr, nullptr, nullptr);
    QCOMPARE(visual.kind, StateKind::NotChecked);
    QCOMPARE(visual.colorRole, ColorRole::StateNotChecked);
    QCOMPARE(visual.icon, StateIcon::Outline);
}

void LoopStateVisualTest::activeWaive_overridesSeverity()
{
    const pdf::PreflightFinding finding = findingWithSeverity(QStringLiteral("error"));
    const pdf::PreflightDecision decision = waiveDecision(documentDigestA());

    const LoopStateVisual visual = resolveStateVisual(&finding, nullptr, &decision, documentDigestA(), profileDigest());

    QCOMPARE(visual.kind, StateKind::Waived);
    QCOMPARE(visual.colorRole, ColorRole::SeverityWarning);
    QCOMPARE(visual.icon, StateIcon::BadgeOverlay);
}

void LoopStateVisualTest::staleWaive_fallsThroughToSeverity()
{
    const pdf::PreflightFinding finding = findingWithSeverity(QStringLiteral("error"));
    const pdf::PreflightDecision decision = waiveDecision(documentDigestA());

    // Recorded against document A; the current document is B. resolveState()
    // reads this as StaleDocument, not Active, so the finding must fall
    // through to its plain severity treatment rather than staying masked as
    // waived.
    const LoopStateVisual visual = resolveStateVisual(&finding, nullptr, &decision, documentDigestB(), profileDigest());

    QCOMPARE(visual.kind, StateKind::Error);
    QCOMPARE(visual.colorRole, ColorRole::SeverityError);
}

void LoopStateVisualTest::nonWaiveDecision_doesNotWaive_data()
{
    QTest::addColumn<int>("kind");

    QTest::newRow("Accept") << static_cast<int>(pdf::PreflightDecisionKind::Accept);
    QTest::newRow("Override") << static_cast<int>(pdf::PreflightDecisionKind::Override);
    QTest::newRow("Reject") << static_cast<int>(pdf::PreflightDecisionKind::Reject);
    QTest::newRow("Reopen") << static_cast<int>(pdf::PreflightDecisionKind::Reopen);
}

void LoopStateVisualTest::nonWaiveDecision_doesNotWaive()
{
    QFETCH(int, kind);

    const pdf::PreflightFinding finding = findingWithSeverity(QStringLiteral("warning"));
    const pdf::PreflightDecision decision = decisionOfKind(static_cast<pdf::PreflightDecisionKind>(kind));

    const LoopStateVisual visual = resolveStateVisual(&finding, nullptr, &decision, documentDigestA(), profileDigest());

    // Only an active Waive resolves to Waived; every other decision kind
    // leaves the finding's own severity as the presentation.
    QCOMPARE(visual.kind, StateKind::Warning);
    QCOMPARE(visual.colorRole, ColorRole::SeverityWarning);
}

void LoopStateVisualTest::incompleteNeverResolvesToPassed_data()
{
    QTest::addColumn<QString>("status");

    QTest::newRow("failed") << QStringLiteral("failed");
    QTest::newRow("warning") << QStringLiteral("warning");
    QTest::newRow("skipped") << QStringLiteral("skipped");
    QTest::newRow("incomplete") << QStringLiteral("incomplete");
    QTest::newRow("unsupported") << QStringLiteral("unsupported");
    QTest::newRow("unrecognised") << QStringLiteral("not-a-real-status");
}

void LoopStateVisualTest::incompleteNeverResolvesToPassed()
{
    QFETCH(QString, status);

    const pdf::PreflightCheckStatus checkStatus = statusWith(status);
    const LoopStateVisual visual = resolveStateVisual(nullptr, &checkStatus, nullptr);

    QVERIFY(visual.kind != StateKind::Passed);
    QVERIFY(visual.colorRole != ColorRole::Success);
    QVERIFY(visual.icon != StateIcon::Checkmark);
}

void LoopStateVisualTest::waivedNeverResolvesToPassed()
{
    for (const QString& severity : { QStringLiteral("error"), QStringLiteral("warning"), QStringLiteral("info") })
    {
        const pdf::PreflightFinding finding = findingWithSeverity(severity);
        const pdf::PreflightDecision decision = waiveDecision(documentDigestA());

        const LoopStateVisual visual = resolveStateVisual(&finding, nullptr, &decision, documentDigestA(), profileDigest());

        QVERIFY(visual.kind != StateKind::Passed);
        QVERIFY(visual.colorRole != ColorRole::Success);
        QVERIFY(visual.icon != StateIcon::Checkmark);
        QCOMPARE(visual.kind, StateKind::Waived);
    }
}

QTEST_APPLESS_MAIN(LoopStateVisualTest)

#include "tst_loopstatevisualtest.moc"
