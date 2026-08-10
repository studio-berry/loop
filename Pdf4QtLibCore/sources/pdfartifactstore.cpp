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

#include "pdfartifactstore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QTemporaryFile>

#include <utility>

namespace pdf
{

PDFArtifactStore::PDFArtifactStore(QString rootDirectory) :
    m_rootDirectory(std::move(rootDirectory))
{
}

QString PDFArtifactStore::artifactToken(const QString& sha256) const
{
    const QString normalized = sha256.toLower();
    return QDir(QStringLiteral("artifacts")).filePath(QDir(normalized.left(2)).filePath(normalized));
}

QString PDFArtifactStore::pathFor(const PDFArtifactIdentity& artifact) const
{
    if (!isPDFSha256(artifact.sha256))
    {
        return {};
    }
    return QDir(m_rootDirectory).filePath(artifactToken(artifact.sha256));
}

bool PDFArtifactStore::contains(const PDFArtifactIdentity& artifact) const
{
    const QString path = pathFor(artifact);
    return !path.isEmpty() && QFileInfo::exists(path) && QFileInfo(path).isFile() &&
           QFileInfo(path).size() == artifact.size;
}

bool PDFArtifactStore::verify(const PDFArtifactIdentity& artifact) const
{
    const QString path = pathFor(artifact);
    QFile file(path);
    if (!artifact.isValid() || !file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && !file.atEnd())
        {
            return false;
        }
        hash.addData(chunk);
    }
    return file.size() == artifact.size && QString::fromLatin1(hash.result().toHex()) == artifact.sha256.toLower();
}

bool PDFArtifactStore::publishReadOnly(const QString& path) const
{
    return QFile::setPermissions(path,
                                 QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
}

bool PDFArtifactStore::remove(const PDFArtifactIdentity& artifact) const
{
    const QString path = pathFor(artifact);
    if (path.isEmpty() || !QFileInfo::exists(path))
    {
        return false;
    }
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                          QFileDevice::ReadGroup | QFileDevice::ReadOther);
    return QFile::remove(path);
}

PDFArtifactRestoreResult PDFArtifactStore::restoreToFile(const PDFArtifactIdentity& artifact,
                                                         const QString& destinationPath) const
{
    PDFArtifactRestoreResult result;
    if (!verify(artifact))
    {
        result.errorMessage = QStringLiteral("Rollback artifact failed integrity verification.");
        return result;
    }
    QFile source(pathFor(artifact));
    if (!source.open(QIODevice::ReadOnly))
    {
        result.errorMessage = QStringLiteral("Rollback artifact could not be opened.");
        return result;
    }
    QSaveFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly))
    {
        result.errorMessage = QStringLiteral("Rollback destination could not be opened atomically.");
        return result;
    }
    while (!source.atEnd())
    {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && !source.atEnd())
        {
            destination.cancelWriting();
            result.errorMessage = QStringLiteral("Rollback artifact could not be read.");
            return result;
        }
        if (destination.write(chunk) != chunk.size())
        {
            destination.cancelWriting();
            result.errorMessage = QStringLiteral("Rollback destination could not be written.");
            return result;
        }
    }
    if (!destination.commit())
    {
        result.errorMessage = QStringLiteral("Rollback destination could not be committed atomically.");
        return result;
    }
    result.success = true;
    return result;
}

PDFArtifactStoreResult PDFArtifactStore::importFile(const QString& sourcePath,
                                                    PDFArtifactImportOptions options) const
{
    QFile source(sourcePath);
    PDFArtifactStoreResult result;
    if (!source.open(QIODevice::ReadOnly))
    {
        result.errorMessage = QStringLiteral("Could not open artifact source '%1'.").arg(sanitizeArtifactLogicalName(sourcePath));
        return result;
    }
    if (options.logicalName.isEmpty())
    {
        options.logicalName = QFileInfo(sourcePath).fileName();
    }
    return importDevice(&source, options);
}

