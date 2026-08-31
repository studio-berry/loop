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

#include "pdflogger.h"

#include "pdflogscrubber.h"
#include "pdfsettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>

#include <atomic>
#include <cstdio>

namespace pdf
{

namespace
{

constexpr qint64 LOG_ROTATION_SIZE_BYTES = 2 * 1024 * 1024;

struct LogState
{
    QString applicationId;
    QString directory;
    QFile file;
    QMutex mutex;
    bool healthy = true;
};

std::atomic<LogState*> g_state{ nullptr };
std::atomic<int> g_level{ static_cast<int>(PDFLogSession::Warning) };
QtMessageHandler g_previousHandler = nullptr;

bool tryParseLevel(const QString& text, PDFLogSession::Level& level)
{
    const QString normalized = text.trimmed().toLower();

    if (normalized == QStringLiteral("off"))
    {
        level = PDFLogSession::Off;
        return true;
    }
    if (normalized == QStringLiteral("error"))
    {
        level = PDFLogSession::Error;
        return true;
    }
    if (normalized == QStringLiteral("warning") || normalized == QStringLiteral("warn"))
    {
        level = PDFLogSession::Warning;
        return true;
    }
    if (normalized == QStringLiteral("info"))
    {
        level = PDFLogSession::Info;
        return true;
    }
    if (normalized == QStringLiteral("debug"))
    {
        level = PDFLogSession::Debug;
        return true;
    }

    bool ok = false;
    const int numeric = text.trimmed().toInt(&ok);
    if (ok && numeric >= PDFLogSession::Off && numeric <= PDFLogSession::Debug)
    {
        level = static_cast<PDFLogSession::Level>(numeric);
        return true;
    }

    return false;
}

PDFLogSession::Level resolveInitialLevel()
{
    const QByteArray envLevel = qgetenv("LOUPE_LOG_LEVEL");
    PDFLogSession::Level level = PDFLogSession::Warning;
    if (!envLevel.isEmpty() && tryParseLevel(QString::fromLocal8Bit(envLevel), level))
    {
        return level;
    }

    // Core reads the diagnostics/logLevel key itself (rather than going through
    // PDFViewerSettings, which lives in Gui) so PdfTool gets the same setting
    // without depending on the Gui module.
    const QSettings settings(QSettings::IniFormat, QSettings::UserScope, QCoreApplication::organizationName(), QCoreApplication::applicationName());
    const QVariant storedValue = settings.value(QStringLiteral("diagnostics/logLevel"));
    if (storedValue.isValid() && tryParseLevel(storedValue.toString(), level))
    {
        return level;
    }

    return PDFLogSession::Warning;
}

QString resolveLogDirectory()
{
    const QByteArray envDirectory = qgetenv("LOUPE_LOG_DIR");
    if (!envDirectory.isEmpty())
    {
        return QString::fromLocal8Bit(envDirectory);
    }

    const QString settingsPath = PDFSettings::getSettingsPath();
    if (!settingsPath.isEmpty())
    {
        return QDir(settingsPath).filePath(QStringLiteral("logs"));
    }

    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (basePath.isEmpty())
    {
        basePath = QDir::tempPath();
    }

    return QDir(basePath).filePath(QStringLiteral("logs"));
}

QChar levelLetter(PDFLogSession::Level level)
{
    switch (level)
    {
        case PDFLogSession::Error:
            return QLatin1Char('E');
        case PDFLogSession::Warning:
            return QLatin1Char('W');
        case PDFLogSession::Info:
            return QLatin1Char('I');
        case PDFLogSession::Debug:
            return QLatin1Char('D');
        case PDFLogSession::Off:
            break;
    }

    return QLatin1Char('?');
}

PDFLogSession::Level levelForMessageType(QtMsgType type)
{
    switch (type)
    {
        case QtDebugMsg:
            return PDFLogSession::Debug;
        case QtInfoMsg:
            return PDFLogSession::Info;
        case QtWarningMsg:
            return PDFLogSession::Warning;
        case QtCriticalMsg:
        case QtFatalMsg:
            return PDFLogSession::Error;
    }

    return PDFLogSession::Error;
}

QString rotatedLogFilePath(const QString& baseLogFilePath, int index)
{
    return index == 0 ? baseLogFilePath : baseLogFilePath + QStringLiteral(".%1").arg(index);
}

/// Shifts ".log.1" -> ".log.2" and ".log" -> ".log.1", pruning anything
/// beyond ".log.2" so the rotation footprint never grows unbounded even if a
/// stray file from an older build is present.
bool rotateLogFiles(LogState& state)
{
    state.file.close();

    const QString baseLogFilePath = QDir(state.directory).filePath(state.applicationId + QStringLiteral(".log"));

    if (QFileInfo::exists(rotatedLogFilePath(baseLogFilePath, 3)))
    {
        if (!QFile::remove(rotatedLogFilePath(baseLogFilePath, 3)))
        {
            state.file.setFileName(baseLogFilePath);
            static_cast<void>(state.file.open(QIODevice::Append | QIODevice::WriteOnly));
            return false;
        }
    }
    if (QFileInfo::exists(rotatedLogFilePath(baseLogFilePath, 2))
        && !QFile::remove(rotatedLogFilePath(baseLogFilePath, 2)))
    {
        state.file.setFileName(baseLogFilePath);
        static_cast<void>(state.file.open(QIODevice::Append | QIODevice::WriteOnly));
        return false;
    }
    if (QFileInfo::exists(rotatedLogFilePath(baseLogFilePath, 1))
        && !QFile::rename(rotatedLogFilePath(baseLogFilePath, 1), rotatedLogFilePath(baseLogFilePath, 2)))
    {
        state.file.setFileName(baseLogFilePath);
        static_cast<void>(state.file.open(QIODevice::Append | QIODevice::WriteOnly));
        return false;
    }
    if (QFileInfo::exists(baseLogFilePath)
        && !QFile::rename(baseLogFilePath, rotatedLogFilePath(baseLogFilePath, 1)))
    {
        state.file.setFileName(baseLogFilePath);
        static_cast<void>(state.file.open(QIODevice::Append | QIODevice::WriteOnly));
        return false;
    }

    state.file.setFileName(baseLogFilePath);
    return state.file.open(QIODevice::Append | QIODevice::WriteOnly);
}

void writeLogLine(LogState& state, PDFLogSession::Level level, const QString& category, const QString& scrubbedMessage)
{
    QMutexLocker locker(&state.mutex);

    if (!state.file.isOpen())
    {
        return;
    }

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString line = QStringLiteral("%1  %2  [%3]  %4  %5\n")
                             .arg(timestamp, levelLetter(level), state.applicationId, category, scrubbedMessage);
    QByteArray encoded = line.toUtf8();
    if (encoded.size() > LOG_ROTATION_SIZE_BYTES)
    {
        // A hostile or accidental single message must not create an
        // unbounded individual write. Keep the line framing and cap it at the
        // same bounded size as the rotating file.
        encoded.truncate(int(LOG_ROTATION_SIZE_BYTES - 1));
        encoded.append('\n');
    }

    if (state.file.size() + encoded.size() > LOG_ROTATION_SIZE_BYTES)
    {
        if (!rotateLogFiles(state))
        {
            state.healthy = false;
        }
    }

    if (state.file.write(encoded) != encoded.size() || !state.file.flush())
    {
        state.healthy = false;
    }
}

/// Minimal replacement for Qt's built-in handler, used when no other handler
/// was previously installed - which, repo-wide, is every call site today.
void defaultMessageOutput(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    Q_UNUSED(context);
    const QByteArray localMessage = message.toLocal8Bit();
    std::fprintf(stderr, "%s\n", localMessage.constData());

    if (type == QtFatalMsg)
    {
        std::fflush(stderr);
    }
}

void loupeMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    LogState* state = g_state.load(std::memory_order_acquire);
    const int threshold = g_level.load(std::memory_order_relaxed);
    const PDFLogSession::Level messageLevel = levelForMessageType(type);

