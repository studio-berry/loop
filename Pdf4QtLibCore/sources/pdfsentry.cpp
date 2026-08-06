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

#include "pdfsentry.h"

#include "pdfconstants.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

#include <cstdlib>

#include "config.h"

#if PDF4QT_ENABLE_SENTRY
#include <sentry.h>
#endif

namespace pdf
{

bool PDFFSentry::s_initialized = false;

void PDFFSentry::initialize()
{
#if PDF4QT_ENABLE_SENTRY
    if (s_initialized)
    {
        return;
    }

    const QByteArray environmentDsn = qgetenv("SENTRY_DSN");
    const QByteArray configuredDsn = QByteArray(PDF4QT_SENTRY_DSN);
    const QByteArray dsn = !environmentDsn.isEmpty() ? environmentDsn : configuredDsn;

    if (dsn.isEmpty())
    {
        return;
    }

    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, dsn.constData());

    const QString databaseDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sentry-native");
    QDir().mkpath(databaseDirectory);
    sentry_options_set_database_path(options, databaseDirectory.toUtf8().constData());

    const QByteArray release = QByteArray("pdf4qt@") + QByteArray(PDF_LIBRARY_VERSION);
    sentry_options_set_release(options, release.constData());

#ifndef NDEBUG
    sentry_options_set_debug(options, 1);
#endif

    if (sentry_init(options) == 0)
    {
        s_initialized = true;

        const QString applicationName = QCoreApplication::applicationName();
        if (!applicationName.isEmpty())
        {
            sentry_set_tag("application", applicationName.toUtf8().constData());
        }

        if (qEnvironmentVariableIsSet("SENTRY_VERIFY"))
        {
            captureMessage("It works!");
        }
    }
#else
    Q_UNUSED(s_initialized);
#endif
}

void PDFFSentry::shutdown()
{
#if PDF4QT_ENABLE_SENTRY
    if (!s_initialized)
    {
        return;
    }

    sentry_close();
    s_initialized = false;
#endif
}

bool PDFFSentry::isEnabled()
{
#if PDF4QT_ENABLE_SENTRY
    return s_initialized;
#else
    return false;
#endif
}

void PDFFSentry::captureMessage(const char* message)
{
#if PDF4QT_ENABLE_SENTRY
    if (!s_initialized)
    {
        return;
    }

    sentry_capture_event(sentry_value_new_message_event(
        SENTRY_LEVEL_INFO,
        "custom",
        message));
#else
    Q_UNUSED(message);
#endif
}

}   // namespace pdf
