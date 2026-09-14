#include "preflightprofilecatalog.h"

#include "preflightengine.h"
#include "preflightprofileresolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>

#include <algorithm>

namespace pdfinteraction
{

PreflightProfileCatalog::PreflightProfileCatalog(QObject* parent) :
    QObject(parent),
    m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&)
            { reload(); });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&)
            { reload(); });
    reload();
}

PreflightProfileCatalog::~PreflightProfileCatalog() = default;

const PreflightProfileChoice* PreflightProfileCatalog::selectedProfile() const
{
    return profileById(m_selectedProfileId);
}

const PreflightProfileChoice* PreflightProfileCatalog::profileById(const QString& id) const
{
    const auto it = std::find_if(m_profiles.cbegin(), m_profiles.cend(),
                                 [&id](const PreflightProfileChoice& profile)
                                 { return profile.id == id; });
    return it == m_profiles.cend() ? nullptr : &(*it);
}

bool PreflightProfileCatalog::selectProfile(const QString& id)
{
    const PreflightProfileChoice* profile = profileById(id);
    if (profile == nullptr || !profile->valid || id == m_selectedProfileId)
    {
        return false;
    }
    m_selectedProfileId = id;
    m_bindings = QJsonObject();
    Q_EMIT profileStaleRequested();
    Q_EMIT changed();
    return true;
}

bool PreflightProfileCatalog::setVariable(const QString& name, const QVariant& value)
{
    const PreflightProfileChoice* profile = selectedProfile();
    if (profile == nullptr || !profile->variables.contains(name))
    {
        return false;
    }
    m_bindings.insert(name, QJsonValue::fromVariant(value));
    Q_EMIT profileStaleRequested();
    Q_EMIT changed();
    return true;
}

void PreflightProfileCatalog::reload()
{
    const QString priorId = m_selectedProfileId;
    QString priorDigest;
    QJsonArray priorChecks;
    for (const PreflightProfileChoice& profile : std::as_const(m_profiles))
    {
        if (profile.id == priorId)
        {
            priorDigest = profile.digest;
            priorChecks = profile.profile.value(QStringLiteral("checks")).toArray();
            break;
        }
    }

    QList<PreflightProfileChoice> profiles;
    const auto addProfile = [&profiles](const QString& source, const QByteArray& data)
    {
        PreflightProfileChoice choice;
        choice.id = source;
        choice.source = source;
        QJsonParseError parseError;
        const QJsonDocument parsed = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        {
            choice.name = QFileInfo(source).completeBaseName();
            choice.diagnostic = QStringLiteral("Profile JSON is invalid.");
            profiles.append(choice);
            return;
        }
        const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(parsed.object(), source);
        choice.name = imported.profile.value(QStringLiteral("name")).toString(QFileInfo(source).completeBaseName());
        choice.version = imported.identity.version;
        choice.digest = imported.identity.digest;
        choice.profile = imported.profile;
        choice.variables = imported.profile.value(QStringLiteral("variables")).toObject();
        choice.valid = imported.ok;
        choice.diagnostic = imported.ok ? QString() : imported.errorMessage;
        if (choice.valid)
        {
            const pdf::PreflightVariableBindResult bound = pdf::bindPreflightProfileVariables(choice.profile);
            if (bound.ok)
            {
                pdf::PreflightProfileData parsedProfile;
                QString profileParseError;
                if (!pdf::PreflightEngine::parseProfile(bound.profile, parsedProfile, profileParseError))
                {
                    choice.valid = false;
                    choice.diagnostic = profileParseError;
                }
            }
            else if (bound.errorCode != QStringLiteral("unresolved-variable"))
            {
                choice.valid = false;
                choice.diagnostic = bound.errorMessage;
            }
            else
            {
                choice.diagnostic = bound.errorMessage;
            }
        }
        profiles.append(choice);
    };

    const QDir bundled(QStringLiteral(":/profiles"));
    for (const QFileInfo& file : bundled.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name))
    {
        QFile input(file.filePath());
        if (input.open(QIODevice::ReadOnly))
        {
            addProfile(file.filePath(), input.readAll());
        }
    }

    const QString localDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                       .filePath(QStringLiteral("profiles"));
    const QDir local(localDirectory);
    for (const QFileInfo& file : local.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name))
    {
        QFile input(file.absoluteFilePath());
        if (input.open(QIODevice::ReadOnly))
        {
            addProfile(file.absoluteFilePath(), input.readAll());
        }
    }

    m_profiles = std::move(profiles);
    if (m_selectedProfileId.isEmpty() ||
        std::none_of(m_profiles.cbegin(), m_profiles.cend(),
                     [this](const PreflightProfileChoice& profile)
                     { return profile.id == m_selectedProfileId && profile.valid; }))
    {
        const auto valid = std::find_if(m_profiles.cbegin(), m_profiles.cend(),
                                        [](const PreflightProfileChoice& profile)
                                        { return profile.valid; });
        m_selectedProfileId = valid == m_profiles.cend() ? QString() : valid->id;
        m_bindings = QJsonObject();
    }
    const auto current = std::find_if(m_profiles.cbegin(), m_profiles.cend(),
                                      [this](const PreflightProfileChoice& profile)
                                      { return profile.id == m_selectedProfileId; });
    if (!priorId.isEmpty() && (priorId != m_selectedProfileId || current == m_profiles.cend()))
    {
        Q_EMIT profileStaleRequested();
    }
    else if (!priorId.isEmpty() && current != m_profiles.cend() && current->digest != priorDigest)
    {
        if (priorChecks != current->profile.value(QStringLiteral("checks")).toArray())
        {
            Q_EMIT checkSetStaleRequested();
        }
        else
        {
            Q_EMIT profileStaleRequested();
        }
    }
    updateWatch();
    Q_EMIT changed();
}

void PreflightProfileCatalog::updateWatch()
{
    if (!m_watcher)
    {
        return;
    }
    m_watcher->removePaths(m_watcher->directories());
    m_watcher->removePaths(m_watcher->files());
    const QString localDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                                       .filePath(QStringLiteral("profiles"));
    if (QFileInfo::exists(localDirectory))
    {
        m_watcher->addPath(localDirectory);
    }
    for (const PreflightProfileChoice& profile : std::as_const(m_profiles))
    {
        if (!profile.source.startsWith(QLatin1Char(':')) && QFileInfo::exists(profile.source))
        {
            m_watcher->addPath(profile.source);
        }
    }
}

}   // namespace pdfinteraction
