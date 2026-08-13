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

#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"
#include "pdfsafefilewriter.h"
#include "pdfsecurityhandler.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <memory>

namespace pdfviewer
{

struct PDFRecoveryManager::CheckpointResult
{
    bool success = false;
    QString diagnosticCode;
    QString message;
    QString sessionDirectory;
    quint64 revision = 0;
};

namespace
{

constexpr int RecoverySchema = 1;
constexpr qint64 FingerprintBytes = 64 * 1024;
const QString RecoveryPdfName = QStringLiteral("document.recovery.pdf");
const QString RecoveryManifestName = QStringLiteral("manifest.json");
const QString RecoveryPdfPreviousName = QStringLiteral("document.recovery.pdf.previous");
const QString RecoveryManifestPreviousName = QStringLiteral("manifest.json.previous");
const QString RecoveryPdfPartialName = QStringLiteral("document.recovery.pdf.partial");
const QString RecoveryManifestPartialName = QStringLiteral("manifest.json.partial");

struct CheckpointRequest
{
    QString sessionDirectory;
    QString sessionId;
    QString sourcePath;
    RecoverySourceIdentity sourceIdentity;
    pdf::PDFDocumentPointer document;
    quint64 revision = 0;
    bool signedDocument = false;
};

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

QString hashFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && !file.atEnd())
        {
            return {};
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString normalizedPath(const QString& path)
{
    QString result = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    result = result.toLower();
#endif
    return QDir::fromNativeSeparators(result);
}

QJsonObject identityToJson(const RecoverySourceIdentity& identity)
{
    QJsonObject object;
    object.insert(QStringLiteral("pathHash"), identity.pathHash);
    object.insert(QStringLiteral("prefixDigest"), identity.prefixDigest);
    object.insert(QStringLiteral("size"), identity.size);
    object.insert(QStringLiteral("mtimeUtc"), identity.modifiedUtc.toString(Qt::ISODateWithMs));
    return object;
}

RecoverySourceIdentity identityFromJson(const QJsonObject& object)
{
    RecoverySourceIdentity identity;
    identity.pathHash = object.value(QStringLiteral("pathHash")).toString();
    identity.prefixDigest = object.value(QStringLiteral("prefixDigest")).toString();
    identity.size = object.value(QStringLiteral("size")).toInteger(-1);
    identity.modifiedUtc = QDateTime::fromString(object.value(QStringLiteral("mtimeUtc")).toString(), Qt::ISODateWithMs);
    return identity;
}

QString generationPath(const QString& sessionDirectory, bool previous, bool manifest)
{
    if (manifest)
    {
        return QDir(sessionDirectory).filePath(previous ? RecoveryManifestPreviousName : RecoveryManifestName);
    }
    return QDir(sessionDirectory).filePath(previous ? RecoveryPdfPreviousName : RecoveryPdfName);
}

bool replaceGeneration(const QString& finalPath, const QString& previousPath, const QString& nextPath)
{
    if (QFile::exists(finalPath))
    {
        QFile::remove(previousPath);
        if (!QFile::rename(finalPath, previousPath))
        {
            return false;
        }
    }

    QFile::remove(finalPath);
    return QFile::rename(nextPath, finalPath);
}

bool safeSessionDirectory(const QString& root, const QString& sessionDirectory)
{
    const QString canonicalRoot = QDir(root).absolutePath();
    const QString canonicalSession = QDir(sessionDirectory).absolutePath();
    return canonicalSession.startsWith(canonicalRoot + QDir::separator())
           && QFileInfo(canonicalSession).fileName().size() == 36;
}

bool readGeneration(const QString& sessionDirectory, bool previous, RecoveryCandidate* candidate)
{
    const QString manifestPath = generationPath(sessionDirectory, previous, true);
    const QString recoveryPath = generationPath(sessionDirectory, previous, false);
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly))
    {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return false;
    }

    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toInt() != RecoverySchema
        || !QFileInfo::exists(recoveryPath))
    {
        return false;
    }

    const QString expectedDigest = object.value(QStringLiteral("recoverySha256")).toString();
    if (expectedDigest.isEmpty() || expectedDigest.compare(hashFile(recoveryPath), Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    const QJsonObject source = object.value(QStringLiteral("source")).toObject();
    RecoveryCandidate result;
    result.sessionId = object.value(QStringLiteral("sessionId")).toString();
    result.sessionDirectory = sessionDirectory;
    result.recoveryFile = recoveryPath;
    result.sourcePath = source.value(QStringLiteral("path")).toString();
    result.sourceFileName = QFileInfo(result.sourcePath).fileName();
    result.checkpointUtc = object.value(QStringLiteral("checkpointUtc")).toString();
    result.documentRevision = object.value(QStringLiteral("documentRevision")).toVariant().toULongLong();
    result.cleanlySaved = object.value(QStringLiteral("cleanlySaved")).toBool(false);
    result.signedDocument = object.value(QStringLiteral("signedDocument")).toBool(false);
    result.sourceIdentity = identityFromJson(source);
    result.valid = result.sourceIdentity.isValid() && !result.sessionId.isEmpty() && !result.sourcePath.isEmpty();
    result.sourceStatus = result.valid ? RecoverySourceStatus::Unchanged : RecoverySourceStatus::Invalid;
    if (!result.valid)
    {
        result.diagnosticCode = QStringLiteral("invalid-manifest");
        result.diagnosticMessage = QStringLiteral("The recovery manifest is missing required identity fields.");
    }
    *candidate = qMove(result);
    return true;
}

bool removeInactiveSession(const QString& directory)
{
    QLockFile lock(QDir(directory).filePath(QStringLiteral("lock")));
    if (!lock.tryLock(0))
    {
        return false;
    }

    const bool removed = QDir(directory).removeRecursively();
    lock.unlock();
    return removed;
}

void enforceRetentionInWorker(const RecoveryPolicy& policy,
                              const QList<RecoveryCandidate>& candidates)
{
    qint64 totalBytes = 0;
    QSet<QString> removed;
    for (const RecoveryCandidate& candidate : candidates)
    {
        for (const QFileInfo& file : QDir(candidate.sessionDirectory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
        {
            totalBytes += file.size();
        }
    }

    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-policy.maxAgeDays);
    for (const RecoveryCandidate& candidate : candidates)
    {
        const QDateTime checkpoint = QDateTime::fromString(candidate.checkpointUtc, Qt::ISODateWithMs);
        if (candidate.sourceStatus != RecoverySourceStatus::Active
            && checkpoint.isValid() && checkpoint < cutoff
            && removeInactiveSession(candidate.sessionDirectory))
        {
            removed.insert(candidate.sessionDirectory);
        }
    }

    QList<RecoveryCandidate> remaining;
    for (const RecoveryCandidate& candidate : candidates)
    {
        if (!removed.contains(candidate.sessionDirectory))
        {
            remaining.push_back(candidate);
        }
    }

    auto removeOldest = [&]() {
        for (int index = remaining.size() - 1; index >= 0; --index)
        {
            if (remaining.at(index).sourceStatus != RecoverySourceStatus::Active
                && removeInactiveSession(remaining.at(index).sessionDirectory))
            {
                removed.insert(remaining.at(index).sessionDirectory);
                remaining.removeAt(index);
                return true;
            }
        }
        return false;
    };

    while (remaining.size() > policy.maxSessions && removeOldest())
    {
    }

    totalBytes = 0;
    for (const RecoveryCandidate& candidate : remaining)
    {
        for (const QFileInfo& file : QDir(candidate.sessionDirectory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
        {
            totalBytes += file.size();
        }
    }
    while (totalBytes > policy.maxBytes && removeOldest())
    {
        totalBytes = 0;
        for (const RecoveryCandidate& candidate : remaining)
        {
            for (const QFileInfo& file : QDir(candidate.sessionDirectory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
            {
                totalBytes += file.size();
            }
        }
    }
}

PDFRecoveryManager::CheckpointResult writeCheckpoint(const CheckpointRequest& request)
{
    PDFRecoveryManager::CheckpointResult result;
    result.sessionDirectory = request.sessionDirectory;
    result.revision = request.revision;

    if (!request.document)
    {
        result.diagnosticCode = QStringLiteral("missing-document");
        result.message = QStringLiteral("No document snapshot was available for recovery.");
        return result;
    }

    const pdf::PDFSecurityHandler* securityHandler = request.document->getStorage().getSecurityHandler();
    if (securityHandler && securityHandler->getMode() != pdf::EncryptionMode::None)
    {
        result.diagnosticCode = QStringLiteral("encrypted-document");
        result.message = QStringLiteral("Recovery is disabled for encrypted documents until an encrypted checkpoint can be preserved safely.");
        return result;
    }

    QDir sessionDirectory(request.sessionDirectory);
    if (!sessionDirectory.mkpath(QStringLiteral(".")))
    {
        result.diagnosticCode = QStringLiteral("recovery-directory-unavailable");
        result.message = QStringLiteral("The private recovery directory could not be created.");
        return result;
    }

    const QString partialPdf = generationPath(request.sessionDirectory, false, false) + QStringLiteral(".partial");
    const QString partialManifest = generationPath(request.sessionDirectory, false, true) + QStringLiteral(".partial");
    QFile::remove(partialPdf);
    QFile::remove(partialManifest);

    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult writeResult = writer.write(partialPdf, request.document.data(), true);
    if (!writeResult)
    {
        result.diagnosticCode = QStringLiteral("checkpoint-serialization-failed");
        result.message = QStringLiteral("The document snapshot could not be serialized.");
        return result;
    }

    const QString digest = hashFile(partialPdf);
    if (digest.isEmpty())
    {
        result.diagnosticCode = QStringLiteral("checkpoint-hash-failed");
        result.message = QStringLiteral("The checkpoint payload could not be verified.");
        return result;
    }

    QJsonObject source = identityToJson(request.sourceIdentity);
    source.insert(QStringLiteral("path"), request.sourcePath);

    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), RecoverySchema);
    manifest.insert(QStringLiteral("sessionId"), request.sessionId);
    manifest.insert(QStringLiteral("source"), source);
    manifest.insert(QStringLiteral("documentRevision"), QString::number(request.revision));
    manifest.insert(QStringLiteral("checkpointUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("cleanlySaved"), false);
    manifest.insert(QStringLiteral("signedDocument"), request.signedDocument);
    manifest.insert(QStringLiteral("appVersion"), QCoreApplication::applicationVersion());
    manifest.insert(QStringLiteral("recoverySha256"), digest);

    const pdf::PDFOperationResult manifestResult = pdf::PDFSafeFileWriter::writeData(
        partialManifest,
        QJsonDocument(manifest).toJson(QJsonDocument::Compact),
        pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite);
    if (!manifestResult)
    {
        result.diagnosticCode = QStringLiteral("manifest-write-failed");
        result.message = QStringLiteral("The recovery manifest could not be committed.");
        return result;
    }

    const QString finalPdf = generationPath(request.sessionDirectory, false, false);
    const QString previousPdf = generationPath(request.sessionDirectory, true, false);
    const QString finalManifest = generationPath(request.sessionDirectory, false, true);
    const QString previousManifest = generationPath(request.sessionDirectory, true, true);
    if (!replaceGeneration(finalPdf, previousPdf, partialPdf)
        || !replaceGeneration(finalManifest, previousManifest, partialManifest))
    {
        result.diagnosticCode = QStringLiteral("checkpoint-commit-failed");
        result.message = QStringLiteral("The recovery checkpoint could not be committed atomically; the previous generation was retained when possible.");
        return result;
    }

    result.success = true;
    return result;
}

} // namespace

PDFRecoveryManager::PDFRecoveryManager(QObject* parent) :
    QObject(parent),
    m_debounceTimer(new QTimer(this)),
    m_intervalTimer(new QTimer(this)),
    m_checkpointWatcher(new QFutureWatcher<CheckpointResult>(this)),
    m_retentionWatcher(new QFutureWatcher<void>(this)),
    m_lockFile(nullptr)
{
    m_debounceTimer->setSingleShot(true);
    m_intervalTimer->setSingleShot(true);
    connect(m_debounceTimer, &QTimer::timeout, this, &PDFRecoveryManager::onDebounceTimeout);
    connect(m_intervalTimer, &QTimer::timeout, this, &PDFRecoveryManager::onIntervalTimeout);
    connect(m_checkpointWatcher, &QFutureWatcher<CheckpointResult>::finished, this, &PDFRecoveryManager::onCheckpointFinished);
}

PDFRecoveryManager::~PDFRecoveryManager()
{
    if (m_checkpointWatcher->isRunning())
    {
        m_checkpointWatcher->waitForFinished();
    }
    if (m_retentionWatcher->isRunning())
    {
        m_retentionWatcher->waitForFinished();
    }
    releaseSession(false);
}

void PDFRecoveryManager::setPolicy(const RecoveryPolicy& policy)
{
    m_policy.intervalSeconds = qMax(1, policy.intervalSeconds);
    m_policy.debounceSeconds = qMax(0, policy.debounceSeconds);
    m_policy.maxBytes = qMax<qint64>(1, policy.maxBytes);
    m_policy.maxSessions = qMax(1, policy.maxSessions);
    m_policy.maxAgeDays = qMax(1, policy.maxAgeDays);
}

QString PDFRecoveryManager::recoveryRoot() const
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty())
    {
        root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    return QDir(root).filePath(QStringLiteral("recovery"));
}

RecoverySourceIdentity PDFRecoveryManager::inspectSource(const QString& sourcePath)
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

RecoverySourceStatus PDFRecoveryManager::classifySource(const RecoverySourceIdentity& expected,
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

QList<RecoveryCandidate> PDFRecoveryManager::scan() const
{
    QList<RecoveryCandidate> candidates;
    const QDir root(recoveryRoot());
    if (!root.exists())
    {
        return candidates;
    }

    const QFileInfoList sessions = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo& sessionInfo : sessions)
    {
        const QString sessionDirectory = sessionInfo.absoluteFilePath();
        RecoveryCandidate candidate;
        if (!readGeneration(sessionDirectory, false, &candidate)
            && !readGeneration(sessionDirectory, true, &candidate))
        {
            if (QFileInfo::exists(generationPath(sessionDirectory, false, true)))
            {
                candidate.sessionDirectory = sessionDirectory;
                candidate.valid = false;
                candidate.sourceStatus = RecoverySourceStatus::Invalid;
                candidate.diagnosticCode = QStringLiteral("corrupt-recovery-artifact");
                candidate.diagnosticMessage = QStringLiteral("The recovery artifact is incomplete or failed integrity validation.");
                candidates.push_back(candidate);
            }
            continue;
        }

        const QFileInfo sourceInfo(candidate.sourcePath);
        const RecoverySourceIdentity actual = inspectSource(candidate.sourcePath);
        candidate.sourceStatus = classifySource(candidate.sourceIdentity, actual, sourceInfo.exists());

        QLockFile lock(QDir(sessionDirectory).filePath(QStringLiteral("lock")));
        bool active = !lock.tryLock(0);
        if (active && lock.removeStaleLockFile())
        {
            active = !lock.tryLock(0);
        }
        if (!active)
        {
            lock.unlock();
        }
        else
        {
            candidate.sourceStatus = RecoverySourceStatus::Active;
        }
        candidates.push_back(qMove(candidate));
    }

    std::sort(candidates.begin(), candidates.end(), [](const RecoveryCandidate& left, const RecoveryCandidate& right) {
        return left.checkpointUtc > right.checkpointUtc;
    });
    return candidates;
}

void PDFRecoveryManager::attach(const QString& sourcePath,
                                pdf::PDFDocumentPointer document,
                                quint64 revision,
                                bool currentSaved,
                                bool signedDocument)
{
    const bool hadInFlightCheckpoint = m_checkpointInFlight;
    if (m_checkpointInFlight)
    {
        if (!m_sessionDirectory.isEmpty())
        {
            m_retireDirectories.append(m_sessionDirectory);
        }
        m_retireAfterWrite = true;
        if (m_lockFile)
        {
            m_lockFile->unlock();
            delete m_lockFile;
            m_lockFile = nullptr;
        }
        m_sessionDirectory.clear();
        m_sessionId.clear();
    }
    else
    {
        releaseSession(false);
    }

    m_sourcePath = QFileInfo(sourcePath).absoluteFilePath();
    m_sourceIdentity = inspectSource(m_sourcePath);
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_documentRevision = revision;
    m_signedDocument = signedDocument;
    m_dirty = !currentSaved;
    m_pendingDocument = qMove(document);
    m_pendingRevision = revision;

    if (m_dirty)
    {
        ensureSession();
        if (m_sessionDirectory.isEmpty())
        {
            m_pendingDocument.reset();
            m_dirty = false;
            return;
        }
        m_debounceTimer->start(m_policy.debounceSeconds * 1000);
        m_intervalTimer->start(m_policy.intervalSeconds * 1000);
    }
    else
    {
        m_pendingDocument.reset();
    }

    if (hadInFlightCheckpoint)
    {
        m_sourcePathAfterRetire = m_sourcePath;
        m_sourceIdentityAfterRetire = m_sourceIdentity;
        m_sessionIdAfterRetire = m_sessionId;
        m_signedDocumentAfterRetire = m_signedDocument;
    }
}

void PDFRecoveryManager::markDirty(pdf::PDFDocumentPointer document, quint64 revision)
{
    if (!document || m_sourcePath.isEmpty())
    {
        return;
    }

    m_dirty = true;
    m_documentRevision = qMax(m_documentRevision, revision);
    m_pendingDocument = qMove(document);
    m_pendingRevision = revision;
    ensureSession();
    if (m_sessionDirectory.isEmpty())
    {
        return;
    }

    if (!m_debounceTimer->isActive())
    {
        m_debounceTimer->start(m_policy.debounceSeconds * 1000);
    }
    if (!m_intervalTimer->isActive())
    {
        m_intervalTimer->start(m_policy.intervalSeconds * 1000);
    }
}

void PDFRecoveryManager::markSaved(const QString& savedPath, quint64 revision)
{
    const QString nextSourcePath = savedPath.isEmpty() ? m_sourcePath : QFileInfo(savedPath).absoluteFilePath();
    const bool signedDocument = m_signedDocument;
    m_documentRevision = revision;
    m_dirty = false;
    m_pendingDocument.reset();
    m_pendingRevision = 0;
    m_debounceTimer->stop();
    m_intervalTimer->stop();
    if (m_checkpointInFlight)
    {
        m_retireAfterWrite = true;
        m_sourcePathAfterRetire = nextSourcePath;
        m_sourceIdentityAfterRetire = inspectSource(nextSourcePath);
        m_sessionIdAfterRetire = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_signedDocumentAfterRetire = signedDocument;
    }
    else
    {
        releaseSession(true);
        m_sourcePath = nextSourcePath;
        m_sourceIdentity = inspectSource(m_sourcePath);
        m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_signedDocument = signedDocument;
    }
}

void PDFRecoveryManager::discardSession()
{
    m_dirty = false;
    m_pendingDocument.reset();
    m_pendingRevision = 0;
    m_debounceTimer->stop();
    m_intervalTimer->stop();
    if (m_checkpointInFlight)
    {
        m_retireAfterWrite = true;
        m_sourcePathAfterRetire.clear();
        m_sourceIdentityAfterRetire = RecoverySourceIdentity();
        m_sessionIdAfterRetire.clear();
        m_signedDocumentAfterRetire = false;
    }
    else
    {
        releaseSession(true);
    }
}

bool PDFRecoveryManager::restoreCandidate(const RecoveryCandidate& candidate,
                                          pdf::PDFDocumentPointer* document,
                                          QString* errorMessage)
{
    if (!document || !candidate.valid || candidate.sourceStatus == RecoverySourceStatus::Active
        || !safeSessionDirectory(recoveryRoot(), candidate.sessionDirectory))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("The selected recovery session is not available for safe restore.");
        }
        return false;
    }

    RecoveryCandidate validated;
    const bool isPrevious = candidate.recoveryFile.endsWith(RecoveryPdfPreviousName);
    if (!readGeneration(candidate.sessionDirectory, isPrevious, &validated))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("The selected recovery snapshot failed integrity validation.");
        }
        return false;
    }

    std::unique_ptr<QLockFile> lock(new QLockFile(QDir(candidate.sessionDirectory).filePath(QStringLiteral("lock"))));
    if (!lock->tryLock(0) && (!lock->removeStaleLockFile() || !lock->tryLock(0)))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Another Loupe instance currently owns this recovery session.");
        }
        return false;
    }

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument recovered = reader.readFromFile(validated.recoveryFile);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        lock->unlock();
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("The recovered PDF could not be opened safely.");
        }
        return false;
    }

    if (m_checkpointInFlight)
    {
        if (!m_sessionDirectory.isEmpty())
        {
            m_retireDirectories.append(m_sessionDirectory);
        }
        m_retireAfterWrite = true;
        if (m_lockFile)
        {
            m_lockFile->unlock();
            delete m_lockFile;
            m_lockFile = nullptr;
        }
        m_sessionDirectory.clear();
        m_sessionId.clear();
        m_sourcePathAfterRetire = validated.sourcePath;
        m_sourceIdentityAfterRetire = validated.sourceIdentity;
        m_sessionIdAfterRetire = validated.sessionId;
        m_signedDocumentAfterRetire = false;
    }
    else
    {
        releaseSession(false);
    }

    m_lockFile = lock.release();
    m_sessionId = validated.sessionId;
    m_sessionDirectory = validated.sessionDirectory;
    m_sourcePath = validated.sourcePath;
    m_sourceIdentity = validated.sourceIdentity;
    m_documentRevision = validated.documentRevision;
    m_signedDocument = validated.signedDocument;
    m_dirty = true;
    m_pendingDocument.reset();
    m_pendingRevision = 0;
    *document = pdf::PDFDocumentPointer(new pdf::PDFDocument(qMove(recovered)));
    return true;
}