    if (state && threshold != PDFLogSession::Off && static_cast<int>(messageLevel) <= threshold)
    {
        const QString category = context.category ? QString::fromUtf8(context.category) : QStringLiteral("default");
        writeLogLine(*state, messageLevel, category, PDFLogScrubber::scrub(message));
    }

    if (g_previousHandler)
    {
        g_previousHandler(type, context, message);
    }
    else
    {
        defaultMessageOutput(type, context, message);
    }
}

}   // namespace

PDFLogSession::PDFLogSession(const QString& applicationId)
{
    LogState* state = new LogState();
    state->applicationId = applicationId;
    state->directory = resolveLogDirectory();

    if (!QDir().mkpath(state->directory))
    {
        state->healthy = false;
    }

    const QString logFilePath = QDir(state->directory).filePath(applicationId + QStringLiteral(".log"));
    state->file.setFileName(logFilePath);
    if (!state->file.open(QIODevice::Append | QIODevice::WriteOnly))
    {
        state->healthy = false;
    }

    g_level.store(static_cast<int>(resolveInitialLevel()), std::memory_order_relaxed);

    g_state.store(state, std::memory_order_release);
    g_previousHandler = qInstallMessageHandler(loupeMessageHandler);

    m_active = true;
}

PDFLogSession::~PDFLogSession()
{
    if (!m_active)
    {
        return;
    }

    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;

    LogState* state = g_state.exchange(nullptr, std::memory_order_acq_rel);
    delete state;

    m_active = false;
}

void PDFLogSession::setLevel(Level level)
{
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

PDFLogSession::Level PDFLogSession::level()
{
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

bool PDFLogSession::isHealthy()
{
    LogState* state = g_state.load(std::memory_order_acquire);
    if (!state)
    {
        return false;
    }

    QMutexLocker locker(&state->mutex);
    return state->healthy;
}

QString PDFLogSession::logDirectory()
{
    LogState* state = g_state.load(std::memory_order_acquire);
    return state ? state->directory : QString();
}

QStringList PDFLogSession::logFiles()
{
    LogState* state = g_state.load(std::memory_order_acquire);
    if (!state)
    {
        return {};
    }

    const QString baseLogFilePath = QDir(state->directory).filePath(state->applicationId + QStringLiteral(".log"));

    QStringList result;
    for (int index = 0; index <= 2; ++index)
    {
        const QString candidate = rotatedLogFilePath(baseLogFilePath, index);
        if (QFileInfo::exists(candidate))
        {
            result.append(candidate);
        }
    }

    return result;
}

}   // namespace pdf
