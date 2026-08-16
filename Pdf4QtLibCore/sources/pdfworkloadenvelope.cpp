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

#include "pdfworkloadenvelope.h"
#include "pdfconstants.h"

#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QSysInfo>
#include <QtGlobal>

namespace pdf
{

namespace
{

QString compilerIdentity()
{
#if defined(__clang__)
    return QStringLiteral("clang %1").arg(QString::fromLatin1(__clang_version__));
#elif defined(__GNUC__)
    return QStringLiteral("gcc %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return QStringLiteral("msvc %1").arg(_MSC_VER);
#else
    return QStringLiteral("unknown");
#endif
}

QString buildTypeIdentity()
{
#ifdef NDEBUG
    return QStringLiteral("Release");
#else
    return QStringLiteral("Debug");
#endif
}

QString gitCommitIdentity()
{
    const QByteArray env = qgetenv("GIT_COMMIT");
    if (!env.isEmpty())
    {
        return QString::fromUtf8(env).trimmed();
    }

    const QByteArray sha = qgetenv("GITHUB_SHA");
    if (!sha.isEmpty())
    {
        return QString::fromUtf8(sha).trimmed();
    }

#ifdef PDF4QT_GIT_COMMIT
    {
        const QString compiled = QString::fromLatin1(PDF4QT_GIT_COMMIT).trimmed();
        if (!compiled.isEmpty())
        {
            return compiled;
        }
    }
#endif

    QProcess git;
    git.setProcessChannelMode(QProcess::SeparateChannels);
    git.start(QStringLiteral("git"), { QStringLiteral("rev-parse"), QStringLiteral("HEAD") });
    if (git.waitForFinished(2000) && git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0)
    {
        const QString result = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
        if (!result.isEmpty())
        {
            return result;
        }
    }

    return QString();
}

}   // namespace

PDFRunIdentity PDFRunIdentity::capture()
{
    PDFRunIdentity identity;
    identity.commit = gitCommitIdentity();
    identity.compiler = compilerIdentity();
    identity.buildType = buildTypeIdentity();
    identity.os = QStringLiteral("%1 %2").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    identity.qtVersion = QString::fromLatin1(qVersion());
    identity.cpuArchitecture = QSysInfo::currentCpuArchitecture();
    identity.gpu = qEnvironmentVariable("PDF4QT_GPU", QStringLiteral("unspecified"));
    identity.renderer = QStringLiteral("pdf4qt");
    identity.productVersion = QString::fromLatin1(PDF_LIBRARY_VERSION);
    identity.operationVersion = QStringLiteral("benchmark-render");
    return identity;
}

QJsonObject PDFRunIdentity::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("commit"), commit);
    object.insert(QStringLiteral("compiler"), compiler);
    object.insert(QStringLiteral("build"), buildType);
    object.insert(QStringLiteral("os"), os);
    object.insert(QStringLiteral("qt"), qtVersion);
    object.insert(QStringLiteral("cpu"), cpuArchitecture);
    object.insert(QStringLiteral("gpu"), gpu);
    object.insert(QStringLiteral("renderer"), renderer);
    object.insert(QStringLiteral("fixture_digest"), fixtureDigest);
    object.insert(QStringLiteral("profile_version"), profileVersion);
    object.insert(QStringLiteral("operation_version"), operationVersion);
    object.insert(QStringLiteral("product_version"), productVersion);
    return object;
}

QString PDFRunIdentity::digestBytes(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString PDFRunIdentity::digestFile(const QString& path)
{
    if (path.isEmpty())
    {
        return QString();
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
    {
        return QString();
    }
    return QString::fromLatin1(hash.result().toHex());
}

qint64 PDFWorkloadEnvelope::currentRssHighWaterBytes()
{
#ifdef Q_OS_LINUX
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return 0;
    }

    while (!status.atEnd())
    {
        const QByteArray line = status.readLine().trimmed();
        if (!line.startsWith("VmHWM:"))
        {
            continue;
        }

        const QList<QByteArray> parts = line.split(' ');
        for (const QByteArray& part : parts)
        {
            bool ok = false;
            const qint64 kiloBytes = part.toLongLong(&ok);
            if (ok && kiloBytes > 0)
            {
                return kiloBytes * 1024;
            }
        }
    }
#endif
    return 0;
}

QJsonObject PDFWorkloadEnvelope::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("identity"), identity.toJson());
    object.insert(QStringLiteral("family"), family);
    object.insert(QStringLiteral("page_count"), pageCount);
    object.insert(QStringLiteral("rss_high_water_bytes"), rssHighWaterBytes);
    object.insert(QStringLiteral("elapsed_ms"), elapsedMs);
    object.insert(QStringLiteral("prefetch_shed"), prefetchShed);
    object.insert(QStringLiteral("interaction_slot_held"), interactionSlotHeld);
    return object;
}

}   // namespace pdf
