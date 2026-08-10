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

#ifndef PREFLIGHTPROFILERESOLVER_H
#define PREFLIGHTPROFILERESOLVER_H

#include "pdfglobal.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>

namespace pdf
{

inline constexpr int PREFLIGHT_PROFILE_RESOLVER_VERSION = 1;

/// Stable production identifiers used when selecting profile sources.
struct PDF4QTLIBCORESHARED_EXPORT PreflightJobContext
{
    QString clientId;
    QString productId;
    QString jobType;
    QString pressId;
    QString stockId;
    QString finishingId;
    QMap<QString, QJsonValue> attributes;

    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& object,
                         PreflightJobContext& context,
                         QString& errorMessage);
};

/// One immutable profile source from a profile-store snapshot.
struct PDF4QTLIBCORESHARED_EXPORT PreflightProfileSource
{
    QString id;
    QString version;
    QByteArray contentHash;
    QJsonObject profile;
    QJsonObject selector;
    int priority = 0;

    QJsonObject toJson() const;
};

/// Immutable source set used for one resolution. Files must be parsed before
/// a resolver sees the snapshot so filesystem changes cannot affect a run.
struct PDF4QTLIBCORESHARED_EXPORT PreflightProfileSnapshot
{
    QList<PreflightProfileSource> sources;
};

struct PDF4QTLIBCORESHARED_EXPORT PreflightResolutionDecision
{
    QString jsonPointer;
    QJsonValue value;
    QString sourceId;
    QString overriddenSourceId;
    QString reason;

    QJsonObject toJson() const;
};

/// Result of profile compilation. A failed result is deliberately not a
/// profile that can be passed to PreflightEngine.
struct PDF4QTLIBCORESHARED_EXPORT PreflightResolvedProfile
{
    bool ok = false;
    QString errorCode;
    QString errorMessage;
    QJsonObject effectiveProfile;
    QByteArray effectiveHash;
    QList<PreflightProfileSource> matchedSources;
    QList<PreflightResolutionDecision> decisions;
    QJsonObject normalizedContext;
    QString resolverVersion = QString::number(PREFLIGHT_PROFILE_RESOLVER_VERSION);
    QString resolutionMode = QStringLiteral("contextual");

    QJsonObject provenance() const;
};

/// Local, deterministic JSON profile store. It intentionally does not know
/// about UI, job databases, URLs, or network services.
class PDF4QTLIBCORESHARED_EXPORT PreflightProfileStore
{
public:
    static bool loadDirectory(const QString& directoryPath,
                              PreflightProfileSnapshot& snapshot,
                              QString& errorMessage);
};

/// Compiles normalized production context and profile sources into one policy.
class PDF4QTLIBCORESHARED_EXPORT PreflightProfileResolver
{
public:
    PreflightResolvedProfile resolve(const PreflightJobContext& context,
                                     const PreflightProfileSnapshot& snapshot) const;

    /// Compatibility path. It bypasses contextual matching but emits the same
    /// identity and provenance shape as contextual resolution.
    PreflightResolvedProfile resolveExplicitProfile(const QJsonObject& profile,
                                                     const QString& sourceId = QStringLiteral("explicit"),
                                                     const QString& version = QStringLiteral("1")) const;
};

/// Recursively sorts JSON object keys and preserves array order.
PDF4QTLIBCORESHARED_EXPORT QJsonValue canonicalizePreflightJson(const QJsonValue& value);

/// Compact canonical JSON used for profile identities and provenance hashes.
PDF4QTLIBCORESHARED_EXPORT QByteArray canonicalPreflightJson(const QJsonValue& value);

} // namespace pdf

#endif // PREFLIGHTPROFILERESOLVER_H
