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
#include "pdfdbgheap.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace pdf
{

namespace
{

QJsonObject pluginIdentityObject(const QJsonObject& json)
{
    if (json.contains(QLatin1String("MetaData")))
    {
        return json.value(QLatin1String("MetaData")).toObject();
    }

    return json;
}

}   // namespace

QString pdfPluginCapabilityToString(PDFPluginCapability capability)
{
    switch (capability)
    {
        case PDFPluginCapability::ReadDocument:
            return QStringLiteral("read-document");
        case PDFPluginCapability::ProposeOperation:
            return QStringLiteral("propose-operation");
        case PDFPluginCapability::ExecuteRegisteredOperation:
            return QStringLiteral("execute-operation");
        case PDFPluginCapability::ExternalProcess:
            return QStringLiteral("external-process");
        case PDFPluginCapability::Network:
            return QStringLiteral("network");
    }

    return QString();
}

bool pdfPluginCapabilityFromString(const QString& value, PDFPluginCapability& capability)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("read-document"))
    {
        capability = PDFPluginCapability::ReadDocument;
        return true;
    }
    if (normalized == QLatin1String("propose-operation"))
    {
        capability = PDFPluginCapability::ProposeOperation;
        return true;
    }
    if (normalized == QLatin1String("execute-operation"))
    {
        capability = PDFPluginCapability::ExecuteRegisteredOperation;
        return true;
    }
    if (normalized == QLatin1String("external-process"))
    {
        capability = PDFPluginCapability::ExternalProcess;
        return true;
    }
    if (normalized == QLatin1String("network"))
    {
        capability = PDFPluginCapability::Network;
        return true;
    }

    return false;
}


PDFPlugin::PDFPlugin(QObject* parent) :
    QObject(parent),
    m_dataExchangeInterface(nullptr),
    m_widget(nullptr),
    m_cmsManager(nullptr),
    m_document(nullptr)
{
}

void PDFPlugin::setDataExchangeInterface(IPluginDataExchange* dataExchangeInterface)
{
    m_dataExchangeInterface = dataExchangeInterface;
}

void PDFPlugin::setWidget(PDFWidget* widget)
{
    m_widget = widget;
}

void PDFPlugin::setCMSManager(PDFCMSManager* manager)
{
    m_cmsManager = manager;
}

void PDFPlugin::setDocument(const PDFModifiedDocument& document)
{
    m_document = document;
}

std::vector<QAction*> PDFPlugin::getActions() const
{
    return std::vector<QAction*>();
}

PDFPluginInfo PDFPluginInfo::loadFromJson(const QJsonObject* json)
{
    PDFPluginInfo result;
    if (!json)
    {
        return result;
    }

    const QJsonObject metadata = pluginIdentityObject(*json);
    result.pluginId = metadata.value(QLatin1String("PluginId")).toString();
    if (result.pluginId.isEmpty())
    {
        result.pluginId = json->value(QLatin1String("IID")).toString();
    }
    result.abiVersion = static_cast<quint32>(metadata.value(QLatin1String("AbiVersion")).toInt(0));
    result.name = metadata.value(QLatin1String("Name")).toString();
    result.author = metadata.value(QLatin1String("Author")).toString();
    result.version = metadata.value(QLatin1String("Version")).toString();
    result.license = metadata.value(QLatin1String("License")).toString();
    result.description = metadata.value(QLatin1String("Description")).toString();
    result.buildId = metadata.value(QLatin1String("BuildId")).toString();

    const QJsonArray capabilities = metadata.value(QLatin1String("Capabilities")).toArray();
    for (const QJsonValue& value : capabilities)
    {
        result.capabilities.append(value.toString().trimmed().toLower());
    }

    return result;
}

bool pluginPathIsInsideAllowedDirectory(const QString& pluginPath, const QString& allowedDirectory)
{
    if (pluginPath.isEmpty() || allowedDirectory.isEmpty())
    {
        return false;
    }

    const QFileInfo directoryInfo(allowedDirectory);
    QString allowed = directoryInfo.exists() ? directoryInfo.canonicalFilePath()
                                             : QDir::cleanPath(directoryInfo.absoluteFilePath());
    if (allowed.isEmpty())
    {
        return false;
    }

    const QFileInfo pluginInfo(pluginPath);
    QString plugin = pluginInfo.exists() ? pluginInfo.canonicalFilePath()
                                         : QDir::cleanPath(pluginInfo.absoluteFilePath());
    if (plugin.isEmpty())
    {
        return false;
    }

    const QString prefix = allowed.endsWith(QLatin1Char('/')) ? allowed : allowed + QLatin1Char('/');
    return plugin == allowed || plugin.startsWith(prefix);
}

PDFPluginTrustDecision inspectPluginManifest(const QJsonObject& qtMetaData,
                                             const QString& pluginPath,
                                             const QString& allowedDirectory,
                                             const QSet<QString>& seenPluginIds,
                                             quint32 expectedAbi)
{
    PDFPluginTrustDecision decision;
    if (!pluginPathIsInsideAllowedDirectory(pluginPath, allowedDirectory))
    {
        decision.errorCode = QStringLiteral("plugin-path-outside-packaged-dir");
        decision.errorMessage = QStringLiteral("Plugin '%1' is outside the packaged plugin directory.").arg(pluginPath);
        return decision;
    }

    const QJsonObject identity = pluginIdentityObject(qtMetaData);
    if (identity.isEmpty())
    {
        decision.errorCode = QStringLiteral("malformed-plugin-metadata");
        decision.errorMessage = QStringLiteral("Plugin '%1' has empty metadata.").arg(pluginPath);
        return decision;
    }

    decision.info = PDFPluginInfo::loadFromJson(&qtMetaData);
    if (decision.info.pluginId.isEmpty() || decision.info.name.isEmpty())
    {
        decision.errorCode = QStringLiteral("malformed-plugin-identity");
        decision.errorMessage = QStringLiteral("Plugin '%1' is missing PluginId or Name.").arg(pluginPath);
        return decision;
    }

    if (decision.info.abiVersion != expectedAbi)
    {
        decision.errorCode = QStringLiteral("unsupported-plugin-abi");
        decision.errorMessage = QStringLiteral("Plugin '%1' declares ABI %2; expected %3.")
                                    .arg(decision.info.pluginId)
                                    .arg(decision.info.abiVersion)
                                    .arg(expectedAbi);
        return decision;
    }

    if (seenPluginIds.contains(decision.info.pluginId))
    {
        decision.errorCode = QStringLiteral("duplicate-plugin-id");
        decision.errorMessage = QStringLiteral("Duplicate plugin id '%1'.").arg(decision.info.pluginId);
        return decision;
    }

    bool hasReadDocument = false;
    for (const QString& token : decision.info.capabilities)
    {
        PDFPluginCapability capability = PDFPluginCapability::ReadDocument;
        if (!pdfPluginCapabilityFromString(token, capability))
        {
            decision.errorCode = QStringLiteral("unknown-plugin-capability");
            decision.errorMessage = QStringLiteral("Plugin '%1' declares unknown capability '%2'.")
                                        .arg(decision.info.pluginId, token);
            return decision;
        }
        if (capability == PDFPluginCapability::ReadDocument)
        {
            hasReadDocument = true;
        }
    }

    if (!hasReadDocument)
    {
        decision.errorCode = QStringLiteral("missing-read-document-capability");
        decision.errorMessage = QStringLiteral("Plugin '%1' must declare read-document.").arg(decision.info.pluginId);
        return decision;
    }

    decision.accepted = true;
    return decision;
}

}   // namespace pdf
