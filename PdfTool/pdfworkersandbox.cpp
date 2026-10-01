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

#include "pdfworkersandbox.h"

#include "pdfworkerprotocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <vector>
#endif
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif

namespace pdftool::worker
{

namespace
{

#if defined(Q_OS_LINUX)

#ifndef landlock_create_ruleset
static inline int landlock_create_ruleset(const struct landlock_ruleset_attr* attr,
                                          size_t size,
                                          __u32 flags)
{
    return static_cast<int>(::syscall(SYS_landlock_create_ruleset, attr, size, flags));
}
#endif

#ifndef landlock_add_rule
static inline int landlock_add_rule(int ruleset_fd,
                                    enum landlock_rule_type rule_type,
                                    const void* rule_attr,
                                    __u32 flags)
{
    return static_cast<int>(::syscall(SYS_landlock_add_rule, ruleset_fd, rule_type, rule_attr, flags));
}
#endif

#ifndef landlock_restrict_self
static inline int landlock_restrict_self(int ruleset_fd, __u32 flags)
{
    return static_cast<int>(::syscall(SYS_landlock_restrict_self, ruleset_fd, flags));
}
#endif

bool setResourceLimits(const WorkerSandboxLimits& limits, QString* errorMessage)
{
    const qint64 rss = limits.rssBytes > 0 ? limits.rssBytes : DEFAULT_RSS_LIMIT_BYTES;
    const qint64 cpu = limits.cpuSeconds > 0 ? limits.cpuSeconds : DEFAULT_CPU_SECONDS;

    const struct rlimit coreLimit
    {
        0, 0
    };
    if (::setrlimit(RLIMIT_CORE, &coreLimit) != 0 || ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to disable worker dumps.");
        return false;
    }

    struct rlimit asLimit;
    asLimit.rlim_cur = static_cast<rlim_t>(rss);
    asLimit.rlim_max = static_cast<rlim_t>(rss);
    if (::setrlimit(RLIMIT_AS, &asLimit) != 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("setrlimit(RLIMIT_AS) failed: %1").arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }

    struct rlimit cpuLimit;
    cpuLimit.rlim_cur = static_cast<rlim_t>(cpu);
    cpuLimit.rlim_max = static_cast<rlim_t>(cpu);
    if (::setrlimit(RLIMIT_CPU, &cpuLimit) != 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("setrlimit(RLIMIT_CPU) failed: %1").arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }
    return true;
}

bool denyNetworkWithSeccomp(QString* errorMessage)
{
    const std::vector<__u32> denied = {
        static_cast<__u32>(__NR_execve),
#ifdef __NR_execveat
        static_cast<__u32>(__NR_execveat),
#endif
#ifdef __NR_fork
        static_cast<__u32>(__NR_fork),
#endif
#ifdef __NR_vfork
        static_cast<__u32>(__NR_vfork),
#endif
#ifdef __NR_io_uring_setup
        static_cast<__u32>(__NR_io_uring_setup),
#endif
        static_cast<__u32>(__NR_socket),
        static_cast<__u32>(__NR_connect),
        static_cast<__u32>(__NR_accept),
        static_cast<__u32>(__NR_accept4),
        static_cast<__u32>(__NR_bind),
        static_cast<__u32>(__NR_listen),
        static_cast<__u32>(__NR_sendto),
        static_cast<__u32>(__NR_sendmsg),
        static_cast<__u32>(__NR_recvfrom),
        static_cast<__u32>(__NR_recvmsg),
        static_cast<__u32>(__NR_shutdown),
#ifdef __NR_socketpair
        static_cast<__u32>(__NR_socketpair),
#endif
    };

    std::vector<sock_filter> filter;
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
#if defined(__x86_64__)
                              AUDIT_ARCH_X86_64,
#elif defined(__aarch64__)
                              AUDIT_ARCH_AARCH64,
#else
#error Unsupported worker seccomp architecture
#endif
                              1, 0));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));

#if defined(__x86_64__)
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#endif
#ifdef __NR_clone3
    // libc falls back to clone, whose flags can be checked for thread creation.
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS));
#endif
#ifdef __NR_clone
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 4));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])));
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x00010000 /* CLONE_THREAD */, 1, 0));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
#endif
    for (const __u32 nr : denied)
    {
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EACCES & SECCOMP_RET_DATA)));
    }
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("prctl(PR_SET_NO_NEW_PRIVS) failed: %1").arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }

    struct sock_fprog program;
    program.len = static_cast<unsigned short>(filter.size());
    program.filter = filter.data();

    if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program) != 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("prctl(PR_SET_SECCOMP) failed: %1").arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }
    return true;
}

