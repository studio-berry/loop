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

#include "pdfrecoverymanager.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class RecoveryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sourceIdentityDetectsReplacement();
    void policyClampsUnsafeValues();
};

void RecoveryTest::sourceIdentityDetectsReplacement()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("source.pdf"));

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("first revision") > 0);
    file.close();

    const pdf::RecoverySourceIdentity original = pdf::inspectRecoverySource(path);
    QVERIFY(original.isValid());
    QVERIFY(!original.pathHash.contains(QStringLiteral("source.pdf")));
    QCOMPARE(pdf::classifyRecoverySource(original, original, true),
             pdf::RecoverySourceStatus::Unchanged);

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write("replacement revision with different bytes") > 0);
    file.close();
    const pdf::RecoverySourceIdentity replacement = pdf::inspectRecoverySource(path);
    QCOMPARE(pdf::classifyRecoverySource(original, replacement, true),
             pdf::RecoverySourceStatus::Changed);
    QCOMPARE(pdf::classifyRecoverySource(original, {}, false),
             pdf::RecoverySourceStatus::Missing);
}

void RecoveryTest::policyClampsUnsafeValues()
{
    pdf::RecoveryPolicy policy;
    policy.intervalSeconds = 0;
    policy.debounceSeconds = -1;
    policy.maxBytes = 0;
    policy.maxSessions = 0;
    policy.maxAgeDays = 0;
    const pdf::RecoveryPolicy clamped = pdf::clampRecoveryPolicy(policy);

    QVERIFY(clamped.intervalSeconds >= 1);
    QVERIFY(clamped.debounceSeconds >= 0);
    QVERIFY(clamped.maxBytes >= 1);
    QVERIFY(clamped.maxSessions >= 1);
    QVERIFY(clamped.maxAgeDays >= 1);
}

QTEST_APPLESS_MAIN(RecoveryTest)

#include "tst_recoverytest.moc"
