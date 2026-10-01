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

#include "pdfworkerprocess.h"
#include "pdfworkerprotocol.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSet>
#include <QMap>
#include <QThread>
#include <QUuid>

#include <vector>

namespace pdftool
{
namespace
{
class Handle
{
public:
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    void reset(HANDLE handle = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = handle;
    }
};

bool grantDirectory(const QString& path, const QString& containerSid, const QString& userSid, bool writable)
{
    const QString sddl = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)(A;OICI;%2;;;%3)")
                             .arg(userSid, writable ? QStringLiteral("FA") : QStringLiteral("GRGX"), containerSid) +
                         (writable ? QStringLiteral("S:(ML;OICI;NW;;;LW)") : QString());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr))
    {
        return false;
    }
    PACL dacl = nullptr, sacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
    GetSecurityDescriptorSacl(descriptor, &present, &sacl, &defaulted);
    auto nativePath = QDir::toNativeSeparators(path).toStdWString();
    const DWORD result = SetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION |
                                                   (writable ? LABEL_SECURITY_INFORMATION : 0),
                                               nullptr, nullptr, dacl, sacl);
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}

QString sidText(PSID sid)
{
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text))
        return {};
    const QString result = QString::fromWCharArray(text);
    LocalFree(text);
    return result;
}

QString currentUserSid()
{
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        return {};
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size))
        return {};
    return sidText(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
}

QString quoteArgument(const QString& argument)
{
    QString result = QStringLiteral("\"");
    qsizetype slashes = 0;
    for (const QChar ch : argument)
    {
        if (ch == QLatin1Char('\\'))
        {
            ++slashes;
            continue;
        }
        result += QString(slashes * (ch == QLatin1Char('"') ? 2 : 1), QLatin1Char('\\'));
        slashes = 0;
        if (ch == QLatin1Char('"'))
            result += QLatin1Char('\\');
        result += ch;
    }
    return result + QString(slashes * 2, QLatin1Char('\\')) + QLatin1Char('"');
}

bool pipePair(Handle& parent, Handle& child, bool parentWrites)
{
    const QString name = QStringLiteral("\\\\.\\pipe\\loop-worker-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    parent.value = CreateNamedPipeW(reinterpret_cast<LPCWSTR>(name.utf16()),
                                    (parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
    if (parent.value == INVALID_HANDLE_VALUE)
        return false;
    SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    child.value = CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()), parentWrites ? GENERIC_READ : GENERIC_WRITE,
                              0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    // Opening the client end connects the local pipe before process creation.
    return child.value != INVALID_HANDLE_VALUE;
}
}

struct WorkerProcess::State
{
    Handle process, job, input, output;
    DWORD processId = 0;
    qint64 lastExitCode = -1;
    QString profileName;
    PSID containerSid = nullptr;
    std::unique_ptr<QTemporaryDir> runtime;
    ~State()
    {
        process.reset();
        job.reset();
        input.reset();
        output.reset();
        if (containerSid)
            FreeSid(containerSid);
        if (!profileName.isEmpty())
            DeleteAppContainerProfile(reinterpret_cast<LPCWSTR>(profileName.utf16()));
    }
};

WorkerProcess::WorkerProcess() :
    m_state(std::make_unique<State>())
{
}
WorkerProcess::~WorkerProcess() { stop(); }