bool pathAllowed(const QString& absolutePath, QString* errorMessage)
{
    const QFileInfo info(absolutePath);
    if (!info.exists())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Sandbox path does not exist: %1").arg(absolutePath);
        }
        return false;
    }
    return true;
}

bool addLandlockPath(int rulesetFd, const QString& absolutePath, __u64 access, QString* errorMessage)
{
    const QFileInfo info(absolutePath);
    const QByteArray utf8 = info.absoluteFilePath().toUtf8();
    const int fd = ::open(utf8.constData(), O_PATH | O_CLOEXEC);
    if (fd < 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Landlock open(%1) failed: %2")
                                .arg(absolutePath, QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }

    // Landlock rejects directory-only rights on file inodes (EINVAL).
    __u64 effectiveAccess = access;
    if (info.isFile())
    {
#ifdef LANDLOCK_ACCESS_FS_REFER
        effectiveAccess &= ~LANDLOCK_ACCESS_FS_REFER;
#endif
        effectiveAccess &= ~(LANDLOCK_ACCESS_FS_READ_DIR |
                             LANDLOCK_ACCESS_FS_REMOVE_DIR |
                             LANDLOCK_ACCESS_FS_MAKE_CHAR |
                             LANDLOCK_ACCESS_FS_MAKE_DIR |
                             LANDLOCK_ACCESS_FS_MAKE_REG |
                             LANDLOCK_ACCESS_FS_MAKE_SOCK |
                             LANDLOCK_ACCESS_FS_MAKE_FIFO |
                             LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                             LANDLOCK_ACCESS_FS_MAKE_SYM);
    }

    struct landlock_path_beneath_attr beneath = {};
    beneath.allowed_access = effectiveAccess;
    beneath.parent_fd = fd;
    const int rc = landlock_add_rule(rulesetFd, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0);
    const int savedErrno = errno;
    ::close(fd);
    if (rc != 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("landlock_add_rule(%1) failed: %2")
                                .arg(absolutePath, QString::fromLocal8Bit(::strerror(savedErrno)));
        }
        return false;
    }
    return true;
}

bool applyLandlock(const WorkerSandboxPaths& paths, QString* errorMessage)
{
    const int abi = landlock_create_ruleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
#if !defined(LANDLOCK_ACCESS_FS_TRUNCATE) || !defined(LANDLOCK_ACCESS_FS_REFER)
    if (errorMessage)
        *errorMessage = QStringLiteral("Landlock headers do not support required filesystem rights.");
    return false;
#else
    if (abi < 3)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Landlock is unavailable (ABI query failed: %1)")
                                .arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }

    struct landlock_ruleset_attr attr = {};
    attr.handled_access_fs =
        LANDLOCK_ACCESS_FS_EXECUTE |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;

    attr.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
    const int rulesetFd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rulesetFd < 0)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("landlock_create_ruleset failed: %1")
                                .arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }

    const QString input = QFileInfo(paths.inputPath).absoluteFilePath();
    const QString tempDir = QFileInfo(paths.tempDir).absoluteFilePath();
    const QString outputDir = QFileInfo(paths.outputDir).absoluteFilePath();

    if (!pathAllowed(input, errorMessage) ||
        !pathAllowed(tempDir, errorMessage) ||
        !pathAllowed(outputDir, errorMessage))
    {
        ::close(rulesetFd);
        return false;
    }

    // Input: read-only. Temp and output: read/write/create within the dir.
    const __u64 readOnly =
        LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    const __u64 readWrite =
        readOnly |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_DIR
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
        | LANDLOCK_ACCESS_FS_TRUNCATE
#endif
        ;

    // When input is a file, Landlock path_beneath on the file itself grants
    // access to that file; when it is a directory, the whole tree is readable.
    if (!addLandlockPath(rulesetFd, input, readOnly, errorMessage) ||
        !addLandlockPath(rulesetFd, tempDir, readWrite, errorMessage) ||
        !addLandlockPath(rulesetFd, outputDir, readWrite, errorMessage))
    {
        ::close(rulesetFd);
        return false;
    }

    // Already mapped libraries remain available. These roots cover deferred
    // font/locale/ICC data and runtime library loads; never grant /proc or /dev.
    const QStringList runtimeRoots{
        QStringLiteral("/usr/lib"), QStringLiteral("/usr/lib64"), QStringLiteral("/lib"), QStringLiteral("/lib64"),
        QStringLiteral("/usr/share/fonts"), QStringLiteral("/usr/share/fontconfig"), QStringLiteral("/etc/fonts"),
        QStringLiteral("/usr/share/color"), QStringLiteral("/usr/share/locale"), QStringLiteral("/etc/ld.so.cache"),
        QCoreApplication::applicationDirPath(), QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../lib"))
    };
    for (const QString& path : runtimeRoots)
    {
        if (QFileInfo::exists(path) && !addLandlockPath(rulesetFd, path, readOnly, errorMessage))
        {
            ::close(rulesetFd);
            return false;
        }
    }

    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        const int savedErrno = errno;
        ::close(rulesetFd);
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("prctl(PR_SET_NO_NEW_PRIVS) failed: %1")
                                .arg(QString::fromLocal8Bit(::strerror(savedErrno)));
        }
        return false;
    }

    if (landlock_restrict_self(rulesetFd, 0) != 0)
    {
        const int savedErrno = errno;
        ::close(rulesetFd);
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("landlock_restrict_self failed: %1")
                                .arg(QString::fromLocal8Bit(::strerror(savedErrno)));
        }
        return false;
    }

    ::close(rulesetFd);
    return true;
