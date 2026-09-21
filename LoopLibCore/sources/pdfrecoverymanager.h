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

#include "pdfglobal.h"

#include <QDateTime>
#include <QString>

namespace pdf
{

/// Retention and cadence limits for the crash-recovery service. The values are
/// the contract in docs/EDITOR_RECOVERY.md; clampRecoveryPolicy() is the only
/// place a caller-supplied policy becomes usable.
struct RecoveryPolicy
{
    int intervalSeconds = 30;
    int debounceSeconds = 3;
    qint64 maxBytes = 2LL * 1024 * 1024 * 1024;
    int maxSessions = 20;
    int maxAgeDays = 14;
};

/// The identity of a source document as recorded in a recovery manifest: a hash
/// of its normalized path plus a bounded fingerprint of its bytes, its size and
/// its modification time. It never carries the path itself.
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

/// One startup restore offer. It carries no filesystem path: the raw source path
/// is retained only in the private recovery store, and the host resolves
/// sessionId through it. \p sourceFileName is for display, and diagnostics
/// travel as a bounded code the host translates rather than as free text.
struct LOOPLIBCORESHARED_EXPORT RecoveryCandidate
{
    QString sessionId;
    QString sourceFileName;
    QString checkpointUtc;
    QString diagnosticCode;
    RecoverySourceIdentity sourceIdentity;
    QString documentRevision;
    RecoverySourceStatus sourceStatus = RecoverySourceStatus::Invalid;
    bool valid = false;
    bool signedDocument = false;
};

/// Clamps every field to the range the recovery service can honor.
LOOPLIBCORESHARED_EXPORT RecoveryPolicy clampRecoveryPolicy(RecoveryPolicy policy);

/// Captures the source identity for a local document without retaining its content.
LOOPLIBCORESHARED_EXPORT RecoverySourceIdentity inspectRecoverySource(const QString& sourcePath);

/// Classifies a live source against the identity a checkpoint recorded for it.
LOOPLIBCORESHARED_EXPORT RecoverySourceStatus classifyRecoverySource(const RecoverySourceIdentity& expected,
                                                                     const RecoverySourceIdentity& actual,
                                                                     bool sourceExists);

}   // namespace pdf

#endif   // PDFRECOVERYMANAGER_H