bool WorkerProcess::start(const QString& executable, const QStringList& arguments, const QString& inputDir,
                          const QString& tempDir, const QString& outputDir, QString& error)
{
    stop();
    error = QStringLiteral("Windows worker containment could not be established.");
    auto state = std::make_unique<State>();
    state->profileName = QStringLiteral("Loop.Worker.") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    error = QStringLiteral("worker.launch.profile");
    if (FAILED(CreateAppContainerProfile(reinterpret_cast<LPCWSTR>(state->profileName.utf16()),
                                         L"Loop PDF Worker", L"Isolated PDF inspection", nullptr, 0, &state->containerSid)))
    {
        return false;
    }
    const QString containerSid = sidText(state->containerSid);
    const QString userSid = currentUserSid();
    error = QStringLiteral("worker.launch.profile-path");
    PWSTR profileFolder = nullptr;
    if (containerSid.isEmpty() || userSid.isEmpty() ||
        FAILED(GetAppContainerFolderPath(reinterpret_cast<LPCWSTR>(containerSid.utf16()), &profileFolder)))
    {
        return false;
    }
    const QString appDataPath = QString::fromWCharArray(profileFolder);
    CoTaskMemFree(profileFolder);
    const QString profilePath = QFileInfo(appDataPath).dir().canonicalPath();
    if (QFileInfo(appDataPath).fileName() != QLatin1String("AC") ||
        QFileInfo(profilePath).fileName().compare(state->profileName, Qt::CaseInsensitive) != 0)
    {
        return false;
    }
    error = QStringLiteral("worker.launch.profile-acl");
    QDirIterator entries(profilePath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    QStringList profileEntries;
    while (entries.hasNext())
        profileEntries.append(entries.next());
    for (const QString& path : profileEntries)
    {
        if (!grantDirectory(path, containerSid, userSid, false))
            return false;
    }
    if (!grantDirectory(profilePath, containerSid, userSid, false))
        return false;
    error = QStringLiteral("worker.launch.directories");
    state->runtime = std::make_unique<QTemporaryDir>();
    if (containerSid.isEmpty() || userSid.isEmpty() || !state->runtime->isValid() ||
        !grantDirectory(state->runtime->path(), containerSid, userSid, false) ||
        !grantDirectory(inputDir, containerSid, userSid, false) ||
        !grantDirectory(tempDir, containerSid, userSid, true) ||
        !grantDirectory(outputDir, containerSid, userSid, true))
    {
        return false;
    }
    const QFileInfo workerInfo(executable);
    const QString stagedExecutable = state->runtime->filePath(workerInfo.fileName());
    error = QStringLiteral("worker.launch.manifest");
    QFile manifest(executable + QStringLiteral(".runtime"));
    if (!workerInfo.isFile() || !manifest.open(QIODevice::ReadOnly) || manifest.size() > 65536 ||
        !QFile::copy(executable, stagedExecutable) ||
        !grantDirectory(stagedExecutable, containerSid, userSid, false))
    {
        return false;
    }
    error = QStringLiteral("worker.launch.runtime-copy");
    QSet<QString> names{ workerInfo.fileName().toLower() };
    while (!manifest.atEnd())
    {
        const QString relative = QString::fromUtf8(manifest.readLine()).trimmed();
        const QFileInfo dependency(workerInfo.dir().filePath(relative));
        const QString canonical = dependency.canonicalFilePath();
        const QString root = workerInfo.dir().canonicalPath() + QLatin1Char('/');
        if (relative.isEmpty())
            continue;
        if (!relative.startsWith(QStringLiteral("worker-runtime/")) ||
            !canonical.startsWith(root, Qt::CaseInsensitive) || !dependency.isFile() ||
            dependency.isSymLink() || names.contains(dependency.fileName().toLower()) ||
            !QFile::copy(canonical, state->runtime->filePath(dependency.fileName())) ||
            !grantDirectory(state->runtime->filePath(dependency.fileName()), containerSid, userSid, false))
        {
            return false;
        }
        names.insert(dependency.fileName().toLower());
    }
    error = QStringLiteral("worker.launch.pipes");
    Handle childInput, childOutput, childError;
    if (!pipePair(state->input, childInput, true) || !pipePair(state->output, childOutput, false))
    {
        return false;
    }
    SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    childError.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &security, OPEN_EXISTING, 0, nullptr);
    error = QStringLiteral("worker.launch.job");
    state->job.value = CreateJobObjectW(nullptr, nullptr);
    if (childError.value == INVALID_HANDLE_VALUE || !state->job.value)
        return false;
    error = QStringLiteral("worker.launch.job-limits");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = worker::DEFAULT_CPU_SECONDS * 10000000;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(worker::DEFAULT_RSS_LIMIT_BYTES);
    if (!SetInformationJobObject(state->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return false;
    error = QStringLiteral("worker.launch.attributes");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 4, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 4, 0, &bytes))
        return false;
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = state->containerSid;
    HANDLE handles[]{ childInput.value, childOutput.value, childError.value };
    DWORD childPolicy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    const bool applied =
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof(capabilities), nullptr, nullptr) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &state->job.value, sizeof(HANDLE), nullptr, nullptr) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &childPolicy, sizeof(childPolicy), nullptr, nullptr);
    if (!applied)
    {
        const DWORD attributeError = GetLastError();
        DeleteProcThreadAttributeList(list);
        error += QStringLiteral(".%1").arg(attributeError);
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput.value;
    startup.StartupInfo.hStdOutput = childOutput.value;
    startup.StartupInfo.hStdError = childError.value;
    startup.lpAttributeList = list;
    QString command = quoteArgument(QDir::toNativeSeparators(stagedExecutable));
    for (const QString& argument : arguments)
        command += QLatin1Char(' ') + quoteArgument(argument);
    auto commandBuffer = command.toStdWString();
    // No inherited PATH, Qt plugin overrides, credentials or reporting configuration.
    wchar_t windowsDirectory[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH))
    {
        DeleteProcThreadAttributeList(list);
        return false;
    }
    QMap<QString, QString> systemEnvironment;
    for (const QString& key : { QStringLiteral("APPDATA"), QStringLiteral("LOCALAPPDATA"), QStringLiteral("USERPROFILE"),
                                QStringLiteral("ProgramData"), QStringLiteral("SystemDrive") })
    {
        const QString value = qEnvironmentVariable(qPrintable(key));
        if (!value.isEmpty())
            systemEnvironment.insert(key, value);
    }
    systemEnvironment.insert(QStringLiteral("SystemRoot"), QString::fromWCharArray(windowsDirectory));
    systemEnvironment.insert(QStringLiteral("windir"), QString::fromWCharArray(windowsDirectory));
    systemEnvironment.insert(QStringLiteral("ALLUSERSPROFILE"), qEnvironmentVariable("ProgramData"));
    systemEnvironment.insert(QStringLiteral("TEMP"), QDir::toNativeSeparators(tempDir));
    systemEnvironment.insert(QStringLiteral("TMP"), QDir::toNativeSeparators(tempDir));
    QString environment;
    for (auto it = systemEnvironment.cbegin(); it != systemEnvironment.cend(); ++it)
    {
        environment += it.key() + QLatin1Char('=') + it.value() + QChar(0);
    }
    environment += QChar(0);
    const auto currentDirectory = QDir::toNativeSeparators(state->runtime->path()).toStdWString();
    PROCESS_INFORMATION process{};
    error = QStringLiteral("worker.launch.create-process");
    const BOOL created = CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT,
                                        const_cast<ushort*>(environment.utf16()), currentDirectory.c_str(), &startup.StartupInfo, &process);
    const DWORD creationError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(list);
    if (!created)
    {
        error += QStringLiteral(".%1").arg(creationError);
        return false;
    }
    CloseHandle(process.hThread);
    state->process.value = process.hProcess;
    state->processId = process.dwProcessId;
    m_state = std::move(state);
    error.clear();
    return true;
}

