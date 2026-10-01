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

#include "pdfworkerprotocol.h"
#include "pdfworkersandbox.h"

#if defined(Q_OS_WIN)
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
void output(const QJsonObject& object)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    fflush(stdout);
}

int networkError = 0;

bool networkDenied(quint16 port)
{
#ifdef Q_OS_WIN
    WSADATA data{};
    networkError = WSAStartup(MAKEWORD(2, 2), &data);
    if (networkError)
        return false;
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET)
        return WSAGetLastError() == WSAEACCES;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    u_long nonblocking = 1;
    if (ioctlsocket(connection, FIONBIO, &nonblocking))
    {
        closesocket(connection);
        WSACleanup();
        return false;
    }
    const int result = connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = result == 0 ? 0 : WSAGetLastError();
    if (error == WSAEWOULDBLOCK)
    {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(connection, &writable);
        FD_SET(connection, &failed);
        timeval deadline{ 2, 0 };
        const int ready = select(0, nullptr, &writable, &failed, &deadline);
        if (ready == 0)
            error = WSAETIMEDOUT;
        else if (ready == SOCKET_ERROR)
            error = WSAGetLastError();
        else
        {
            int length = sizeof(error);
            if (getsockopt(connection, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length))
                error = WSAGetLastError();
        }
    }
    networkError = error;
    closesocket(connection);
    WSACleanup();
    return result == SOCKET_ERROR && (error == WSAEACCES || error == WSAETIMEDOUT);
#else
    Q_UNUSED(port);
    const int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection >= 0)
        close(connection);
    return connection < 0 && errno == EACCES;
#endif
}

bool childDenied()
{
#ifdef Q_OS_WIN
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    const auto executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    const BOOL created = CreateProcessW(executable.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &child);
    if (created)
    {
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
    const DWORD error = GetLastError();
    return !created && (error == ERROR_ACCESS_DENIED || error == ERROR_CHILD_PROCESS_BLOCKED);
#else
    const pid_t child = fork();
    if (child == 0)
        _exit(0);
    return child < 0 && errno == EACCES;
#endif
}
}

