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

#include "pdfplugin.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

class PluginAbiTest : public QObject
{
    Q_OBJECT

private slots:
    void missingAbiFailsClosed();
    void matchingAbiIsAccepted();
    void mismatchedAbiFailsClosed();
};

void PluginAbiTest::missingAbiFailsClosed()
{
    QJsonObject root{
        { QStringLiteral("MetaData"), QJsonObject{
            { QStringLiteral("Name"), QStringLiteral("test") },
            { QStringLiteral("Version"), QStringLiteral("1.0.0") }
        } }
    };
    const pdf::PDFPluginInfo info = pdf::PDFPluginInfo::loadFromJson(&root);
    QVERIFY(!info.isAbiCompatible());
}

void PluginAbiTest::matchingAbiIsAccepted()
{
    QJsonObject root{
        { QStringLiteral("MetaData"), QJsonObject{
            { QStringLiteral("Name"), QStringLiteral("test") },
            { QStringLiteral("AbiVersion"), pdf::PDFPluginInfo::CurrentAbiVersion },
            { QStringLiteral("Capabilities"), QJsonArray{ QStringLiteral("preflight") } }
        } }
    };
    const pdf::PDFPluginInfo info = pdf::PDFPluginInfo::loadFromJson(&root);
    QVERIFY(info.isAbiCompatible());
    QCOMPARE(info.abiVersion, pdf::PDFPluginInfo::CurrentAbiVersion);
    QVERIFY(!info.allowsNetwork);
    QVERIFY(!info.allowsExternalProcess);
}

void PluginAbiTest::mismatchedAbiFailsClosed()
{
    QJsonObject root{
        { QStringLiteral("MetaData"), QJsonObject{
            { QStringLiteral("Name"), QStringLiteral("test") },
            { QStringLiteral("AbiVersion"), pdf::PDFPluginInfo::CurrentAbiVersion + 1 }
        } }
    };
    const pdf::PDFPluginInfo info = pdf::PDFPluginInfo::loadFromJson(&root);
    QVERIFY(!info.isAbiCompatible());
}

QTEST_APPLESS_MAIN(PluginAbiTest)
#include "tst_pluginabitest.moc"