void PDFRecoveryManager::discardCandidate(const RecoveryCandidate& candidate)
{
    if (safeSessionDirectory(recoveryRoot(), candidate.sessionDirectory))
    {
        QDir(candidate.sessionDirectory).removeRecursively();
    }
}

void PDFRecoveryManager::ensureSession()
{
    if (!m_sessionDirectory.isEmpty())
    {
        return;
    }

    if (m_sourcePath.isEmpty() || recoveryRoot().isEmpty())
    {
        checkpointFailed(QStringLiteral("recovery-source-unavailable"), QStringLiteral("Recovery is unavailable because the document has no local source identity."));
        return;
    }

    m_sessionDirectory = QDir(recoveryRoot()).filePath(m_sessionId);
    if (!QDir().mkpath(m_sessionDirectory))
    {
        m_sessionDirectory.clear();
        checkpointFailed(QStringLiteral("recovery-directory-unavailable"), QStringLiteral("The private recovery directory could not be created."));
        return;
    }

    m_lockFile = new QLockFile(QDir(m_sessionDirectory).filePath(QStringLiteral("lock")));
    if (!m_lockFile->tryLock(0) && (!m_lockFile->removeStaleLockFile() || !m_lockFile->tryLock(0)))
    {
        delete m_lockFile;
        m_lockFile = nullptr;
        m_sessionDirectory.clear();
        checkpointFailed(QStringLiteral("recovery-session-locked"), QStringLiteral("Another Loupe instance owns the recovery session."));
    }
}