#endif
}

#endif   // Q_OS_LINUX

}   // namespace

bool applyWorkerSandbox(const WorkerSandboxPaths& paths,
                        const WorkerSandboxLimits& limits,
                        QString* errorMessage)
{
#if defined(Q_OS_LINUX)
    if (paths.inputPath.isEmpty() || paths.tempDir.isEmpty() || paths.outputDir.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Sandbox requires input, temp, and output paths.");
        }
        return false;
    }

    if (!QDir().mkpath(paths.tempDir) || !QDir().mkpath(paths.outputDir))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Failed to create sandbox temp/output directories.");
        }
        return false;
    }

    if (!setResourceLimits(limits, errorMessage))
    {
        return false;
    }
    if (!applyLandlock(paths, errorMessage))
    {
        return false;
    }
    if (!denyNetworkWithSeccomp(errorMessage))
    {
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    Q_UNUSED(paths);
    Q_UNUSED(limits);
    HANDLE token = nullptr;
    BOOL appContainer = FALSE;
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const BOOL isolated = GetTokenInformation(token, TokenIsAppContainer, &appContainer, sizeof(appContainer), &size);
    GetTokenInformation(token, TokenCapabilities, nullptr, 0, &size);
    std::vector<unsigned char> capabilities(size);
    const BOOL queried = size > 0 && GetTokenInformation(token, TokenCapabilities, capabilities.data(), size, &size);
    const bool noCapabilities = queried && reinterpret_cast<TOKEN_GROUPS*>(capabilities.data())->GroupCount == 0;
    CloseHandle(token);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    PROCESS_MITIGATION_CHILD_PROCESS_POLICY children{};
    const DWORD required = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                           JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME;
    if (!isolated || !appContainer)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("worker.sandbox.token");
        return false;
    }
    if (!noCapabilities)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("worker.sandbox.capabilities");
        return false;
    }
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof(job), nullptr))
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("worker.sandbox.job-query");
        return false;
    }
    if ((job.BasicLimitInformation.LimitFlags & required) != required ||
        job.BasicLimitInformation.ActiveProcessLimit != 1 ||
        job.ProcessMemoryLimit != static_cast<SIZE_T>(limits.rssBytes > 0 ? limits.rssBytes : DEFAULT_RSS_LIMIT_BYTES) ||
        job.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart != (limits.cpuSeconds > 0 ? limits.cpuSeconds : DEFAULT_CPU_SECONDS) * 10000000)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("worker.sandbox.job-limits");
        return false;
    }
    if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessChildProcessPolicy, &children, sizeof(children)) ||
        !children.NoChildProcessCreation)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("worker.sandbox.children");
        return false;
    }
    return true;
#else
    Q_UNUSED(paths);
    Q_UNUSED(limits);
    if (errorMessage)
    {
        *errorMessage = QStringLiteral("Linux worker sandbox is required; this platform has no release-worker sandbox yet.");
    }
    return false;
#endif
}

QJsonObject sandboxStatusJson(bool applied, const QString& detail, WorkerSandboxLimits limits)
{
    return QJsonObject{
        { QStringLiteral("applied"), applied },
        { QStringLiteral("platform"),
#if defined(Q_OS_LINUX)
          QStringLiteral("linux")
#elif defined(Q_OS_WIN)
          QStringLiteral("windows")
#else
          QStringLiteral("other")
#endif
        },
        { QStringLiteral("detail"), detail },
        { QStringLiteral("rss_limit_bytes"), (limits.rssBytes > 0 ? limits.rssBytes : DEFAULT_RSS_LIMIT_BYTES) },
        { QStringLiteral("cpu_limit_seconds"), (limits.cpuSeconds > 0 ? limits.cpuSeconds : DEFAULT_CPU_SECONDS) },
    };
}

}   // namespace pdftool::worker