void WorkerProcess::stop()
{
    qint64 exitCode = m_state->lastExitCode;
    if (m_state->job.value)
    {
        TerminateJobObject(m_state->job.value, 1);
        m_state->job.reset();
    }
    if (m_state->process.value)
    {
        WaitForSingleObject(m_state->process.value, 5000);
        DWORD status = 0;
        if (GetExitCodeProcess(m_state->process.value, &status))
            exitCode = status;
    }
    m_state = std::make_unique<State>();
    m_state->lastExitCode = exitCode;
}

bool WorkerProcess::running() const
{
    return m_state->process.value && WaitForSingleObject(m_state->process.value, 0) == WAIT_TIMEOUT;
}
qint64 WorkerProcess::pid() const { return m_state->processId; }
qint64 WorkerProcess::exitCode() const
{
    DWORD status = 0;
    if (m_state->process.value && GetExitCodeProcess(m_state->process.value, &status) && status != STILL_ACTIVE)
        return status;
    return m_state->lastExitCode;
}

bool WorkerProcess::write(const QByteArray& bytes, QElapsedTimer& timer, int timeoutMs, const std::atomic_bool& cancelled)
{
    OVERLAPPED operation{};
    Handle event;
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value)
        return false;
    operation.hEvent = event.value;
    DWORD written = 0;
    if (WriteFile(m_state->input.value, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, &operation))
        return written == bytes.size();
    if (GetLastError() != ERROR_IO_PENDING)
        return false;
    while (timer.elapsed() < timeoutMs && !cancelled.load())
    {
        if (WaitForSingleObject(event.value, 10) == WAIT_OBJECT_0)
            return GetOverlappedResult(m_state->input.value, &operation, &written, FALSE) && written == bytes.size();
    }
    CancelIoEx(m_state->input.value, &operation);
    GetOverlappedResult(m_state->input.value, &operation, &written, TRUE);
    return false;
}

QByteArray WorkerProcess::read(qint64 maximum)
{
    DWORD available = 0;
    if (!PeekNamedPipe(m_state->output.value, nullptr, 0, nullptr, &available, nullptr))
        return {};
    if (!available)
    {
        QThread::msleep(5);
        return {};
    }
    QByteArray bytes(qMin(qMin(maximum, qint64(available)), qint64(65536)), Qt::Uninitialized);
    OVERLAPPED operation{};
    Handle event;
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value)
        return {};
    operation.hEvent = event.value;
    DWORD count = 0;
    const BOOL completed = ReadFile(m_state->output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, &operation);
    if (!completed)
    {
        if (GetLastError() != ERROR_IO_PENDING)
            return {};
        if (WaitForSingleObject(event.value, 10) != WAIT_OBJECT_0)
        {
            CancelIoEx(m_state->output.value, &operation);
            GetOverlappedResult(m_state->output.value, &operation, &count, TRUE);
            return {};
        }
        if (!GetOverlappedResult(m_state->output.value, &operation, &count, FALSE))
            return {};
    }
    bytes.resize(count);
    return bytes;
}
}
#endif