void PDFRecoveryManager::startCheckpoint()
{
    if (!m_dirty || m_checkpointInFlight || !m_pendingDocument)
    {
        return;
    }
    ensureSession();
    if (m_sessionDirectory.isEmpty())
    {
        return;
    }

    CheckpointRequest request;
    request.sessionDirectory = m_sessionDirectory;
    request.sessionId = m_sessionId;
    request.sourcePath = m_sourcePath;
    request.sourceIdentity = m_sourceIdentity;
    request.document = m_pendingDocument;
    request.revision = m_pendingRevision;
    request.signedDocument = m_signedDocument;
    m_pendingDocument.reset();
    m_pendingRevision = 0;
    m_checkpointInFlight = true;
    m_checkpointWatcher->setFuture(QtConcurrent::run(writeCheckpoint, request));
}

void PDFRecoveryManager::releaseSession(bool removeFiles)
{
    m_debounceTimer->stop();
    m_intervalTimer->stop();
    m_pendingDocument.reset();
    m_pendingRevision = 0;
    if (m_lockFile)
    {
        m_lockFile->unlock();
        delete m_lockFile;
        m_lockFile = nullptr;
    }
    if (removeFiles && !m_sessionDirectory.isEmpty())
    {
        QDir(m_sessionDirectory).removeRecursively();
    }
    m_sessionDirectory.clear();
    m_sessionId.clear();
    m_sourcePath.clear();
    m_sourceIdentity = RecoverySourceIdentity();
    m_signedDocument = false;
    m_retireAfterWrite = false;
}

