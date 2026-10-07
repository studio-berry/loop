// MIT License
// Copyright (c) 2026 Loop contributors

#include "operatorhelp.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUrl>

#include <utility>

OperatorHelp::OperatorHelp(QSettings& settings, QString sampleDirectory, QObject* parent) :
    QObject(parent),
    m_settings(settings),
    m_sampleDirectory(std::move(sampleDirectory))
{
    m_catalog = readResource(QStringLiteral(":/operator/check-catalog.json"));
    m_guide = readResource(QStringLiteral(":/operator/guide.json"));
    m_samples = readResource(QStringLiteral(":/operator/samples/manifest.json"));
}

QJsonObject OperatorHelp::readResource(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        m_diagnostic = tr("Operator help could not be loaded: %1").arg(path);
        return {};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        m_diagnostic = tr("Operator help is not valid: %1").arg(path);
        return {};
    }
    return document.object();
}

QVariantList OperatorHelp::sampleJobs() const
{
    return m_samples.value(QStringLiteral("jobs")).toArray().toVariantList();
}

bool OperatorHelp::welcomeNeeded() const
{
    return !m_settings.value(QStringLiteral("OperatorHelp/welcomeSeen"), false).toBool();
}

bool OperatorHelp::acknowledgeWelcome()
{
    m_settings.setValue(QStringLiteral("OperatorHelp/welcomeSeen"), true);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
    {
        m_settings.remove(QStringLiteral("OperatorHelp/welcomeSeen"));
        return false;
    }
    Q_EMIT welcomeChanged();
    return true;
}

QString OperatorHelp::workspaceHelp(int workspace) const
{
    const QJsonArray workspaces = m_guide.value(QStringLiteral("workspaces")).toArray();
    if (workspace < 0 || workspace >= workspaces.size())
    {
        return tr("Help is unavailable for this workspace.");
    }
    return workspaces.at(workspace).toObject().value(QStringLiteral("help")).toString();
}

QVariantMap OperatorHelp::findingHelp(const QString& checkId, const QString& findingType) const
{
    const QJsonObject check = m_catalog.value(QStringLiteral("checks")).toObject().value(checkId).toObject();
    if (check.isEmpty())
    {
        return { { QStringLiteral("limitations"), tr("This check has no catalog entry. Read the report's coverage and limitations; help cannot establish coverage.") } };
    }
    QVariantMap help = check.toVariantMap();
    help.insert(QStringLiteral("checkId"), checkId);
    help.insert(QStringLiteral("findingType"), findingType);
    for (const QJsonValue& entry : check.value(QStringLiteral("severity")).toArray())
    {
        const QJsonObject finding = entry.toObject();
        if (finding.value(QStringLiteral("finding_type")).toString() == findingType)
        {
            help.insert(QStringLiteral("condition"), finding.value(QStringLiteral("condition")).toString());
            break;
        }
    }
    return help;
}

QVariantMap OperatorHelp::prepareSample(const QString& id) const
{
    const auto fail = [](const QString& reason) -> QVariantMap
    {
        return { { QStringLiteral("ok"), false }, { QStringLiteral("error"), reason } };
    };
    if (!m_diagnostic.isEmpty() || m_sampleDirectory.isEmpty())
    {
        return fail(tr("The sample corpus is unavailable."));
    }
    QJsonObject selected;
    for (const QJsonValue& entry : m_samples.value(QStringLiteral("jobs")).toArray())
    {
        if (entry.toObject().value(QStringLiteral("id")).toString() == id)
        {
            selected = entry.toObject();
            break;
        }
    }
    if (selected.isEmpty())
    {
        return fail(tr("Unknown sample job."));
    }
    const QString directory = QDir(m_sampleDirectory).filePath(id);
    if (!QDir().mkpath(directory))
    {
        return fail(tr("Could not create the sample job directory."));
    }
    QVariantMap result{ { QStringLiteral("ok"), true } };
    const QJsonObject members = selected.value(QStringLiteral("members")).toObject();
    for (auto it = members.constBegin(); it != members.constEnd(); ++it)
    {
        const QString name = it.value().toString();
        QFile source(QStringLiteral(":/operator/samples/") + name);
        if (!source.open(QIODevice::ReadOnly))
        {
            return fail(tr("Could not read a bundled sample file."));
        }
        const QByteArray bytes = source.readAll();
        const QString path = QDir(directory).filePath(name);
        QFile existing(path);
        if (existing.exists())
        {
            if (!existing.open(QIODevice::ReadOnly) || existing.readAll() != bytes)
            {
                return fail(tr("The local sample has changed. Keep your work and move the sample directory before opening a fresh copy: %1").arg(directory));
            }
        }
        else
        {
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
            {
                return fail(tr("Could not write the sample job copy."));
            }
        }
        result.insert(it.key(), QUrl::fromLocalFile(path));
    }
    return result;
}
