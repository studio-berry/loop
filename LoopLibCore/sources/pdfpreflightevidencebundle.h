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

#ifndef PDFPREFLIGHTEVIDENCEBUNDLE_H
#define PDFPREFLIGHTEVIDENCEBUNDLE_H

#include "pdfglobal.h"
#include "pdfgovernedexecution.h"
#include "pdfoperationhistory.h"
#include "pdfpreflightcertificate.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace pdf
{

/// Bundle format constants. The bundle is a portable export of one certified
/// preflight act; the internal certified state, the operation-history chain and
/// the document sidecar remain canonical.
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundleSchemaKind();
LOOPLIBCORESHARED_EXPORT int preflightEvidenceBundleSchemaVersion();
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundleManifestMember();
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundleReportMember();
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundleCertificateMember();
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundleSignOffMember();
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundleHistoryMember();
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundleRollbackMember();

/// One declared bundle member. The name is a sanitized logical file name inside
/// the bundle directory; it is never derived from a caller's path.
struct LOOPLIBCORESHARED_EXPORT PreflightEvidenceBundleMember
{
    QString name;
    QString mediaType = QStringLiteral("application/json");
    QByteArray content;

    QJsonObject toManifestEntry() const;
};

/// Portable output identity of a corrected document, when a correction is part
/// of the handoff. Only digests and sizes are retained; the artifact itself is
/// not copied into the bundle.
struct LOOPLIBCORESHARED_EXPORT PreflightEvidenceBundleOutput
{
    QString sha256;
    qint64 byteCount = -1;

    bool isValid() const { return isPDFSha256(sha256); }
    QJsonObject toJson() const;
};

/// Everything the caller must supply to build a bundle. All identity-bearing
/// inputs come from the canonical Core structures, so the bundle cannot invent
/// a second state.
struct LOOPLIBCORESHARED_EXPORT PreflightEvidenceBundleRequest
{
    /// Exact bytes of the revision the bundle describes. Only the digest is
    /// retained in the bundle.
    QByteArray documentBytes;
    /// Preflight report for that revision. The source path is removed from the
    /// exported copy.
    QJsonObject report;
    /// Effective-profile identity record, as reported by the resolver.
    QJsonObject profileIdentity;
    /// Effective-profile resolution provenance, as reported by the resolver.
    QJsonObject profileResolution;
    /// Coverage scope actually evaluated by the run.
    QJsonObject coverageScope;
    /// Decisions (accept/waive/override) carried by the report.
    QList<PreflightDecision> decisions;
    /// Retained certificate, when the revision is certified.
    std::optional<PreflightCertificate> certificate;
    /// Publication sign-off record (`loop.governed-sign-off`), when a
    /// correction was published.
    QJsonObject signOff;
    /// Corrected output identity, when a correction is included.
    std::optional<PreflightEvidenceBundleOutput> output;
    /// Operation-history slice for the revision, in sequence order.
    QList<PDFOperationHistoryEvent> history;
    /// Retained rollback points. Paths are reduced to content digests.
    QList<PDFRollbackPoint> rollbackPoints;
    QString producer;
    QString producerVersion;
    QDateTime createdAtUtc;
};

struct LOOPLIBCORESHARED_EXPORT PreflightEvidenceBundle
{
    QJsonObject manifest;
    /// Declared members, excluding the manifest itself.
    QList<PreflightEvidenceBundleMember> members;

    QByteArray manifestBytes() const;
    const PreflightEvidenceBundleMember* findMember(const QString& name) const;
};

/// Builds the bundle manifest and members from canonical Core state. Fails
/// closed when the supplied identities disagree, when a digest is malformed, or
/// when the history chain does not verify.
LOOPLIBCORESHARED_EXPORT bool buildPreflightEvidenceBundle(const PreflightEvidenceBundleRequest& request,
                                                           PreflightEvidenceBundle& bundle,
                                                           QString& errorMessage);

/// Writes the bundle members and commits `manifest.json` last, so a directory
/// that contains a manifest is a complete bundle. An existing directory is
/// refused unless it is empty, so undeclared files can never be mistaken for
/// bundle members.
LOOPLIBCORESHARED_EXPORT bool writePreflightEvidenceBundle(const PreflightEvidenceBundle& bundle,
                                                           const QString& directory,
                                                           QString& errorMessage);

struct LOOPLIBCORESHARED_EXPORT PreflightEvidenceBundleFinding
{
    QString code;
    QString member;
    QString message;

    QJsonObject toJson() const;
};

struct LOOPLIBCORESHARED_EXPORT PreflightEvidenceBundleVerification
{
    bool valid = false;
    QString summary;
    int membersChecked = 0;
    /// True when the certificate's report binding can be re-derived from the
    /// bundle alone. It cannot be when the exported report omits the source
    /// path that the certificate hashed.
    bool reportBindingRecomputable = false;
    QList<PreflightEvidenceBundleFinding> findings;

    QJsonObject toJson() const;
};

/// Verifies one bundle directory offline: member integrity against the
/// manifest, exact member set, path hygiene, identity agreement between the
/// manifest, the report, the certificate, the sign-off and the history slice,
/// and the hash chain of the exported history slice.
LOOPLIBCORESHARED_EXPORT PreflightEvidenceBundleVerification verifyPreflightEvidenceBundle(
    const QString& directory,
    QString* errorMessage = nullptr);

/// Replaces every absolute-path-shaped substring in free-form operator text
/// with the bundle's path placeholder. Used by the exporter so no member can
/// carry a private path.
LOOPLIBCORESHARED_EXPORT QString redactBundlePaths(const QString& value);

/// The placeholder that replaces a redacted path.
LOOPLIBCORESHARED_EXPORT QString preflightEvidenceBundlePathPlaceholder();

}   // namespace pdf

#endif   // PDFPREFLIGHTEVIDENCEBUNDLE_H