void PDFRecoveryManager::retireSessionDirectory(const QString& directory)
{
    if (safeSessionDirectory(recoveryRoot(), directory))
    {
        QDir(directory).removeRecursively();
    }
}

void PDFRecoveryManager::enforceRetention()
{
    if (m_retentionWatcher->isRunning())
    {
        return;
    }

    const RecoveryPolicy policy = m_policy;
    const QList<RecoveryCandidate> candidates = scan();
    m_retentionWatcher->setFuture(QtConcurrent::run(enforceRetentionInWorker, policy, candidates));
}

void PDFRecoveryManager::onDebounceTimeout()
{
    startCheckpoint();
}

void PDFRecoveryManager::onIntervalTimeout()
{
    startCheckpoint();
    if (m_dirty)
    {
        m_intervalTimer->start(m_policy.intervalSeconds * 1000);
    }
}

void PDFRecoveryManager::onCheckpointFinished()
{
    const CheckpointResult result = m_checkpointWatcher->result();
    m_checkpointInFlight = false;

    const bool retire = m_retireAfterWrite || m_retireDirectories.contains(result.sessionDirectory);
    m_retireDirectories.removeAll(result.sessionDirectory);

    if (result.success)
    {
        checkpointCompleted(result.revision);
    }
    else
    {
        checkpointFailed(result.diagnosticCode, result.message);
    }

    if (retire)
    {
        const QString sourcePathAfterRetire = m_sourcePathAfterRetire;
        const RecoverySourceIdentity sourceIdentityAfterRetire = m_sourceIdentityAfterRetire;
        const QString sessionIdAfterRetire = m_sessionIdAfterRetire;
        const bool signedDocumentAfterRetire = m_signedDocumentAfterRetire;
        retireSessionDirectory(result.sessionDirectory);
        if (result.sessionDirectory == m_sessionDirectory)
        {
            releaseSession(false);
        }
        if (!sourcePathAfterRetire.isEmpty())
        {
            m_sourcePath = sourcePathAfterRetire;
            m_sourceIdentity = sourceIdentityAfterRetire;
            m_sessionId = sessionIdAfterRetire;
            m_signedDocument = signedDocumentAfterRetire;
        }
        m_sourcePathAfterRetire.clear();
        m_sourceIdentityAfterRetire = RecoverySourceIdentity();
        m_sessionIdAfterRetire.clear();
        m_signedDocumentAfterRetire = false;
        m_retireAfterWrite = false;
    }
    else if (m_pendingDocument && m_dirty)
    {
        startCheckpoint();
    }

    enforceRetention();
}

} // namespace pdfviewer
