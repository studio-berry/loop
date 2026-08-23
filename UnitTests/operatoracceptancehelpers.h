// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#ifndef OPERATORACCEPTANCEHELPERS_H
#define OPERATORACCEPTANCEHELPERS_H

#include "pdfworkloadenvelope.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

namespace operatoracceptance
{

constexpr char DEFAULT_PROFILE_REL[] = "profiles/loupe-default.json";

inline QString fixturesDir()
{
    return QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures");
}

inline QString sourceDir()
{
    return QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR);
}

inline QString defaultProfilePath()
{
    return QDir(sourceDir()).filePath(QString::fromLatin1(DEFAULT_PROFILE_REL));
}

inline QString fixturePath(const QString& pdf)
{
    return QDir(fixturesDir()).filePath(pdf);
}

inline QByteArray fileSha256(const QString& path)
{
    return QByteArray::fromHex(pdf::PDFRunIdentity::digestFile(path).toLatin1());
}

#ifdef Q_OS_LINUX
inline qint64 readProcessMemoryFieldKb(qint64 processId, const char* fieldName)
{
    QFile statusFile(QStringLiteral("/proc/%1/status").arg(processId));
    if (!statusFile.open(QIODevice::ReadOnly))
    {
        return -1;
    }

    const QList<QByteArray> lines = statusFile.readAll().split('\n');
    const QByteArray prefix = QByteArray(fieldName) + ':';
    for (const QByteArray& line : lines)
    {
        if (line.startsWith(prefix))
        {
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() >= 2)
            {
                return parts.at(1).toLongLong();
            }
        }
    }

    return -1;
}
#endif

inline bool runPdfTool(const QString& pdfToolPath,
                       const QStringList& arguments,
                       QByteArray* stdOut,
                       QByteArray* stdErr,
                       int* exitCode,
                       qint64* peakChildMemoryKb = nullptr)
{
    QProcess process;
    QTemporaryDir captureDirectory;
    if (!captureDirectory.isValid())
    {
        return false;
    }

    const QProcessEnvironment systemEnvironment = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment environment;
    for (const QString& name : { QStringLiteral("PATH"), QStringLiteral("SystemRoot"),
                                 QStringLiteral("TEMP"), QStringLiteral("TMP"),
                                 QStringLiteral("USERPROFILE"), QStringLiteral("LANG"),
                                 QStringLiteral("LC_ALL"), QStringLiteral("LC_CTYPE") })
    {
        if (systemEnvironment.contains(name))
        {
            environment.insert(name, systemEnvironment.value(name));
        }
    }
    if (!environment.contains(QStringLiteral("LANG")) && !environment.contains(QStringLiteral("LC_ALL")))
    {
        environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    }
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
                       QDir(QFileInfo(pdfToolPath).absolutePath()).filePath(QStringLiteral("platforms")));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(QFileInfo(pdfToolPath).absolutePath());
    process.setStandardOutputFile(captureDirectory.filePath(QStringLiteral("stdout.txt")));
    process.setStandardErrorFile(captureDirectory.filePath(QStringLiteral("stderr.txt")));
    process.start(QDir::toNativeSeparators(pdfToolPath), arguments);
    if (!process.waitForStarted(10000))
    {
        return false;
    }

    qint64 peakMemoryKb = -1;
    QElapsedTimer runTimer;
    runTimer.start();
    while (!process.waitForFinished(250))
    {
        if (runTimer.elapsed() > 120000)
        {
            process.kill();
            process.waitForFinished(5000);
            return false;
        }

#ifdef Q_OS_LINUX
        const qint64 sample = readProcessMemoryFieldKb(process.processId(), "VmHWM");
        if (sample > peakMemoryKb)
        {
            peakMemoryKb = sample;
        }
#endif
    }

#ifdef Q_OS_LINUX
    const qint64 finalSample = readProcessMemoryFieldKb(process.processId(), "VmHWM");
    if (finalSample > peakMemoryKb)
    {
        peakMemoryKb = finalSample;
    }
#endif

    if (exitCode)
    {
        *exitCode = process.exitCode();
    }

    auto readCapture = [](const QString& path) -> QByteArray
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return {};
        }
        return file.readAll();
    };
    const QByteArray capturedStdOut = readCapture(captureDirectory.filePath(QStringLiteral("stdout.txt")));
    const QByteArray capturedStdErr = readCapture(captureDirectory.filePath(QStringLiteral("stderr.txt")));

    if (stdOut)
    {
        *stdOut = capturedStdOut;
    }

    if (stdErr)
    {
        *stdErr = capturedStdErr;
    }

    if (peakChildMemoryKb)
    {
        *peakChildMemoryKb = peakMemoryKb;
    }

    return process.exitStatus() == QProcess::NormalExit;
}

}   // namespace operatoracceptance

#endif   // OPERATORACCEPTANCEHELPERS_H
