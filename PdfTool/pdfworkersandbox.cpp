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
    // Deny networking syscalls; everything else is allowed. Hand-rolled BPF so
    // we do not introduce a libseccomp link dependency.
    const std::vector<__u32> denied = {
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
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));

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

    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
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
        // Create parent directory for temp/output if missing.
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
    if (abi < 1)
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
        LANDLOCK_ACCESS_FS_REMOVE_DIR;

    // When input is a file, Landlock path_beneath on the file itself grants
    // access to that file; when it is a directory, the whole tree is readable.
    if (!addLandlockPath(rulesetFd, input, readOnly, errorMessage) ||
        !addLandlockPath(rulesetFd, tempDir, readWrite, errorMessage) ||
        !addLandlockPath(rulesetFd, outputDir, readWrite, errorMessage))
    {
        ::close(rulesetFd);
        return false;
    }

    // Allow reading the worker binary and shared libraries under /usr and /lib
    // so Qt/LoopLibCore can continue to resolve after restriction. Without this
    // the process dies on the next dlopen. Also allow common read-only system
    // roots Qt and libc touch during startup (/etc, /dev, /proc).
    for (const char* root : { "/usr", "/lib", "/lib64", "/opt", "/etc", "/dev", "/proc", "/sys" })
    {
        if (::access(root, F_OK) != 0)
        {
            continue;
        }
        QString ignored;
        if (!addLandlockPath(rulesetFd, QString::fromLatin1(root),
                             LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
                             &ignored))
        {
            // Optional roots may be absent or unopenable; continue.
        }
    }

    // Allow the directory that contains the worker executable (build/install tree).
    {
        char selfPath[4096] = {};
        const ssize_t length = ::readlink("/proc/self/exe", selfPath, sizeof(selfPath) - 1);
        if (length > 0)
        {
            selfPath[length] = '\0';
            const QString exeDir = QFileInfo(QString::fromLocal8Bit(selfPath, int(length))).absolutePath();
            QString ignored;
            addLandlockPath(rulesetFd, exeDir,
                            LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
                            &ignored);
            const QString libDir = QFileInfo(exeDir + QStringLiteral("/../lib")).absoluteFilePath();
            if (QFileInfo::exists(libDir))
            {
                addLandlockPath(rulesetFd, libDir,
                                LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
                                &ignored);
            }
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

QJsonObject sandboxStatusJson(bool applied, const QString& detail)
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
        { QStringLiteral("rss_limit_bytes"), DEFAULT_RSS_LIMIT_BYTES },
        { QStringLiteral("cpu_limit_seconds"), DEFAULT_CPU_SECONDS },
    };
}

}   // namespace pdftool::worker
