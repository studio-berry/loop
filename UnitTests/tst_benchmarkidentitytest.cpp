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

#include "pdfschemaversion.h"

#include <QJsonObject>
#include <QSysInfo>
#include <QtGlobal>
#include <QtTest>

class BenchmarkIdentityTest : public QObject
{
    Q_OBJECT

private slots:
    void recordWithoutIdentityIsRejected();
    void completeIdentityIsAccepted();
};

static bool hasBenchmarkIdentity(const QJsonObject& record)
{
    const QStringList required = {
        QStringLiteral("commit"),
        QStringLiteral("compiler"),
        QStringLiteral("os"),
        QStringLiteral("qt"),
        QStringLiteral("cpu"),
        QStringLiteral("renderer"),
        QStringLiteral("fixture_digest"),
        QStringLiteral("profile_or_operation_version")
    };
    for (const QString& key : required)
    {
        if (record.value(key).toString().trimmed().isEmpty())
        {
            return false;
        }
    }
    return true;
}

void BenchmarkIdentityTest::recordWithoutIdentityIsRejected()
{
    QVERIFY(!hasBenchmarkIdentity(QJsonObject{ { QStringLiteral("ms"), 12 } }));
}

void BenchmarkIdentityTest::completeIdentityIsAccepted()
{
    QJsonObject record{
        { QStringLiteral("commit"), QStringLiteral("deadbeef") },
        { QStringLiteral("compiler"), QStringLiteral("g++") },
        { QStringLiteral("os"), QSysInfo::prettyProductName() },
        { QStringLiteral("qt"), QString::fromLatin1(qVersion()) },
        { QStringLiteral("cpu"), QSysInfo::currentCpuArchitecture() },
        { QStringLiteral("renderer"), QStringLiteral("QPainter") },
        { QStringLiteral("fixture_digest"), QString(64, QLatin1Char('a')) },
        { QStringLiteral("profile_or_operation_version"), QStringLiteral("preflight-report/3.0") }
    };
    QVERIFY(hasBenchmarkIdentity(record));
}

QTEST_APPLESS_MAIN(BenchmarkIdentityTest)
#include "tst_benchmarkidentitytest.moc"
