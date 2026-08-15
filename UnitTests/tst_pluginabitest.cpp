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

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class PluginAbiTest : public QObject
{
    Q_OBJECT

private slots:
    void acceptedManifestPasses();
    void unsupportedAbiFailsClosed();
    void malformedIdentityFailsClosed();
    void duplicateIdFailsClosed();
    void unknownCapabilityFailsClosed();
    void missingReadDocumentFailsClosed();
    void pathOutsidePackagedDirFailsClosed();
    void networkRequiresDeclaration();
};

namespace
{

QJsonObject qtPluginJson(const QJsonObject& metadata, const QString& iid = QStringLiteral("PDF4QT.TestPlugin"))
{
    return QJsonObject{
        { QStringLiteral("IID"), iid },
        { QStringLiteral("MetaData"), metadata }
    };
}

QJsonObject validMetadata()
{
    return QJsonObject{
        { QStringLiteral("PluginId"), QStringLiteral("PDF4QT.TestPlugin") },
        { QStringLiteral("AbiVersion"), 1 },
        { QStringLiteral("Name"), QStringLiteral("Test Plugin") },
        { QStringLiteral("Author"), QStringLiteral("Loupe") },
        { QStringLiteral("Version"), QStringLiteral("1.0.0") },
        { QStringLiteral("License"), QStringLiteral("MIT") },
        { QStringLiteral("Description"), QStringLiteral("ABI unit test plugin") },
        { QStringLiteral("Capabilities"), QJsonArray{ QStringLiteral("read-document"), QStringLiteral("propose-operation") } },
        { QStringLiteral("BuildId"), QStringLiteral("loupe-0.1.0") }
    };
}

} // namespace

void PluginAbiTest::acceptedManifestPasses()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString pluginPath = directory.filePath(QStringLiteral("test.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    const pdf::PDFPluginTrustDecision decision = pdf::inspectPluginManifest(qtPluginJson(validMetadata()),
                                                                            pluginPath,
                                                                            directory.path(),
                                                                            {});
    QVERIFY(decision.accepted);
    QCOMPARE(decision.info.abiVersion, quint32(pdf::PDF_PLUGIN_ABI_VERSION));
    QCOMPARE(decision.info.pluginId, QStringLiteral("PDF4QT.TestPlugin"));
    QVERIFY(decision.info.capabilities.contains(QStringLiteral("read-document")));
}

void PluginAbiTest::unsupportedAbiFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString pluginPath = directory.filePath(QStringLiteral("test.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    QJsonObject metadata = validMetadata();
    metadata.insert(QStringLiteral("AbiVersion"), 99);
    const pdf::PDFPluginTrustDecision decision = pdf::inspectPluginManifest(qtPluginJson(metadata),
                                                                            pluginPath,
                                                                            directory.path(),
                                                                            {});
    QVERIFY(!decision.accepted);
    QCOMPARE(decision.errorCode, QStringLiteral("unsupported-plugin-abi"));
}

void PluginAbiTest::malformedIdentityFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString pluginPath = directory.filePath(QStringLiteral("test.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    QJsonObject metadata = validMetadata();
    metadata.remove(QStringLiteral("PluginId"));
    metadata.remove(QStringLiteral("Name"));
    const pdf::PDFPluginTrustDecision decision = pdf::inspectPluginManifest(qtPluginJson(metadata, QString()),
                                                                            pluginPath,
                                                                            directory.path(),
                                                                            {});
    QVERIFY(!decision.accepted);
    QCOMPARE(decision.errorCode, QStringLiteral("malformed-plugin-identity"));
}

void PluginAbiTest::duplicateIdFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString pluginPath = directory.filePath(QStringLiteral("test.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    const QSet<QString> seen{ QStringLiteral("PDF4QT.TestPlugin") };
    const pdf::PDFPluginTrustDecision decision = pdf::inspectPluginManifest(qtPluginJson(validMetadata()),
                                                                            pluginPath,
                                                                            directory.path(),
                                                                            seen);
    QVERIFY(!decision.accepted);
    QCOMPARE(decision.errorCode, QStringLiteral("duplicate-plugin-id"));
}

void PluginAbiTest::unknownCapabilityFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString pluginPath = directory.filePath(QStringLiteral("test.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    QJsonObject metadata = validMetadata();
    metadata.insert(QStringLiteral("Capabilities"), QJsonArray{ QStringLiteral("read-document"), QStringLiteral("pwn") });
    const pdf::PDFPluginTrustDecision decision = pdf::inspectPluginManifest(qtPluginJson(metadata),
                                                                            pluginPath,
                                                                            directory.path(),
                                                                            {});
    QVERIFY(!decision.accepted);
    QCOMPARE(decision.errorCode, QStringLiteral("unknown-plugin-capability"));
}

void PluginAbiTest::missingReadDocumentFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString pluginPath = directory.filePath(QStringLiteral("test.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    QJsonObject metadata = validMetadata();
    metadata.insert(QStringLiteral("Capabilities"), QJsonArray{ QStringLiteral("network") });
    const pdf::PDFPluginTrustDecision decision = pdf::inspectPluginManifest(qtPluginJson(metadata),
                                                                            pluginPath,
                                                                            directory.path(),
                                                                            {});
    QVERIFY(!decision.accepted);
    QCOMPARE(decision.errorCode, QStringLiteral("missing-read-document-capability"));
}

void PluginAbiTest::pathOutsidePackagedDirFailsClosed()
{
    QTemporaryDir allowed;
    QTemporaryDir other;
    QVERIFY(allowed.isValid());
    QVERIFY(other.isValid());
    const QString pluginPath = other.filePath(QStringLiteral("evil.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    const pdf::PDFPluginTrustDecision decision = pdf::inspectPluginManifest(qtPluginJson(validMetadata()),
                                                                            pluginPath,
                                                                            allowed.path(),
                                                                            {});
    QVERIFY(!decision.accepted);
    QCOMPARE(decision.errorCode, QStringLiteral("plugin-path-outside-packaged-dir"));
}

void PluginAbiTest::networkRequiresDeclaration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString pluginPath = directory.filePath(QStringLiteral("test.so"));
    QVERIFY(QFile(pluginPath).open(QIODevice::WriteOnly));

    const pdf::PDFPluginTrustDecision withoutNetwork = pdf::inspectPluginManifest(qtPluginJson(validMetadata()),
                                                                                  pluginPath,
                                                                                  directory.path(),
                                                                                  {});
    QVERIFY(withoutNetwork.accepted);
    QVERIFY(!withoutNetwork.info.capabilities.contains(QStringLiteral("network")));
    QVERIFY(!withoutNetwork.info.capabilities.contains(QStringLiteral("external-process")));

    QJsonObject metadata = validMetadata();
    metadata.insert(QStringLiteral("Capabilities"),
                    QJsonArray{ QStringLiteral("read-document"), QStringLiteral("network"), QStringLiteral("external-process") });
    const pdf::PDFPluginTrustDecision withNetwork = pdf::inspectPluginManifest(qtPluginJson(metadata),
                                                                               pluginPath,
                                                                               directory.path(),
                                                                               {});
    QVERIFY(withNetwork.accepted);
    QVERIFY(withNetwork.info.capabilities.contains(QStringLiteral("network")));
    QVERIFY(withNetwork.info.capabilities.contains(QStringLiteral("external-process")));
}

QTEST_GUILESS_MAIN(PluginAbiTest)
#include "tst_pluginabitest.moc"
