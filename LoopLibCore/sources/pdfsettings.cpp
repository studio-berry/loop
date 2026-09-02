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

#include "pdfsettings.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStringList>
#include <QVariant>

namespace pdf
{

namespace
{

constexpr int LegacySettingsMigrationVersion = 1;
const QString LegacySettingsMigrationKey = QStringLiteral("migration/legacyIdentityV1");

QStringList legacyApplicationNames(const QString& currentApplicationName)
{
    QStringList names;
    names << currentApplicationName << QString();

    if (currentApplicationName == QLatin1String("LoopEditor"))
    {
        names << QStringLiteral("Loop")
              << QStringLiteral("LOOP Editor")
              << QStringLiteral("LOUPE Editor")
              << QStringLiteral("Loupe Editor")
              << QStringLiteral("LOUPE")
              << QStringLiteral("Loupe");
    }
    else if (currentApplicationName == QLatin1String("JBIG2Viewer"))
    {
        names << QStringLiteral("JBIG2_image_viewer");
    }

    names << QStringLiteral("PDF4QT")
          << QStringLiteral("PDF4QT Editor")
          << QStringLiteral("PDF4QT Viewer")
          << QStringLiteral("PDF4QT PageMaster")
          << QStringLiteral("PDF4QT Diff")
          << QStringLiteral("PDF4QT LaunchPad");
    names.removeDuplicates();
    return names;
}

}   // namespace

QCommandLineOption PDFSettings::getConfigPathOption()
{
    return QCommandLineOption(QStringList({ "c", "config" }),
                              QCoreApplication::translate("CommandLine", "Use custom directory for user settings."),
                              QCoreApplication::translate("CommandLine", "path"));
}

void PDFSettings::applyCommandLineSettingsPath(const QCommandLineParser& parser)
{
    if (parser.isSet("config"))
    {
        setSettingsPath(parser.value("config"));
    }
}

void PDFSettings::migrateLegacySettings()
{
    const QString applicationName = QCoreApplication::applicationName();
    if (applicationName.isEmpty())
    {
        return;
    }

    QSettings target(QSettings::IniFormat,
                     QSettings::UserScope,
                     QCoreApplication::organizationName(),
                     applicationName);
    if (target.value(LegacySettingsMigrationKey).toInt() >= LegacySettingsMigrationVersion)
    {
        return;
    }

    for (const QString& legacyApplicationName : legacyApplicationNames(applicationName))
    {
        QSettings legacy(QSettings::IniFormat,
                         QSettings::UserScope,
                         QStringLiteral("MelkaJ"),
                         legacyApplicationName);
        const QStringList legacyKeys = legacy.allKeys();
        if (legacy.status() == QSettings::FormatError)
        {
            continue;
        }

        for (const QString& key : legacyKeys)
        {
            if (key.startsWith(QStringLiteral("migration/")) || target.contains(key))
            {
                continue;
            }

            target.setValue(key, legacy.value(key));
        }
    }

    target.setValue(LegacySettingsMigrationKey, LegacySettingsMigrationVersion);
    target.sync();
}

void PDFSettings::setSettingsPath(const QString& path)
{
    if (path.isEmpty())
    {
        return;
    }

    QDir settingsDirectory(path);
    const QString settingsPath = settingsDirectory.absolutePath();
    QDir().mkpath(settingsPath);

    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);

    if (QCoreApplication* application = QCoreApplication::instance())
    {
        application->setProperty("loopSettingsPath", settingsPath);
    }
}

QString PDFSettings::getSettingsPath()
{
    if (QCoreApplication* application = QCoreApplication::instance())
    {
        return application->property("loopSettingsPath").toString();
    }

    return QString();
}

}   // namespace