PDFArtifactStoreResult PDFArtifactStore::importBytes(const QByteArray& bytes,
                                                     PDFArtifactImportOptions options) const
{
    QBuffer source;
    source.setData(bytes);
    if (!source.open(QIODevice::ReadOnly))
    {
        return { false, false, {}, QStringLiteral("Could not open in-memory artifact source.") };
    }
    if (options.logicalName.isEmpty())
    {
        options.logicalName = QStringLiteral("artifact");
    }
    return importDevice(&source, options);
}

PDFArtifactStoreResult PDFArtifactStore::importDevice(QIODevice* source,
                                                      const PDFArtifactImportOptions& options) const
{
    PDFArtifactStoreResult result;
    if (!source || !source->isOpen())
    {
        result.errorMessage = QStringLiteral("Artifact source is not open.");
        return result;
    }

    const QString artifactsDirectory = QDir(m_rootDirectory).filePath(QStringLiteral("artifacts"));
    if (!QDir().mkpath(artifactsDirectory))
    {
        result.errorMessage = QStringLiteral("Could not create the artifact store directory.");
        return result;
    }

    QTemporaryFile temporary(QDir(artifactsDirectory).filePath(QStringLiteral(".artifact-XXXXXX.tmp")));
    temporary.setAutoRemove(false);
    if (!temporary.open())
    {
        result.errorMessage = QStringLiteral("Could not create an atomic artifact staging file.");
        return result;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 size = 0;
    while (!source->atEnd())
    {
        const QByteArray chunk = source->read(1024 * 1024);
        if (chunk.isEmpty() && !source->atEnd())
        {
            temporary.remove();
            result.errorMessage = QStringLiteral("Could not read the artifact source.");
            return result;
        }
        if (temporary.write(chunk) != chunk.size())
        {
            temporary.remove();
            result.errorMessage = QStringLiteral("Could not stage the artifact atomically.");
            return result;
        }
        hash.addData(chunk);
        size += chunk.size();
    }
    if (!temporary.flush())
    {
        temporary.remove();
        result.errorMessage = QStringLiteral("Could not flush the staged artifact.");
        return result;
    }
    temporary.close();

    const QString sha256 = QString::fromLatin1(hash.result().toHex());
    const QString token = artifactToken(sha256);
    const QString finalPath = QDir(m_rootDirectory).filePath(token);
    const QFileInfo finalInfo(finalPath);
    bool newlyPublished = false;
    if (!QDir().mkpath(finalInfo.absolutePath()))
    {
        QFile::remove(temporary.fileName());
        result.errorMessage = QStringLiteral("Could not create the artifact digest directory.");
        return result;
    }

    if (QFileInfo::exists(finalPath))
    {
        const bool sameSize = QFileInfo(finalPath).size() == size;
        QFile::remove(temporary.fileName());
        if (!sameSize)
        {
            result.errorMessage = QStringLiteral("Artifact digest collision detected.");
            return result;
        }
        PDFArtifactIdentity existing;
        existing.sha256 = sha256;
        existing.size = size;
        if (!verify(existing))
        {
            result.errorMessage = QStringLiteral("Existing content-addressed artifact failed integrity verification.");
            return result;
        }
        result.reused = true;
    }
    else if (!QFile::rename(temporary.fileName(), finalPath))
    {
        QFile::remove(temporary.fileName());
        result.errorMessage = QStringLiteral("Could not atomically publish the artifact.");
        return result;
    }
    else
    {
        newlyPublished = true;
    }

    if (!publishReadOnly(finalPath))
    {
        if (newlyPublished)
        {
            QFile::setPermissions(finalPath,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther);
            QFile::remove(finalPath);
        }
        result.errorMessage = QStringLiteral("Could not publish the artifact as read-only.");
        return result;
    }

    result.success = true;
    result.artifact.sha256 = sha256;
    result.artifact.size = size;
    result.artifact.mediaType = options.mediaType.trimmed().isEmpty() ? QStringLiteral("application/octet-stream") : options.mediaType.trimmed();
    result.artifact.logicalName = sanitizeArtifactLogicalName(options.logicalName);
    result.artifact.storageToken = token;
    return result;
}

} // namespace pdf