int main(int argc, char** argv)
{
#ifdef Q_OS_WIN
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto option = [&](const QString& key)
    {
        const int index = args.indexOf(key);
        return index >= 0 && index + 1 < args.size() ? args.at(index + 1) : QString();
    };
    const pdftool::worker::WorkerSandboxPaths paths{ option(QStringLiteral("--sandbox-input")),
                                                     option(QStringLiteral("--sandbox-temp")), option(QStringLiteral("--sandbox-output")) };
    QString error;
    if (!pdftool::worker::applyWorkerSandbox(paths, {}, &error))
        return 3;
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 4;
    for (;;)
    {
        const auto bytes = input.readLine(pdftool::worker::MAX_REQUEST_BYTES + 1);
        if (bytes.isEmpty())
            break;
        const auto request = QJsonDocument::fromJson(bytes).object();
        const QString id = request.value(QStringLiteral("id")).toString();
        const QString op = request.value(QStringLiteral("op")).toString();
        if (op == QLatin1String("ping"))
        {
            output(pdftool::worker::makeOkResponse(id, op, QStringLiteral("success"),
                                                   { { QStringLiteral("sandbox"), pdftool::worker::sandboxStatusJson(true, QStringLiteral("test-probe")) } }));
            continue;
        }
        const QString mode = request.value(QStringLiteral("password")).toString();
        if (mode == QLatin1String("crash"))
            std::abort();
        if (mode == QLatin1String("hang"))
            for (;;)
                QThread::msleep(10);
        if (mode == QLatin1String("cpu"))
            for (;;)
            {
                static volatile unsigned value = 0;
                value = value + 1;
            }
        if (mode == QLatin1String("memory"))
        {
            std::vector<QByteArray> allocations;
            for (;;)
                allocations.emplace_back(16 * 1024 * 1024, 'x');
        }
        if (mode == QLatin1String("malformed"))
        {
            fputs("{invalid-json\n", stdout);
            fflush(stdout);
            continue;
        }
        if (mode == QLatin1String("oversized"))
        {
            const QByteArray chunk(65536, 'x');
            for (int i = 0; i < 1025; ++i)
                fwrite(chunk.constData(), 1, size_t(chunk.size()), stdout);
            fflush(stdout);
            continue;
        }
        auto response = pdftool::worker::makeOkResponse(id, op, QStringLiteral("success"));
        response.insert(QStringLiteral("input_sha256"), request.value(QStringLiteral("input_sha256")));
        response.insert(QStringLiteral("profile_sha256"), request.value(QStringLiteral("profile_sha256")));
        if (mode == QLatin1String("wrong-id"))
            response.insert(QStringLiteral("id"), QStringLiteral("other-request"));
        if (mode == QLatin1String("wrong-version"))
            response.insert(QStringLiteral("v"), 1);
        if (mode == QLatin1String("wrong-op"))
            response.insert(QStringLiteral("op"), QStringLiteral("open"));
        if (mode == QLatin1String("wrong-status"))
            response.insert(QStringLiteral("ok"), false);
        if (mode == QLatin1String("raw-error"))
        {
            fputs("LOOP_CUSTOMER_SECRET_20\n", stderr);
            response = pdftool::worker::makeErrorResponse(id, op, QStringLiteral("input-error"),
                                                          QStringLiteral("LOOP_CUSTOMER_SECRET_20"), QStringLiteral("LOOP_CUSTOMER_SECRET_20"));
        }
        if (mode == QLatin1String("raw-success"))
        {
            response.insert(QStringLiteral("artifact"), QJsonObject{
                                                            { QStringLiteral("sha256"), request.value(QStringLiteral("input_sha256")) },
                                                            { QStringLiteral("size"), QFileInfo(request.value(QStringLiteral("input_path")).toString()).size() },
                                                            { QStringLiteral("mediaType"), QStringLiteral("application/pdf") },
                                                            { QStringLiteral("logicalName"), QStringLiteral("input.pdf") },
                                                            { QStringLiteral("storageToken"), QStringLiteral("worker-input") },
                                                            { QStringLiteral("extra"), QStringLiteral("LOOP_CUSTOMER_SECRET_20") } });
            response.insert(QStringLiteral("page_count"), 1);
            response.insert(QStringLiteral("verdict"), QStringLiteral("PASS"));
            response.insert(QStringLiteral("report_path"), QStringLiteral("LOOP_CUSTOMER_SECRET_20"));
        }
        if (mode.startsWith(QStringLiteral("probe:")))
        {
            const int separator = mode.indexOf(QLatin1Char(':'), 6);
            const quint16 port = mode.mid(6, separator - 6).toUShort();
            QFile outside(mode.mid(separator + 1));
            QFile runtime(QCoreApplication::applicationFilePath());
            QFile temporary(QDir(paths.tempDir).filePath(QStringLiteral("allowed.tmp")));
            const bool outsideDenied = !outside.open(QIODevice::ReadOnly);
            const bool runtimeDenied = !runtime.open(QIODevice::WriteOnly | QIODevice::Append);
            const bool tempAllowed = temporary.open(QIODevice::WriteOnly) && temporary.write("ok") == 2;
            temporary.close();
            QFile snapshot(request.value(QStringLiteral("input_path")).toString());
            const bool snapshotDenied = !snapshot.open(QIODevice::WriteOnly | QIODevice::Append);
#ifdef Q_OS_WIN
            QFile profile(QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(QStringLiteral("../outside-temp.tmp")));
            const bool profileDenied = !profile.open(QIODevice::WriteOnly);
#else
            const bool profileDenied = true;
#endif
            response.insert(QStringLiteral("artifact"), QJsonObject{ { QStringLiteral("sha256"), request.value(QStringLiteral("input_sha256")) } });
            response.insert(QStringLiteral("page_count"), 1);
            response.insert(QStringLiteral("runtime_path"), QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
            response.insert(QStringLiteral("profile_path"), QDir::cleanPath(QDir(qEnvironmentVariable("LOCALAPPDATA")).absoluteFilePath(QStringLiteral(".."))));
#endif
            response.insert(QStringLiteral("probe"), QJsonObject{
                                                         { QStringLiteral("network_denied"), networkDenied(port) }, { QStringLiteral("child_denied"), childDenied() }, { QStringLiteral("outside_denied"), outsideDenied }, { QStringLiteral("runtime_denied"), runtimeDenied }, { QStringLiteral("temp_allowed"), tempAllowed }, { QStringLiteral("snapshot_denied"), snapshotDenied }, { QStringLiteral("network_error"), networkError }, { QStringLiteral("profile_denied"), profileDenied } });
        }
        output(response);
    }
    return 0;
}
