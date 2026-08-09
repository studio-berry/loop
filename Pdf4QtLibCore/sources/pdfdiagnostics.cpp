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

#include "pdfdiagnostics.h"

#include "pdflogger.h"
#include "pdflogscrubber.h"
#include "pdfsentry.h"
#include "pdfsettings.h"
#include "pdfutils.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QSettings>
#include <QSysInfo>
#include <QTemporaryFile>

#include <vector>

namespace pdf
{

namespace
{

struct WrittenFileInfo
{
    QString name;
    qint64 bytes = 0;
    QString sha256;
};

/// Turns an application name like "PDF4QT Editor" into "pdf4qt-editor" for
/// use in the bundle directory name.
QString sanitizeForDirectoryName(const QString& text)
{
    QString result;
    result.reserve(text.size());

    for (const QChar ch : text)
    {
        if (ch.isLetterOrNumber())
        {
            result += ch.toLower();
        }
        else if (!result.isEmpty() && !result.endsWith(QLatin1Char('-')))
        {
            result += QLatin1Char('-');
        }
    }

    while (result.endsWith(QLatin1Char('-')))
    {
        result.chop(1);
    }

    return result.isEmpty() ? QStringLiteral("app") : result;
}

QJsonObject buildSystemInfo()
{
    QJsonObject root;
    root[QStringLiteral("applicationName")] = QCoreApplication::applicationName();
    root[QStringLiteral("applicationVersion")] = QCoreApplication::applicationVersion();
    root[QStringLiteral("qtVersionCompileTime")] = QStringLiteral(QT_VERSION_STR);
    root[QStringLiteral("qtVersionRuntime")] = QString::fromUtf8(qVersion());
    root[QStringLiteral("osProductType")] = QSysInfo::productType();
    root[QStringLiteral("osProductVersion")] = QSysInfo::productVersion();
    root[QStringLiteral("kernelType")] = QSysInfo::kernelType();
    root[QStringLiteral("kernelVersion")] = QSysInfo::kernelVersion();
    root[QStringLiteral("cpuArchitecture")] = QSysInfo::currentCpuArchitecture();
    root[QStringLiteral("buildAbi")] = QSysInfo::buildAbi();
    root[QStringLiteral("locale")] = QLocale::system().name();
    root[QStringLiteral("logDirectory")] = PDFLogScrubber::scrub(PDFLogSession::logDirectory());
    root[QStringLiteral("settingsPath")] = PDFLogScrubber::scrub(PDFSettings::getSettingsPath());
    root[QStringLiteral("sentryActive")] = PDFSentrySession::isGloballyActive();

    QJsonArray dependencies;
    for (const PDFDependentLibraryInfo& info : PDFDependentLibraryInfo::getLibraryInfo())
    {
        QJsonObject dependency;
        dependency[QStringLiteral("library")] = info.library;
        dependency[QStringLiteral("version")] = info.version;
        dependency[QStringLiteral("license")] = info.license;
        dependency[QStringLiteral("url")] = info.url;
        dependencies.append(dependency);
    }
    root[QStringLiteral("dependencies")] = dependencies;

    return root;
}

QJsonObject buildPlugins(const PDFPluginInfos& plugins)
{
    QJsonArray array;
    for (const PDFPluginInfo& plugin : plugins)
    {
        QJsonObject entry;
        entry[QStringLiteral("name")] = plugin.name;
        entry[QStringLiteral("author")] = plugin.author;
        entry[QStringLiteral("version")] = plugin.version;
        entry[QStringLiteral("license")] = plugin.license;
        entry[QStringLiteral("file")] = plugin.pluginFile;
        array.append(entry);
    }

    QJsonObject root;
    root[QStringLiteral("plugins")] = array;
    return root;
}

/// Copies the user's QSettings INI into a fresh, in-memory INI with the
/// path/name-bearing keys removed. The recent-files list and default
/// directory reveal which documents the user has opened and where they keep
/// them; the custom author name is, deliberately, a real name.
QByteArray buildFilteredSettingsIni()
{
    static const QSet<QString> denylistKeys = {
        QStringLiteral("RecentFiles/RecentFileList"),
        QStringLiteral("ViewerSettings/defaultDirectory"),
        QStringLiteral("ViewerSettings/customAuthorName"),
    };

    const QSettings source(QSettings::IniFormat, QSettings::UserScope, QCoreApplication::organizationName(), QCoreApplication::applicationName());

    QTemporaryFile temporaryIni;
    if (!temporaryIni.open())
    {
        return QByteArray();
    }
    const QString temporaryPath = temporaryIni.fileName();
    temporaryIni.close();

    {
        QSettings filtered(temporaryPath, QSettings::IniFormat);
        for (const QString& key : source.allKeys())
        {
            if (!denylistKeys.contains(key))
            {
                filtered.setValue(key, source.value(key));
            }
        }
        filtered.sync();
    }

    QFile filteredFile(temporaryPath);
    QByteArray content;
    if (filteredFile.open(QIODevice::ReadOnly))
    {
        content = filteredFile.readAll();
    }
    QFile::remove(temporaryPath);

    return content;
}

QByteArray buildReadme()
{
    QString text;
    text += QStringLiteral("Loupe diagnostics bundle\n");
    text += QStringLiteral("=========================\n\n");
    text += QStringLiteral("This bundle was generated on request to help investigate a problem.\n\n");
    text += QStringLiteral("Included:\n");
    text += QStringLiteral("  - manifest.json      file list with sizes and SHA-256 hashes\n");
    text += QStringLiteral("  - system-info.json   app/Qt/OS versions, dependency versions, locale\n");
    text += QStringLiteral("  - plugins.json       loaded editor plugins (when applicable)\n");
    text += QStringLiteral("  - logs/*.log         rotated application log files\n");
    text += QStringLiteral("  - settings.ini       application settings, with the recent-files list,\n");
    text += QStringLiteral("                       default open directory, and custom author name removed\n\n");
    text += QStringLiteral("Not included:\n");
    text += QStringLiteral("  - Any PDF or document content\n");
    text += QStringLiteral("  - The recent-files list\n");
    text += QStringLiteral("  - Crash minidumps: these are a separate, opt-in mechanism (SENTRY_DSN) with\n");
    text += QStringLiteral("    different privacy properties - a minidump can contain PDF content and file\n");
    text += QStringLiteral("    paths, and nothing in the Sentry SDK can scrub that. See SECURITY.md and\n");
    text += QStringLiteral("    R-008 in docs/V1_RELEASE_READINESS.md.\n\n");
    text += QStringLiteral("Log lines and settings.ini paths are scrubbed of the home/temp directory,\n");
    text += QStringLiteral("login name, host name, other absolute paths, email addresses, and IPv4\n");
    text += QStringLiteral("literals before they are written. Absolute paths keep only their file\n");
    text += QStringLiteral("extension - the file name itself is dropped.\n");

    return text.toUtf8();
}

}   // namespace

PDFDiagnosticsResult PDFDiagnosticsCollector::collect(const PDFDiagnosticsOptions& options)
{
    PDFDiagnosticsResult result;

    if (options.outputDirectory.isEmpty())
    {
        result.errorMessage = tr("Output directory is not set.");
        return result;
    }

    const QString applicationSlug = sanitizeForDirectoryName(QCoreApplication::applicationName());
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString bundleDirectory = QDir(options.outputDirectory).filePath(QStringLiteral("loupe-diagnostics-%1-%2").arg(applicationSlug, timestamp));

    if (!QDir().mkpath(bundleDirectory))
    {
        result.errorMessage = tr("Could not create diagnostics directory '%1'.").arg(bundleDirectory);
        return result;
    }

    std::vector<WrittenFileInfo> writtenFiles;

    auto writeFile = [&](const QString& relativeName, const QByteArray& content) -> bool
    {
        const QString fullPath = QDir(bundleDirectory).filePath(relativeName);
        QDir().mkpath(QFileInfo(fullPath).absolutePath());

        QSaveFile file(fullPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        if (file.write(content) != content.size())
        {
            file.cancelWriting();
            return false;
        }
        if (!file.commit())
        {
            return false;
        }

        WrittenFileInfo info;
        info.name = relativeName;
        info.bytes = content.size();
        info.sha256 = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
        writtenFiles.push_back(std::move(info));
        return true;
    };

    bool ok = writeFile(QStringLiteral("system-info.json"), QJsonDocument(buildSystemInfo()).toJson(QJsonDocument::Indented));

    if (ok && !options.plugins.empty())
    {
        ok = writeFile(QStringLiteral("plugins.json"), QJsonDocument(buildPlugins(options.plugins)).toJson(QJsonDocument::Indented));
    }

    if (ok && options.includeLogs)
    {
        for (const QString& logFilePath : PDFLogSession::logFiles())
        {
            QFile logFile(logFilePath);
            if (!logFile.open(QIODevice::ReadOnly))
            {
                continue;
            }

            const QString scrubbedContent = PDFLogScrubber::scrub(QString::fromUtf8(logFile.readAll()));
            ok = writeFile(QStringLiteral("logs/%1").arg(QFileInfo(logFilePath).fileName()), scrubbedContent.toUtf8());
            if (!ok)
            {
                break;
            }
        }
    }

    if (ok && options.includeSettings)
    {
        ok = writeFile(QStringLiteral("settings.ini"), buildFilteredSettingsIni());
    }

    if (ok)
    {
        ok = writeFile(QStringLiteral("README.txt"), buildReadme());
    }

    if (ok)
    {
        QJsonArray filesArray;
        for (const WrittenFileInfo& info : writtenFiles)
        {
            QJsonObject entry;
            entry[QStringLiteral("name")] = info.name;
            entry[QStringLiteral("bytes")] = info.bytes;
            entry[QStringLiteral("sha256")] = info.sha256;
            filesArray.append(entry);
        }

        QJsonObject manifest;
        manifest[QStringLiteral("schemaVersion")] = 1;
        manifest[QStringLiteral("generatorApplication")] = QCoreApplication::applicationName();
        manifest[QStringLiteral("generatorVersion")] = QCoreApplication::applicationVersion();
        manifest[QStringLiteral("generatedAtUtc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        manifest[QStringLiteral("files")] = filesArray;

        ok = writeFile(QStringLiteral("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    }

    if (!ok)
    {
        QDir(bundleDirectory).removeRecursively();
        result.success = false;
        result.errorMessage = tr("Failed to write the diagnostics bundle to '%1'.").arg(bundleDirectory);
        return result;
    }

    result.success = true;
    result.bundleDirectory = bundleDirectory;
    for (const WrittenFileInfo& info : writtenFiles)
    {
        result.files.append(info.name);
    }

    return result;
}

}   // namespace pdf
