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

#ifndef PDFRECOVERYMANAGER_H
#define PDFRECOVERYMANAGER_H

#include "pdfdocument.h"
#include "pdfviewerglobal.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QStringList>

class QLockFile;
class QTimer;
template<typename T>
class QFutureWatcher;

namespace pdfviewer
{

struct RecoveryPolicy
{
    int intervalSeconds = 30;
    int debounceSeconds = 3;
    qint64 maxBytes = 2LL * 1024 * 1024 * 1024;
    int maxSessions = 20;
    int maxAgeDays = 14;
};

struct RecoverySourceIdentity
{
    QString pathHash;
    QString prefixDigest;
    qint64 size = -1;
    QDateTime modifiedUtc;

    bool isValid() const { return !pathHash.isEmpty() && size >= 0 && modifiedUtc.isValid() && !prefixDigest.isEmpty(); }
    bool operator==(const RecoverySourceIdentity&) const = default;
};

enum class RecoverySourceStatus
{
    Unchanged,
    Changed,
    Missing,
    Active,
    Invalid
};

struct PDF4QTLIBGUILIBSHARED_EXPORT RecoveryCandidate
{
    QString sessionId;
    QString sessionDirectory;
    QString recoveryFile;
    QString sourcePath;
    QString sourceFileName;
    QString checkpointUtc;
    QString diagnosticCode;
    QString diagnosticMessage;
    RecoverySourceIdentity sourceIdentity;
    quint64 documentRevision = 0;
    RecoverySourceStatus sourceStatus = RecoverySourceStatus::Invalid;
    bool valid = false;
    bool cleanlySaved = false;
    bool signedDocument = false;
};

class PDF4QTLIBGUILIBSHARED_EXPORT PDFRecoveryManager final : public QObject
{
    Q_OBJECT

public:
    explicit PDFRecoveryManager(QObject* parent = nullptr);
    ~PDFRecoveryManager() override;

    const RecoveryPolicy& policy() const { return m_policy; }
    void setPolicy(const RecoveryPolicy& policy);

    QString recoveryRoot() const;
    QList<RecoveryCandidate> scan() const;

    /// Captures the source identity for a local document without retaining its content.
    static RecoverySourceIdentity inspectSource(const QString& sourcePath);
    static RecoverySourceStatus classifySource(const RecoverySourceIdentity& expected,
                                                const RecoverySourceIdentity& actual,
                                                bool sourceExists);

    void attach(const QString& sourcePath,
                pdf::PDFDocumentPointer document,
                quint64 revision,
                bool currentSaved,
                bool signedDocument = false);
    void markDirty(pdf::PDFDocumentPointer document, quint64 revision);
    void markSaved(const QString& savedPath, quint64 revision);
    void discardSession();

    /// Reads a validated recovery snapshot and claims its session lock.
    bool restoreCandidate(const RecoveryCandidate& candidate,
                          pdf::PDFDocumentPointer* document,
                          QString* errorMessage);
    void discardCandidate(const RecoveryCandidate& candidate);

    struct CheckpointResult;

signals:
    void checkpointCompleted(quint64 revision);
    void checkpointFailed(QString diagnosticCode, QString message);

private slots:
    void onDebounceTimeout();
    void onIntervalTimeout();
    void onCheckpointFinished();

private:
    void ensureSession();
    void startCheckpoint();
    void releaseSession(bool removeFiles);
    void retireSessionDirectory(const QString& directory);
    void enforceRetention();

    RecoveryPolicy m_policy;
    QTimer* m_debounceTimer;
    QTimer* m_intervalTimer;
    QFutureWatcher<CheckpointResult>* m_checkpointWatcher;
    QFutureWatcher<void>* m_retentionWatcher;
    bool m_checkpointInFlight = false;
    bool m_dirty = false;
    bool m_signedDocument = false;
    bool m_retireAfterWrite = false;
    quint64 m_documentRevision = 0;
    pdf::PDFDocumentPointer m_pendingDocument;
    quint64 m_pendingRevision = 0;
    QString m_sourcePath;
    RecoverySourceIdentity m_sourceIdentity;
    QString m_sessionId;
    QString m_sessionDirectory;
    QLockFile* m_lockFile;
    QStringList m_retireDirectories;
    QString m_sourcePathAfterRetire;
    RecoverySourceIdentity m_sourceIdentityAfterRetire;
    QString m_sessionIdAfterRetire;
    bool m_signedDocumentAfterRetire = false;
};

} // namespace pdfviewer

#endif // PDFRECOVERYMANAGER_H
