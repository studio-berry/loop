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

#include "pdfrecoverymanager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace pdf
{

namespace
{

/// The source fingerprint reads the first and the last 64 KiB of the file, so a
/// rewrite that preserves the head is still detected as a replacement.
constexpr qint64 FingerprintBytes = 64 * 1024;

QString hashBytes(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray readFingerprintBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    const qint64 size = file.size();
    QByteArray bytes = file.read(FingerprintBytes);
    if (size > FingerprintBytes && file.seek(qMax<qint64>(FingerprintBytes, size - FingerprintBytes)))
    {
        bytes.append(file.read(FingerprintBytes));
    }
    return bytes;
}

QString normalizedPath(const QString& path)
{
    QString result = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    result = result.toLower();
#endif
    return QDir::fromNativeSeparators(result);
}

}   // namespace

RecoveryPolicy clampRecoveryPolicy(RecoveryPolicy policy)
{
    policy.intervalSeconds = qMax(1, policy.intervalSeconds);
    policy.debounceSeconds = qMax(0, policy.debounceSeconds);
    policy.maxBytes = qMax<qint64>(1, policy.maxBytes);
    policy.maxSessions = qMax(1, policy.maxSessions);
    policy.maxAgeDays = qMax(1, policy.maxAgeDays);
    return policy;
}

RecoverySourceIdentity inspectRecoverySource(const QString& sourcePath)
{
    RecoverySourceIdentity identity;
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile())
    {
        return identity;
    }

    identity.pathHash = hashBytes(normalizedPath(sourcePath).toUtf8());
    identity.prefixDigest = hashBytes(readFingerprintBytes(sourcePath));
    identity.size = info.size();
    identity.modifiedUtc = info.lastModified().toUTC();
    return identity;
}

RecoverySourceStatus classifyRecoverySource(const RecoverySourceIdentity& expected,
                                            const RecoverySourceIdentity& actual,
                                            bool sourceExists)
{
    if (!expected.isValid())
    {
        return RecoverySourceStatus::Invalid;
    }
    if (!sourceExists || !actual.isValid())
    {
        return RecoverySourceStatus::Missing;
    }
    return expected == actual ? RecoverySourceStatus::Unchanged : RecoverySourceStatus::Changed;
}

}   // namespace pdf
