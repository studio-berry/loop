// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors

#ifndef OPERATORACCEPTANCEHELPERS_H
#define OPERATORACCEPTANCEHELPERS_H

#include "pdfworkloadenvelope.h"

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>

#include <functional>
#include <utility>

namespace operatoracceptance
{

/// A privacy-safe, symbolic operator trace.  The action names deliberately do
/// not contain screen coordinates or document content, so the same trace can
/// be replayed by a Qt host, a CLI adapter, or a future product smoke runner.
struct OperatorLoopTraceStep
{
    QString action;
    QString outcome;
    QString detail;
};

class OperatorLoopTrace final
{
public:
    explicit OperatorLoopTrace(QString scenario) :
        m_scenario(std::move(scenario))
    {
    }

    ~OperatorLoopTrace()
    {
        if (!m_complete && !m_failureArtifactWritten)
        {
            const QString failure = m_failure.isEmpty()
                                        ? (m_steps.isEmpty()
                                               ? QStringLiteral("test aborted")
                                               : QStringLiteral("test aborted after action: %1").arg(m_steps.back().action))
                                        : m_failure;
            writeFailureArtifact(failure);
        }
    }

    OperatorLoopTrace(const OperatorLoopTrace&) = delete;
    OperatorLoopTrace& operator=(const OperatorLoopTrace&) = delete;

    void note(const QString& action, const QString& detail = {})
    {
        m_steps.push_back({ action, QStringLiteral("observed"), detail.left(4096) });
    }

    bool expect(bool condition, const QString& action, const QString& failure)
    {
        m_steps.push_back({ action, condition ? QStringLiteral("passed") : QStringLiteral("failed"),
                            condition ? QString() : failure.left(4096) });
        if (!condition && m_failure.isEmpty())
        {
            m_failure = failure;
            m_failureArtifactWritten = true;
            writeFailureArtifact(m_failure);
        }
        return condition;
    }

    void complete() noexcept { m_complete = true; }

    const QString& scenario() const noexcept { return m_scenario; }
    const QString& failure() const noexcept { return m_failure; }
    const QList<OperatorLoopTraceStep>& steps() const noexcept { return m_steps; }

    /// Executes a symbolic trace step.  Test callers can supply a lambda for
    /// whichever surface owns the operation, keeping the action sequence
    /// reusable without teaching this helper product semantics.
    bool replay(const QString& action, const std::function<bool()>& operation)
    {
        const bool passed = operation && operation();
        return expect(passed, action, QStringLiteral("operator trace step failed: %1").arg(action));
    }

private:
    static QString artifactDirectory()
    {
        const QString configured = qEnvironmentVariable("LOOP_OPERATOR_ARTIFACT_DIR");
        if (!configured.trimmed().isEmpty())
        {
            return configured;
        }
        return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("loop-operator-loop"));
    }

    void writeFailureArtifact(const QString& failure) const
    {
        const QString directory = artifactDirectory();
        if (!QDir().mkpath(directory))
        {
            return;
        }

        QString safeScenario = m_scenario;
        safeScenario.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
        const QString timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
        QSaveFile artifact(QDir(directory).filePath(QStringLiteral("%1-%2.json").arg(safeScenario, timestamp)));
        if (!artifact.open(QIODevice::WriteOnly))
        {
            return;
        }

        QJsonArray steps;
        for (const OperatorLoopTraceStep& step : m_steps)
        {
            steps.append(QJsonObject{ { QStringLiteral("action"), step.action },
                                      { QStringLiteral("outcome"), step.outcome },
                                      { QStringLiteral("detail"), step.detail } });
        }
        const QJsonObject document{
            { QStringLiteral("schema"), QStringLiteral("loop.operator-loop-failure") },
            { QStringLiteral("schema_version"), 1 },
            { QStringLiteral("scenario"), m_scenario },
            { QStringLiteral("failure"), failure.left(4096) },
            { QStringLiteral("platform"), QSysInfo::prettyProductName() },
            { QStringLiteral("timestamp_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
            { QStringLiteral("steps"), steps }
        };
        artifact.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
        artifact.commit();
    }

    QString m_scenario;
    QList<OperatorLoopTraceStep> m_steps;
    QString m_failure;
    bool m_complete = false;
    bool m_failureArtifactWritten = false;
};

constexpr char DEFAULT_PROFILE_REL[] = "profiles/loop-default.json";

inline QString fixturesDir()
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR "/testdata/fixtures");
}

inline QString sourceDir()
{
    return QStringLiteral(LOOP_PREFLIGHT_SOURCE_DIR);
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
        if (stdErr)
        {
            *stdErr = process.errorString().toUtf8();
        }
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
            if (stdErr)
            {
                *stdErr = QByteArrayLiteral("process timed out after 120000 ms");
            }
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
