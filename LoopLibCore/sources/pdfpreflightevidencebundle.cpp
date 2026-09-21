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

#include "pdfpreflightevidencebundle.h"

#include "pdfartifactidentity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace pdf
{

namespace
{

const QLatin1String BUNDLE_SCHEMA("loop.preflight-evidence-bundle");
const QLatin1String HISTORY_SCHEMA("loop.preflight-evidence-bundle-history");
const QLatin1String ROLLBACK_SCHEMA("loop.preflight-evidence-bundle-rollback");
const QLatin1String VERIFICATION_SCHEMA("loop.preflight-evidence-bundle-verification");
const QLatin1String PATH_PLACEHOLDER("<path-omitted>");
const QLatin1String SOURCE_PATH_KEY("pdf");

QString sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString canonicalDigest(const QJsonValue& value)
{
    return sha256Hex(canonicalJson(value));
}

QString dateTimeString(const QDateTime& value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

bool isSha256(const QString& value)
{
    return isPDFSha256(value);
}

/// Absolute-path-shaped substrings that must never leave the machine in a
/// bundle member. Windows drive paths, UNC shares and at least two-segment
/// POSIX paths qualify; URLs are deliberately left alone.
QRegularExpression pathPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral(R"((^|[\s"'(=\[{,:])((?:[A-Za-z]:[\\/]|\\\\[A-Za-z0-9._-]+[\\/]|/(?:[A-Za-z0-9._-]+/)+)[^\s"')<>|,\]]*))"),
        QRegularExpression::MultilineOption);
    return pattern;
}

QString redactPathsInString(const QString& value)
{
    if (value.isEmpty() ||
        (!value.contains(QLatin1Char('/')) && !value.contains(QLatin1Char('\\'))))
    {
        return value;
    }

    const QRegularExpression pattern = pathPattern();
    QString result;
    qsizetype consumed = 0;
    QRegularExpressionMatchIterator iterator = pattern.globalMatch(value);
    while (iterator.hasNext())
    {
        const QRegularExpressionMatch match = iterator.next();
        const qsizetype pathStart = match.capturedStart(2);
        const QString candidate = match.captured(2);
        // `https://host/path` is not a file path; leave the URL intact.
        if (match.captured(1) == QLatin1String(":") && candidate.startsWith(QLatin1String("//")))
        {
            continue;
        }

        result += value.mid(consumed, pathStart - consumed);
        result += QString(PATH_PLACEHOLDER);
        consumed = match.capturedEnd(2);
    }
    result += value.mid(consumed);
    return result;
}

QJsonValue sanitizeValue(const QJsonValue& value, bool dropSourcePath);

QJsonArray sanitizeArray(const QJsonArray& array, bool dropSourcePath)
{
    QJsonArray result;
    for (const QJsonValue& item : array)
    {
        result.append(sanitizeValue(item, dropSourcePath));
    }
    return result;
}

QJsonObject sanitizeObject(const QJsonObject& object, bool dropSourcePath)
{
    QJsonObject result;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        const QString key = it.key();
        if (dropSourcePath &&
            (key == SOURCE_PATH_KEY || key == QLatin1String("source_path") || key == QLatin1String("artifactPath")))
        {
            continue;
        }
        // `path` in a profile-resolution record is a JSON pointer into the
        // profile document, not a filesystem path; redacting it would destroy
        // the provenance it exists to carry.
        if (key == QLatin1String("path") && it.value().isString())
        {
            result.insert(key, it.value());
            continue;
        }
        result.insert(key, sanitizeValue(it.value(), dropSourcePath));
    }
    return result;
}

QJsonValue sanitizeValue(const QJsonValue& value, bool dropSourcePath)
{
    if (value.isObject())
    {
        return sanitizeObject(value.toObject(), dropSourcePath);
    }
    if (value.isArray())
    {
        return sanitizeArray(value.toArray(), dropSourcePath);
    }
    if (value.isString())
    {
        return redactPathsInString(value.toString());
    }
    return value;
}

/// Bundle members are redacted first (sensitive keys), then stripped of the
/// source-path key and of any path-shaped free-form text.
QJsonObject sanitizeReport(const QJsonObject& report)
{
    return sanitizeObject(redactSensitiveJson(report).toObject(), true);
}

/// Exported history removes the opaque artifact storage token: the bundle is
/// content-addressed by digest and carries no store layout.
QJsonObject sanitizeEventSummary(const QJsonObject& summary)
{
    return sanitizeObject(summary, true);
}

QJsonObject sanitizeArtifactIdentity(const QJsonObject& identity)
{
    QJsonObject result = sanitizeObject(identity, true);
    result.remove(QStringLiteral("storageToken"));
    return result;
}

PDFOperationHistoryEvent eventFromJson(const QJsonObject& object)
{
    PDFOperationHistoryEvent event;
    event.sequence = static_cast<qint64>(object.value(QStringLiteral("sequence")).toInteger(0));
    event.entryId = QUuid(object.value(QStringLiteral("entryId")).toString());
    event.executionId = QUuid(object.value(QStringLiteral("executionId")).toString());
    event.kind = pdfOperationHistoryEventKindFromString(object.value(QStringLiteral("kind")).toString());
    event.status = pdfOperationHistoryStatusFromString(object.value(QStringLiteral("status")).toString());
    event.operatorIdentity = object.value(QStringLiteral("operatorIdentity")).toString();
    event.documentRevisionDigest = object.value(QStringLiteral("documentRevisionDigest")).toString();
    event.effectiveProfileDigest = object.value(QStringLiteral("effectiveProfileDigest")).toString();
    event.resultSummary = object.value(QStringLiteral("result")).toObject();
    for (const QJsonValue& value : object.value(QStringLiteral("findingIds")).toArray())
    {
        event.findingIds.append(value.toString());
    }
    event.reportArtifactSha256 = object.value(QStringLiteral("reportArtifactSha256")).toString();
    event.diffArtifactSha256 = object.value(QStringLiteral("diffArtifactSha256")).toString();
    event.approval = PDFApprovalRecord::fromJson(object.value(QStringLiteral("approval")).toObject());
    event.previousEventHash = QByteArray::fromHex(object.value(QStringLiteral("previousEventHash")).toString().toLatin1());
    event.eventHash = QByteArray::fromHex(object.value(QStringLiteral("eventHash")).toString().toLatin1());
    event.createdUtc = QDateTime::fromString(object.value(QStringLiteral("createdUtc")).toString(), Qt::ISODateWithMs);
    if (object.contains(QStringLiteral("output")))
    {
        event.output = PDFArtifactIdentity::fromJson(object.value(QStringLiteral("output")).toObject());
    }
    return event;
}

/// Builds the exported copy of one event: every field is sanitized *before* the
/// hash links are recomputed, so the exported chain verifies against the bytes
/// that were actually written.
QJsonObject eventToBundleJson(const PDFOperationHistoryEvent& origin, const QByteArray& previousHash)
{
    PDFOperationHistoryEvent event = origin;
    event.resultSummary = sanitizeEventSummary(origin.resultSummary);
    event.operatorIdentity = redactPathsInString(origin.operatorIdentity);
    if (origin.output)
    {
        event.output = PDFArtifactIdentity::fromJson(sanitizeArtifactIdentity(origin.output->toJson()));
    }
    event.approval = PDFApprovalRecord::fromJson(sanitizeValue(origin.approval.toJson(), true).toObject());
    event.previousEventHash = previousHash;
    event.eventHash = computeOperationHistoryEventHash(event, previousHash);

    QJsonObject object{
        { QStringLiteral("sequence"), event.sequence },
        { QStringLiteral("entryId"), event.entryId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("executionId"), event.executionId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("kind"), pdfOperationHistoryEventKindToString(event.kind) },
        { QStringLiteral("status"), pdfOperationHistoryStatusToString(event.status) },
        { QStringLiteral("operatorIdentity"), event.operatorIdentity },
        { QStringLiteral("documentRevisionDigest"), event.documentRevisionDigest },
        { QStringLiteral("effectiveProfileDigest"), event.effectiveProfileDigest },
        { QStringLiteral("result"), event.resultSummary },
        { QStringLiteral("findingIds"), QJsonArray::fromStringList(event.findingIds) },
        { QStringLiteral("reportArtifactSha256"), event.reportArtifactSha256 },
        { QStringLiteral("diffArtifactSha256"), event.diffArtifactSha256 },
        { QStringLiteral("approval"), event.approval.toJson() },
        { QStringLiteral("previousEventHash"), QString::fromLatin1(event.previousEventHash.toHex()) },
        { QStringLiteral("eventHash"), QString::fromLatin1(event.eventHash.toHex()) },
        { QStringLiteral("createdUtc"), dateTimeString(event.createdUtc) }
    };
    if (event.output)
    {
        object.insert(QStringLiteral("output"), event.output->toJson());
    }
    return object;
}

/// The certificate hashes the redacted report it was issued over; this is the
/// same rule, so an exporter can prove the supplied certificate binds the
/// supplied report before it drops the source path from the exported copy.
QString reportBindingDigest(const QJsonObject& report)
{
    return canonicalDigest(redactSensitiveJson(report));
}

QString rollbackArtifactDigest(const PDFRollbackPoint& point)
{
    const QString fileName = QFileInfo(point.artifactPath).fileName().toLower();
    return isSha256(fileName) ? fileName : QString();
}

QJsonObject rollbackReferenceToJson(const PDFRollbackPoint& point)
{
    QJsonObject object{
        { QStringLiteral("rollback_id"), point.rollbackId },
        { QStringLiteral("audit_event_id"), point.auditEventId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("document_revision_digest"), point.documentRevisionDigest },
        { QStringLiteral("created_at_utc"), dateTimeString(point.createdAtUtc) },
        { QStringLiteral("artifact_sha256"), rollbackArtifactDigest(point) },
        { QStringLiteral("artifact_bytes"), point.artifactBytes },
        { QStringLiteral("operation_id"), point.operationId },
        { QStringLiteral("plan_summary"), redactPathsInString(point.planSummary) },
        { QStringLiteral("is_original_input"), point.isOriginalInput },
        { QStringLiteral("approved_output"), point.approvedOutput },
        { QStringLiteral("artifact_evicted"), point.artifactEvicted }
    };
    return object;
}

QJsonObject approvalToJson(const PDFApprovalRecord& approval, qint64 sequence, const QUuid& entryId)
{
    return QJsonObject{
        { QStringLiteral("sequence"), sequence },
        { QStringLiteral("entry_id"), entryId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("kind"), pdfApprovalKindToString(approval.kind) },
        { QStringLiteral("actor_id"), redactPathsInString(approval.actorId) },
        { QStringLiteral("decision"), redactPathsInString(approval.decision) },
        { QStringLiteral("policy_id"), redactPathsInString(approval.policyId) },
        { QStringLiteral("rationale"), redactPathsInString(approval.rationale) },
        { QStringLiteral("evidence_sha256"), approval.evidenceSha256 },
        { QStringLiteral("decision_reference"), approval.decisionReference },
        { QStringLiteral("decided_utc"), dateTimeString(approval.decidedUtc) }
    };
}

QByteArray serialize(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

QJsonObject verificationFindingJson(const PreflightEvidenceBundleFinding& finding)
{
    return QJsonObject{
        { QStringLiteral("code"), finding.code },
        { QStringLiteral("member"), finding.member },
        { QStringLiteral("message"), finding.message }
    };
}

/// A refusal, not an aggregate: every finding clears the verdict, so the bundle
/// verifies only while no refusal has been recorded against it.
void recordFinding(PreflightEvidenceBundleVerification& verification,
                   PreflightEvidenceBundleFinding finding)
{
    verification.valid = false;
    verification.findings.append(finding);
}

bool readJsonObject(const QString& path, QJsonObject& object, QString& errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = parseError.errorString();
        return false;
    }
    object = document.object();
    return true;
}

QJsonObject memberEntry(const PreflightEvidenceBundleMember& member)
{
    return QJsonObject{
        { QStringLiteral("name"), member.name },
        { QStringLiteral("media_type"), member.mediaType },
        { QStringLiteral("sha256"), sha256Hex(member.content) },
        { QStringLiteral("byte_count"), member.content.size() }
    };
}

void addMember(PreflightEvidenceBundle& bundle, const QString& name, const QJsonObject& content)
{
    PreflightEvidenceBundleMember member;
    member.name = name;
    member.content = serialize(content);
    bundle.members.append(member);
}

QList<PDFOperationHistoryEvent> parseExportedHistory(const QJsonObject& historyMember)
{
    QList<PDFOperationHistoryEvent> events;
    for (const QJsonValue& value : historyMember.value(QStringLiteral("events")).toArray())
    {
        events.append(eventFromJson(value.toObject()));
    }
    return events;
}

QJsonObject historyMemberJson(const QList<PDFOperationHistoryEvent>& history, QString* sourceChainDigest)
{
    QJsonArray events;
    QByteArray previousHash;
    QStringList sourceChainEntries;
    for (const PDFOperationHistoryEvent& origin : history)
    {
        const QJsonObject exported = eventToBundleJson(origin, previousHash);
        events.append(exported);
        previousHash = QByteArray::fromHex(
            exported.value(QStringLiteral("eventHash")).toString().toLatin1());
        sourceChainEntries.append(QStringLiteral("%1:%2:%3")
                                      .arg(origin.sequence)
                                      .arg(origin.entryId.toString(QUuid::WithoutBraces),
                                           QString::fromLatin1(origin.eventHash.toHex())));
    }
    if (sourceChainDigest)
    {
        *sourceChainDigest = sha256Hex(sourceChainEntries.join(QLatin1Char('\n')).toUtf8());
    }

    return QJsonObject{
        { QStringLiteral("schema"), QString(HISTORY_SCHEMA) },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("chain_mode"), QStringLiteral("path-redacted") },
        { QStringLiteral("events"), events }
    };
}

bool verifyExportedChain(const QList<PDFOperationHistoryEvent>& events,
                         PreflightEvidenceBundleVerification& verification)
{
    bool verified = true;
    QByteArray previousHash;
    qint64 expectedSequence = 1;
    for (const PDFOperationHistoryEvent& event : events)
    {
        if (event.sequence != expectedSequence)
        {
            recordFinding(verification, { QStringLiteral("history.sequence-discontinuous"),
                                          preflightEvidenceBundleHistoryMember(),
                                          QStringLiteral("Event sequence %1 is out of order; %2 was expected.")
                                              .arg(event.sequence)
                                              .arg(expectedSequence) });
            verified = false;
            break;
        }
        const QByteArray expectedHash = computeOperationHistoryEventHash(event, previousHash);
        if (event.previousEventHash != previousHash || event.eventHash != expectedHash)
        {
            recordFinding(verification, { QStringLiteral("history.chain-broken"),
                                          preflightEvidenceBundleHistoryMember(),
                                          QStringLiteral("The exported history chain does not verify at sequence %1 (event %2).")
                                              .arg(event.sequence)
                                              .arg(event.entryId.toString(QUuid::WithoutBraces)) });
            verified = false;
            break;
        }
        previousHash = event.eventHash;
        ++expectedSequence;
    }
    return verified;
}

}   // namespace

QString preflightEvidenceBundleSchemaKind()
{
    return QString(BUNDLE_SCHEMA);
}

int preflightEvidenceBundleSchemaVersion()
{
    return 1;
}

QString preflightEvidenceBundleManifestMember()
{
    return QStringLiteral("manifest.json");
}

QString preflightEvidenceBundleReportMember()
{
    return QStringLiteral("report.json");
}

QString preflightEvidenceBundleCertificateMember()
{
    return QStringLiteral("certificate.json");
}

QString preflightEvidenceBundleSignOffMember()
{
    return QStringLiteral("signoff.json");
}

QString preflightEvidenceBundleHistoryMember()
{
    return QStringLiteral("history.json");
}

QString preflightEvidenceBundleRollbackMember()
{
    return QStringLiteral("rollback-references.json");
}

QString preflightEvidenceBundlePathPlaceholder()
{
    return QString(PATH_PLACEHOLDER);
}

QString redactBundlePaths(const QString& value)
{
    return redactPathsInString(value);
}

QJsonObject PreflightEvidenceBundleMember::toManifestEntry() const
{
    return memberEntry(*this);
}

QJsonObject PreflightEvidenceBundleOutput::toJson() const
{
    return QJsonObject{
        { QStringLiteral("sha256"), sha256 },
        { QStringLiteral("byte_count"), byteCount }
    };
}

QJsonObject PreflightEvidenceBundleFinding::toJson() const
{
    return verificationFindingJson(*this);
}

QJsonObject PreflightEvidenceBundleVerification::toJson() const
{
    QJsonArray findingArray;
    for (const PreflightEvidenceBundleFinding& finding : findings)
    {
        findingArray.append(finding.toJson());
    }
    return QJsonObject{
        { QStringLiteral("schema"), QString(VERIFICATION_SCHEMA) },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("valid"), valid },
        { QStringLiteral("summary"), summary },
        { QStringLiteral("members_checked"), membersChecked },
        { QStringLiteral("report_binding_recomputable"), reportBindingRecomputable },
        { QStringLiteral("findings"), findingArray }
    };
}

QByteArray PreflightEvidenceBundle::manifestBytes() const
{
    return serialize(manifest);
}

const PreflightEvidenceBundleMember* PreflightEvidenceBundle::findMember(const QString& name) const
{
    for (const PreflightEvidenceBundleMember& member : members)
    {
        if (member.name == name)
        {
            return &member;
        }
    }
    return nullptr;
}

bool buildPreflightEvidenceBundle(const PreflightEvidenceBundleRequest& request,
                                  PreflightEvidenceBundle& bundle,
                                  QString& errorMessage)
{
    bundle = {};
    errorMessage.clear();

    if (request.documentBytes.isEmpty())
    {
        errorMessage = QStringLiteral("The bundle requires the document revision bytes.");
        return false;
    }

    const QString documentDigest = sha256Hex(request.documentBytes);
    const QString reportedDigest = request.report.value(QStringLiteral("document_revision_digest")).toString().toLower();
    if (!isSha256(reportedDigest))
    {
        errorMessage = QStringLiteral("The report does not carry a document revision digest.");
        return false;
    }
    if (reportedDigest != documentDigest)
    {
        errorMessage = QStringLiteral("The report describes a different document revision than the bytes supplied.");
        return false;
    }

    const QString effectiveProfileDigest =
        request.report.value(QStringLiteral("effective_profile_digest")).toString().toLower();
    if (!isSha256(effectiveProfileDigest))
    {
        errorMessage = QStringLiteral("The report does not carry an effective profile digest.");
        return false;
    }

    const QJsonObject coverageScope = request.report.value(QStringLiteral("coverage_scope")).toObject();
    if (coverageScope.isEmpty())
    {
        errorMessage = QStringLiteral("The report does not declare the coverage scope that was evaluated.");
        return false;
    }
    if (!request.coverageScope.isEmpty() && request.coverageScope != coverageScope)
    {
        errorMessage = QStringLiteral("The supplied coverage scope disagrees with the report.");
        return false;
    }

    QJsonObject profileIdentity = request.profileIdentity;
    if (profileIdentity.isEmpty())
    {
        profileIdentity = request.report.value(QStringLiteral("profile_identity")).toObject();
    }
    QJsonObject profileResolution = request.profileResolution;
    if (profileResolution.isEmpty())
    {
        profileResolution = request.report.value(QStringLiteral("profile_resolution")).toObject();
    }

    // The exported history slice must be the complete chain from sequence 1,
    // otherwise the exported chain cannot be verified from its first link.
    if (request.history.isEmpty())
    {
        errorMessage = QStringLiteral("The bundle requires the operation-history slice for the revision.");
        return false;
    }
    QByteArray previousHash;
    qint64 expectedSequence = 1;
    for (const PDFOperationHistoryEvent& event : request.history)
    {
        if (event.sequence != expectedSequence)
        {
            errorMessage = QStringLiteral("The history slice must be the complete chain from sequence 1.");
            return false;
        }
        if (event.previousEventHash != previousHash ||
            event.eventHash != computeOperationHistoryEventHash(event, previousHash))
        {
            errorMessage = QStringLiteral("The supplied history chain does not verify at sequence %1.").arg(event.sequence);
            return false;
        }
        previousHash = event.eventHash;
        ++expectedSequence;
    }

    const QJsonObject sanitizedReport = sanitizeReport(request.report);

    QJsonObject certificateObject;
    QString reportBinding = QStringLiteral("not-applicable");
    if (request.certificate)
    {
        const PreflightCertificate& certificate = *request.certificate;
        if (certificate.documentRevisionDigest.compare(documentDigest, Qt::CaseInsensitive) != 0)
        {
            errorMessage = QStringLiteral("The certificate describes a different document revision.");
            return false;
        }
        if (certificate.effectiveProfileDigest.compare(effectiveProfileDigest, Qt::CaseInsensitive) != 0)
        {
            errorMessage = QStringLiteral("The certificate binds a different effective profile.");
            return false;
        }
        const QString boundDigest = reportBindingDigest(request.report);
        if (certificate.reportDigest.compare(boundDigest, Qt::CaseInsensitive) != 0)
        {
            errorMessage = QStringLiteral("The certificate does not bind the supplied preflight report.");
            return false;
        }
        reportBinding = canonicalDigest(sanitizedReport).compare(certificate.reportDigest, Qt::CaseInsensitive) == 0
                            ? QStringLiteral("exact")
                            : QStringLiteral("path-omitted");
        certificateObject = QJsonObject{
            { QStringLiteral("certificate_id"), certificate.certificateId },
            { QStringLiteral("issued_at_utc"), dateTimeString(certificate.issuedAtUtc) },
            { QStringLiteral("issued_by"), redactPathsInString(certificate.issuedBy) },
            { QStringLiteral("document_revision_digest"), certificate.documentRevisionDigest },
            { QStringLiteral("effective_profile_digest"), certificate.effectiveProfileDigest },
            { QStringLiteral("certificate_report_digest"), certificate.reportDigest },
            { QStringLiteral("audit_chain_head_event_id"), certificate.auditChainHeadEventId },
            { QStringLiteral("error_count"), certificate.errorCount },
            { QStringLiteral("waived_error_count"), certificate.waivedErrorCount },
            { QStringLiteral("covering_decision_ids"), QJsonArray::fromStringList(certificate.coveringDecisionIds) }
        };
    }

    QJsonObject signOffObject;
    if (!request.signOff.isEmpty())
    {
        const QJsonObject supplied = request.signOff;
        if (supplied.value(QStringLiteral("schema")).toString() != QLatin1String("loop.governed-sign-off"))
        {
            errorMessage = QStringLiteral("The supplied sign-off record is not a 'loop.governed-sign-off' document.");
            return false;
        }
        for (const QString& key : { QStringLiteral("plan_digest"),
                                    QStringLiteral("source_sha256"),
                                    QStringLiteral("candidate_sha256"),
                                    QStringLiteral("published_sha256"),
                                    QStringLiteral("revalidation_report_sha256"),
                                    QStringLiteral("effective_profile_digest") })
        {
            if (!isSha256(supplied.value(key).toString().toLower()))
            {
                errorMessage = QStringLiteral("The sign-off record does not carry a valid '%1'.").arg(key);
                return false;
            }
        }
        if (supplied.value(QStringLiteral("effective_profile_digest")).toString().compare(effectiveProfileDigest, Qt::CaseInsensitive) != 0)
        {
            errorMessage = QStringLiteral("The sign-off record binds a different effective profile.");
            return false;
        }
        if (supplied.value(QStringLiteral("source_sha256")).toString().compare(documentDigest, Qt::CaseInsensitive) != 0)
        {
            errorMessage = QStringLiteral("The sign-off record binds a different source revision.");
            return false;
        }

        signOffObject = sanitizeObject(supplied, true);
        signOffObject.insert(QStringLiteral("approval"),
                             sanitizeValue(PDFApprovalRecord::fromJson(
                                               supplied.value(QStringLiteral("approval")).toObject())
                                               .toJson(),
                                           true));
    }

    QJsonObject outputObject;
    if (request.output && request.output->isValid())
    {
        outputObject = request.output->toJson();
    }
    else if (!signOffObject.isEmpty() && isSha256(signOffObject.value(QStringLiteral("published_sha256")).toString()))
    {
        outputObject = PreflightEvidenceBundleOutput{
            signOffObject.value(QStringLiteral("published_sha256")).toString(), -1
        }
                           .toJson();
    }
    if (!outputObject.isEmpty() && !signOffObject.isEmpty())
    {
        const QString publishedDigest = signOffObject.value(QStringLiteral("published_sha256")).toString().toLower();
        if (outputObject.value(QStringLiteral("sha256")).toString().toLower() != publishedDigest)
        {
            errorMessage = QStringLiteral("The supplied output artifact is not the artifact the sign-off published.");
            return false;
        }
    }

    QJsonArray decisionsArray;
    for (const PreflightDecision& decision : request.decisions)
    {
        decisionsArray.append(sanitizeObject(decision.toJson(documentDigest, effectiveProfileDigest), true));
    }

    QJsonArray approvalArray;
    for (const PDFOperationHistoryEvent& event : request.history)
    {
        if (event.approval.kind != PDFApprovalKind::None)
        {
            approvalArray.append(approvalToJson(event.approval, event.sequence, event.entryId));
        }
    }
    if (!signOffObject.isEmpty() && !signOffObject.value(QStringLiteral("approval")).toObject().isEmpty())
    {
        approvalArray.append(approvalToJson(
            PDFApprovalRecord::fromJson(signOffObject.value(QStringLiteral("approval")).toObject()), 0, QUuid()));
    }

    QJsonArray rollbackArray;
    for (const PDFRollbackPoint& point : request.rollbackPoints)
    {
        rollbackArray.append(rollbackReferenceToJson(point));
    }

    QString sourceChainDigest;
    const QJsonObject historyMember = historyMemberJson(request.history, &sourceChainDigest);

    bundle.members.clear();
    addMember(bundle, preflightEvidenceBundleReportMember(), sanitizedReport);
    if (request.certificate)
    {
        addMember(bundle, preflightEvidenceBundleCertificateMember(), request.certificate->toJson());
    }
    if (!signOffObject.isEmpty())
    {
        addMember(bundle, preflightEvidenceBundleSignOffMember(), signOffObject);
    }
    addMember(bundle, preflightEvidenceBundleHistoryMember(), historyMember);
    addMember(bundle,
              preflightEvidenceBundleRollbackMember(),
              QJsonObject{
                  { QStringLiteral("schema"), QString(ROLLBACK_SCHEMA) },
                  { QStringLiteral("schema_version"), 1 },
                  { QStringLiteral("references"), rollbackArray } });

    QJsonArray memberArray;
    for (const PreflightEvidenceBundleMember& member : bundle.members)
    {
        memberArray.append(member.toManifestEntry());
    }

    QString producedBy = request.producer;
    if (producedBy.isEmpty())
    {
        producedBy = QStringLiteral("Loop");
    }
    const QString producerVersion = request.producerVersion.isEmpty()
                                        ? QCoreApplication::applicationVersion()
                                        : request.producerVersion;

    const PDFOperationHistoryEvent& headEvent = request.history.isEmpty() ? PDFOperationHistoryEvent{} : request.history.back();

    bundle.manifest = QJsonObject{
        { QStringLiteral("schema"), QString(BUNDLE_SCHEMA) },
        { QStringLiteral("schema_version"), preflightEvidenceBundleSchemaVersion() },
        { QStringLiteral("created_at_utc"),
          dateTimeString(request.createdAtUtc.isValid() ? request.createdAtUtc : QDateTime::currentDateTimeUtc()) },
        { QStringLiteral("produced_by"),
          QJsonObject{
              { QStringLiteral("tool"), producedBy },
              { QStringLiteral("version"), producerVersion } } },
        { QStringLiteral("authority"),
          QJsonObject{
              { QStringLiteral("canonical_state"), QStringLiteral("internal") },
              { QStringLiteral("statement"),
                QStringLiteral("Portable export only. The internal certified preflight state and the canonical "
                               "operation-history chain remain the source of truth.") } } },
        { QStringLiteral("document"),
          QJsonObject{
              { QStringLiteral("revision_digest"), documentDigest },
              { QStringLiteral("byte_count"), request.documentBytes.size() },
              { QStringLiteral("source_path_included"), false } } },
        { QStringLiteral("effective_profile"),
          QJsonObject{
              { QStringLiteral("digest"), effectiveProfileDigest },
              { QStringLiteral("identity"), sanitizeObject(profileIdentity, true) },
              { QStringLiteral("resolution"), sanitizeObject(profileResolution, true) } } },
        { QStringLiteral("coverage_scope"), sanitizeObject(coverageScope, true) },
        { QStringLiteral("report"),
          QJsonObject{
              { QStringLiteral("member"), preflightEvidenceBundleReportMember() },
              { QStringLiteral("member_sha256"),
                sha256Hex(bundle.findMember(preflightEvidenceBundleReportMember())->content) },
              { QStringLiteral("certificate_binding"), reportBinding } } },
        { QStringLiteral("history"),
          QJsonObject{
              { QStringLiteral("member"), preflightEvidenceBundleHistoryMember() },
              { QStringLiteral("event_count"), request.history.size() },
              { QStringLiteral("first_sequence"), request.history.isEmpty() ? 0 : request.history.first().sequence },
              { QStringLiteral("last_sequence"), headEvent.sequence },
              { QStringLiteral("head_event_id"), headEvent.entryId.toString(QUuid::WithoutBraces) },
              { QStringLiteral("head_event_hash"), QString::fromLatin1(headEvent.eventHash.toHex()) },
              { QStringLiteral("chain_mode"), QStringLiteral("path-redacted") },
              { QStringLiteral("canonical_chain_digest"), sourceChainDigest } } },
        { QStringLiteral("decisions"), decisionsArray },
        { QStringLiteral("approvals"), approvalArray },
        { QStringLiteral("rollback_references"), rollbackArray },
        { QStringLiteral("members"), memberArray }
    };
    if (!certificateObject.isEmpty())
    {
        bundle.manifest.insert(QStringLiteral("certificate"), certificateObject);
    }
    if (!signOffObject.isEmpty())
    {
        bundle.manifest.insert(QStringLiteral("sign_off"), signOffObject);
    }
    if (!outputObject.isEmpty())
    {
        bundle.manifest.insert(QStringLiteral("output"), outputObject);
    }

    return true;
}

bool writePreflightEvidenceBundle(const PreflightEvidenceBundle& bundle,
                                  const QString& directory,
                                  QString& errorMessage)
{
    errorMessage.clear();
    if (directory.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("A bundle output directory is required.");
        return false;
    }

    QDir target(directory);
    if (target.exists() && !target.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
    {
        errorMessage = QStringLiteral("The bundle output directory '%1' is not empty.").arg(directory);
        return false;
    }
    if (!target.exists() && !QDir().mkpath(directory))
    {
        errorMessage = QStringLiteral("Could not create the bundle output directory '%1'.").arg(directory);
        return false;
    }

    auto writeFile = [&](const QString& name, const QByteArray& content) -> bool
    {
        QSaveFile file(target.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        {
            errorMessage = QStringLiteral("Could not write bundle member '%1': %2").arg(name, file.errorString());
            file.cancelWriting();
            return false;
        }
        if (!file.commit())
        {
            errorMessage = QStringLiteral("Could not commit bundle member '%1': %2").arg(name, file.errorString());
            return false;
        }
        return true;
    };

    for (const PreflightEvidenceBundleMember& member : bundle.members)
    {
        if (!writeFile(member.name, member.content))
        {
            return false;
        }
    }

    // The manifest is committed last: a directory that carries a manifest is a
    // complete bundle.
    return writeFile(preflightEvidenceBundleManifestMember(), bundle.manifestBytes());
}

PreflightEvidenceBundleVerification verifyPreflightEvidenceBundle(const QString& directory,
                                                                  QString* errorMessage)
{
    PreflightEvidenceBundleVerification verification;
    verification.valid = true;
    if (errorMessage)
    {
        errorMessage->clear();
    }

    const QDir target(directory);
    QJsonObject manifest;
    QString readError;
    if (!readJsonObject(target.filePath(preflightEvidenceBundleManifestMember()), manifest, readError))
    {
        verification.summary = QStringLiteral("The bundle manifest could not be read: %1").arg(readError);
        recordFinding(verification, { QStringLiteral("bundle.manifest-missing"),
                                      preflightEvidenceBundleManifestMember(),
                                      verification.summary });
        if (errorMessage)
        {
            *errorMessage = verification.summary;
        }
        return verification;
    }

    if (manifest.value(QStringLiteral("schema")).toString() != QString(BUNDLE_SCHEMA) ||
        static_cast<int>(manifest.value(QStringLiteral("schema_version")).toInteger()) !=
            preflightEvidenceBundleSchemaVersion())
    {
        verification.summary = QStringLiteral("Unsupported bundle schema '%1' version %2.")
                                   .arg(manifest.value(QStringLiteral("schema")).toString())
                                   .arg(manifest.value(QStringLiteral("schema_version")).toInteger());
        recordFinding(verification, { QStringLiteral("bundle.schema-unsupported"),
                                      preflightEvidenceBundleManifestMember(),
                                      verification.summary });
        return verification;
    }

    const QJsonObject manifestDocument = manifest.value(QStringLiteral("document")).toObject();
    const QString documentDigest = manifestDocument.value(QStringLiteral("revision_digest")).toString().toLower();
    if (!isSha256(documentDigest))
    {
        recordFinding(verification, { QStringLiteral("bundle.document-digest-invalid"),
                                      preflightEvidenceBundleManifestMember(),
                                      QStringLiteral("The manifest does not declare a document revision digest.") });
    }

    // 1. Exact member set plus per-member integrity.
    QSet<QString> declared;
    for (const QJsonValue& value : manifest.value(QStringLiteral("members")).toArray())
    {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        declared.insert(name);
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
            name == QLatin1String(".."))
        {
            recordFinding(verification, { QStringLiteral("member.name-invalid"),
                                          name,
                                          QStringLiteral("The manifest declares an invalid member name.") });
            continue;
        }

        QFile file(target.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
        {
            recordFinding(verification, { QStringLiteral("member.missing"), name, file.errorString() });
            continue;
        }
        const QByteArray content = file.readAll();
        ++verification.membersChecked;
        const qint64 declaredBytes = entry.value(QStringLiteral("byte_count")).toInteger(-1);
        const QString declaredDigest = entry.value(QStringLiteral("sha256")).toString().toLower();
        if (declaredBytes >= 0 && declaredBytes != content.size())
        {
            recordFinding(verification, { QStringLiteral("member.size-mismatch"),
                                          name,
                                          QStringLiteral("Member '%1' is %2 bytes; the manifest declares %3.")
                                              .arg(name)
                                              .arg(content.size())
                                              .arg(declaredBytes) });
            continue;
        }
        const QString actualDigest = sha256Hex(content);
        if (declaredDigest.isEmpty() || declaredDigest != actualDigest)
        {
            recordFinding(verification, { QStringLiteral("member.digest-mismatch"),
                                          name,
                                          QStringLiteral("Member '%1' hashes to %2; the manifest declares %3.")
                                              .arg(name, actualDigest, declaredDigest) });
        }
    }

    const QStringList presentFiles =
        target.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& file : presentFiles)
    {
        if (file != QString(preflightEvidenceBundleManifestMember()) && !declared.contains(file))
        {
            recordFinding(verification, { QStringLiteral("member.undeclared"),
                                          file,
                                          QStringLiteral("The bundle carries '%1', which the manifest does not declare.")
                                              .arg(file) });
        }
    }

    // 2. The report member must be path-free and must describe the same
    //    revision, profile and coverage scope as the manifest.
    QJsonObject reportMember;
    if (!readJsonObject(target.filePath(preflightEvidenceBundleReportMember()), reportMember, readError))
    {
        recordFinding(verification, { QStringLiteral("member.invalid-json"),
                                      preflightEvidenceBundleReportMember(),
                                      readError });
    }
    else
    {
        if (reportMember.contains(SOURCE_PATH_KEY))
        {
            recordFinding(verification, { QStringLiteral("bundle.source-path-present"),
                                          preflightEvidenceBundleReportMember(),
                                          QStringLiteral("The exported report still carries the source path key '%1'.")
                                              .arg(SOURCE_PATH_KEY) });
        }
        const QString reportDocumentDigest =
            reportMember.value(QStringLiteral("document_revision_digest")).toString().toLower();
        if (!reportDocumentDigest.isEmpty() && reportDocumentDigest != documentDigest)
        {
            recordFinding(verification, { QStringLiteral("bundle.document-digest-mismatch"),
                                          preflightEvidenceBundleReportMember(),
                                          QStringLiteral("The report describes revision %1; the manifest declares %2.")
                                              .arg(reportDocumentDigest, documentDigest) });
        }

        const QJsonObject reportScope = reportMember.value(QStringLiteral("coverage_scope")).toObject();
        const QJsonObject manifestScope = manifest.value(QStringLiteral("coverage_scope")).toObject();
        if (manifestScope.isEmpty())
        {
            recordFinding(verification, { QStringLiteral("bundle.coverage-scope-missing"),
                                          preflightEvidenceBundleManifestMember(),
                                          QStringLiteral("The manifest does not carry the coverage scope.") });
        }
        else if (manifestScope != reportScope)
        {
            recordFinding(verification, { QStringLiteral("bundle.coverage-scope-mismatch"),
                                          preflightEvidenceBundleManifestMember(),
                                          QStringLiteral("The manifest coverage scope differs from the report's.") });
        }

        const QString reportProfileDigest =
            reportMember.value(QStringLiteral("effective_profile_digest")).toString().toLower();
        const QString manifestProfileDigest =
            manifest.value(QStringLiteral("effective_profile")).toObject().value(QStringLiteral("digest")).toString().toLower();
        if (!isSha256(manifestProfileDigest))
        {
            recordFinding(verification, { QStringLiteral("bundle.profile-digest-missing"),
                                          preflightEvidenceBundleManifestMember(),
                                          QStringLiteral("The manifest does not carry the effective profile digest.") });
        }
        else if (!reportProfileDigest.isEmpty() && reportProfileDigest != manifestProfileDigest)
        {
            recordFinding(verification, { QStringLiteral("bundle.profile-digest-mismatch"),
                                          preflightEvidenceBundleManifestMember(),
                                          QStringLiteral("The report binds profile %1; the manifest declares %2.")
                                              .arg(reportProfileDigest, manifestProfileDigest) });
        }

        // Decisions are carried in the manifest as well as in the report; both
        // copies must name the same decisions.
        QStringList reportDecisionIds;
        for (const QJsonValue& value : reportMember.value(QStringLiteral("decisions")).toArray())
        {
            reportDecisionIds.append(value.toObject().value(QStringLiteral("finding_id")).toString());
        }
        QStringList manifestDecisionIds;
        for (const QJsonValue& value : manifest.value(QStringLiteral("decisions")).toArray())
        {
            manifestDecisionIds.append(value.toObject().value(QStringLiteral("finding_id")).toString());
        }
        reportDecisionIds.sort();
        manifestDecisionIds.sort();
        if (reportDecisionIds != manifestDecisionIds)
        {
            recordFinding(verification, { QStringLiteral("bundle.decision-mismatch"),
                                          preflightEvidenceBundleManifestMember(),
                                          QStringLiteral("The manifest decisions do not match the report's decisions.") });
        }

        const QJsonObject reportBinding = manifest.value(QStringLiteral("report")).toObject();
        const QString bindingMode = reportBinding.value(QStringLiteral("certificate_binding")).toString();
        const QString declaredMemberDigest = reportBinding.value(QStringLiteral("member_sha256")).toString().toLower();
        const QString actualMemberDigest = sha256Hex(QJsonDocument(reportMember).toJson(QJsonDocument::Indented));
        if (!declaredMemberDigest.isEmpty() && declaredMemberDigest != actualMemberDigest)
        {
            recordFinding(verification,
                          { QStringLiteral("report.member-mismatch"),
                            preflightEvidenceBundleReportMember(),
                            QStringLiteral("The manifest declares report member digest %1, but the member hashes to %2.")
                                .arg(declaredMemberDigest, actualMemberDigest) });
        }
        if (!bindingMode.isEmpty() && bindingMode != QLatin1String("exact") &&
            bindingMode != QLatin1String("path-omitted") && bindingMode != QLatin1String("not-applicable"))
        {
            recordFinding(verification, { QStringLiteral("bundle.report-binding-invalid"),
                                          preflightEvidenceBundleManifestMember(),
                                          QStringLiteral("Unknown report binding '%1'.").arg(bindingMode) });
        }
    }

    // 3. The exported history chain and its head identity.
    QJsonObject historyMember;
    if (!readJsonObject(target.filePath(preflightEvidenceBundleHistoryMember()), historyMember, readError))
    {
        recordFinding(verification, { QStringLiteral("member.invalid-json"),
                                      preflightEvidenceBundleHistoryMember(),
                                      readError });
    }
    else
    {
        const QList<PDFOperationHistoryEvent> events = parseExportedHistory(historyMember);
        const bool chainVerified = verifyExportedChain(events, verification);
        const QJsonObject manifestHistory = manifest.value(QStringLiteral("history")).toObject();
        if (static_cast<int>(manifestHistory.value(QStringLiteral("event_count")).toInteger(-1)) != events.size())
        {
            recordFinding(verification,
                          { QStringLiteral("history.event-count-mismatch"),
                            preflightEvidenceBundleHistoryMember(),
                            QStringLiteral("The manifest declares %1 history events; the member carries %2.")
                                .arg(manifestHistory.value(QStringLiteral("event_count")).toInteger())
                                .arg(events.size()) });
        }
        if (chainVerified && !events.isEmpty())
        {
            const QString manifestHead =
                manifestHistory.value(QStringLiteral("head_event_id")).toString().toLower();
            const QString memberHead = events.back().entryId.toString(QUuid::WithoutBraces).toLower();
            if (manifestHead.isEmpty() || manifestHead != memberHead)
            {
                recordFinding(verification, { QStringLiteral("history.head-mismatch"),
                                              preflightEvidenceBundleHistoryMember(),
                                              QStringLiteral("The manifest head event %1 is not the exported chain head %2.")
                                                  .arg(manifestHead, memberHead) });
            }
        }
    }

    // 4. Certificate and sign-off identities must agree with the manifest.
    const QJsonObject manifestCertificate = manifest.value(QStringLiteral("certificate")).toObject();
    if (!manifestCertificate.isEmpty())
    {
        if (manifestCertificate.value(QStringLiteral("document_revision_digest")).toString().toLower() != documentDigest)
        {
            recordFinding(verification, { QStringLiteral("certificate.identity-mismatch"),
                                          preflightEvidenceBundleCertificateMember(),
                                          QStringLiteral("The certificate binds a different document revision than the manifest.") });
        }
        if (manifestCertificate.value(QStringLiteral("effective_profile_digest")).toString().toLower() !=
            manifest.value(QStringLiteral("effective_profile")).toObject().value(QStringLiteral("digest")).toString().toLower())
        {
            recordFinding(verification, { QStringLiteral("certificate.identity-mismatch"),
                                          preflightEvidenceBundleCertificateMember(),
                                          QStringLiteral("The certificate binds a different effective profile than the manifest.") });
        }

        QJsonObject certificateMember;
        if (readJsonObject(target.filePath(preflightEvidenceBundleCertificateMember()), certificateMember, readError))
        {
            PreflightCertificate certificate;
            if (!PreflightCertificate::fromJson(certificateMember, certificate, readError))
            {
                recordFinding(verification, { QStringLiteral("member.invalid-json"),
                                              preflightEvidenceBundleCertificateMember(),
                                              readError });
            }
            else if (certificate.certificateId != manifestCertificate.value(QStringLiteral("certificate_id")).toString() ||
                     certificate.reportDigest.compare(
                         manifestCertificate.value(QStringLiteral("certificate_report_digest")).toString(),
                         Qt::CaseInsensitive) != 0)
            {
                recordFinding(verification,
                              { QStringLiteral("certificate.manifest-mismatch"),
                                preflightEvidenceBundleCertificateMember(),
                                QStringLiteral("The certificate member disagrees with the manifest's certificate record.") });
            }
        }
        else
        {
            recordFinding(verification, { QStringLiteral("member.missing"),
                                          preflightEvidenceBundleCertificateMember(),
                                          readError });
        }

        const QJsonObject reportBinding = manifest.value(QStringLiteral("report")).toObject();
        if (reportBinding.value(QStringLiteral("certificate_binding")).toString() == QLatin1String("exact"))
        {
            // Re-derivable offline: the exported report is the exact report the
            // certificate hashed.
            QJsonObject report;
            if (readJsonObject(target.filePath(preflightEvidenceBundleReportMember()), report, readError))
            {
                verification.reportBindingRecomputable = true;
                if (reportBindingDigest(report).compare(
                        manifestCertificate.value(QStringLiteral("certificate_report_digest")).toString(),
                        Qt::CaseInsensitive) != 0)
                {
                    recordFinding(verification,
                                  { QStringLiteral("report.certificate-binding"),
                                    preflightEvidenceBundleReportMember(),
                                    QStringLiteral("The exported report does not hash to the certificate's report digest.") });
                }
            }
        }
    }

    const QJsonObject manifestSignOff = manifest.value(QStringLiteral("sign_off")).toObject();
    if (!manifestSignOff.isEmpty())
    {
        if (manifestSignOff.value(QStringLiteral("effective_profile_digest")).toString().toLower() !=
            manifest.value(QStringLiteral("effective_profile")).toObject().value(QStringLiteral("digest")).toString().toLower())
        {
            recordFinding(verification, { QStringLiteral("signoff.identity-mismatch"),
                                          preflightEvidenceBundleSignOffMember(),
                                          QStringLiteral("The sign-off record binds a different effective profile than the manifest.") });
        }
        const QJsonObject output = manifest.value(QStringLiteral("output")).toObject();
        const QString publishedDigest = manifestSignOff.value(QStringLiteral("published_sha256")).toString().toLower();
        if (!output.isEmpty() && isSha256(publishedDigest) &&
            output.value(QStringLiteral("sha256")).toString().toLower() != publishedDigest)
        {
            recordFinding(verification, { QStringLiteral("output.identity-mismatch"),
                                          preflightEvidenceBundleSignOffMember(),
                                          QStringLiteral("The declared output artifact is not the artifact the sign-off published.") });
        }
    }

    // 5. History must bind the certificate and the declared revision.
    if (!manifestCertificate.isEmpty())
    {
        // The certificate records the chain head *at issuance*. The canonical
        // chain legitimately continues afterwards (the CLI appends the
        // CertificateIssued event itself), so the binding to prove offline is
        // that the certificate's issuance point is inside the exported slice.
        const QString certificateHead =
            manifestCertificate.value(QStringLiteral("audit_chain_head_event_id")).toString().toLower();
        if (!certificateHead.isEmpty())
        {
            bool headFound = false;
            if (QFileInfo::exists(target.filePath(preflightEvidenceBundleHistoryMember())))
            {
                QJsonObject historyMemberForHead;
                if (readJsonObject(target.filePath(preflightEvidenceBundleHistoryMember()), historyMemberForHead, readError))
                {
                    for (const QJsonValue& value : historyMemberForHead.value(QStringLiteral("events")).toArray())
                    {
                        if (value.toObject().value(QStringLiteral("entryId")).toString().toLower() == certificateHead)
                        {
                            headFound = true;
                            break;
                        }
                    }
                }
            }
            if (!headFound)
            {
                recordFinding(verification,
                              { QStringLiteral("certificate.chain-head-missing"),
                                preflightEvidenceBundleHistoryMember(),
                                QStringLiteral("The certificate's audit chain head event '%1' is not in the exported history slice.")
                                    .arg(certificateHead) });
            }
        }

        // Waived errors travel with the bundle: every decision the certificate
        // covers must be resolvable from the exported decisions.
        QStringList covering;
        for (const QJsonValue& value : manifestCertificate.value(QStringLiteral("covering_decision_ids")).toArray())
        {
            covering.append(value.toString().toLower());
        }
        if (!covering.isEmpty())
        {
            QStringList available;
            for (const QJsonValue& value : manifest.value(QStringLiteral("decisions")).toArray())
            {
                PreflightDecision decision;
                QString decisionError;
                if (!PreflightDecision::fromJson(value.toObject(), decision, decisionError))
                {
                    recordFinding(verification, { QStringLiteral("decision.unreadable"),
                                                  preflightEvidenceBundleReportMember(),
                                                  QStringLiteral("A decision covered by the certificate is not a valid decision record: %1")
                                                      .arg(decisionError) });
                    continue;
                }
                available.append(preflightDecisionIdentity(decision).toLower());
            }
            for (const QString& id : covering)
            {
                if (!available.contains(id))
                {
                    recordFinding(verification,
                                  { QStringLiteral("certificate.decision-unresolved"),
                                    preflightEvidenceBundleReportMember(),
                                    QStringLiteral("The certificate covers decision '%1', which the exported decisions do not "
                                                   "resolve (a decision whose free-form text was path-redacted cannot be "
                                                   "re-identified offline).")
                                        .arg(id) });
                }
            }
        }
    }
    if (!manifestSignOff.isEmpty())
    {
        const QString signOffSource = manifestSignOff.value(QStringLiteral("source_sha256")).toString().toLower();
        if (isSha256(signOffSource) && signOffSource != documentDigest)
        {
            recordFinding(verification, { QStringLiteral("signoff.identity-mismatch"),
                                          preflightEvidenceBundleSignOffMember(),
                                          QStringLiteral("The sign-off record binds a different source revision.") });
        }
    }

    // 6. Rollback references are digest-addressed, never path-addressed.
    QJsonObject rollbackMember;
    if (readJsonObject(target.filePath(preflightEvidenceBundleRollbackMember()), rollbackMember, readError))
    {
        const QJsonArray memberReferences = rollbackMember.value(QStringLiteral("references")).toArray();
        const QJsonArray manifestReferences = manifest.value(QStringLiteral("rollback_references")).toArray();
        if (memberReferences != manifestReferences)
        {
            recordFinding(verification, { QStringLiteral("rollback.manifest-mismatch"),
                                          preflightEvidenceBundleRollbackMember(),
                                          QStringLiteral("The rollback references member disagrees with the manifest.") });
        }
        for (const QJsonValue& value : memberReferences)
        {
            const QJsonObject reference = value.toObject();
            for (auto it = reference.constBegin(); it != reference.constEnd(); ++it)
            {
                if (it.value().isString() &&
                    redactPathsInString(it.value().toString()) != it.value().toString())
                {
                    recordFinding(verification,
                                  { QStringLiteral("rollback.raw-path"),
                                    preflightEvidenceBundleRollbackMember(),
                                    QStringLiteral("Rollback reference '%1' carries a raw path in '%2'.")
                                        .arg(reference.value(QStringLiteral("rollback_id")).toString(), it.key()) });
                }
            }
        }
    }
    else
    {
        recordFinding(verification, { QStringLiteral("member.missing"),
                                      preflightEvidenceBundleRollbackMember(),
                                      readError });
    }

    verification.summary = verification.valid
                               ? QStringLiteral("The portable proof-of-preflight bundle verifies: %1 members intact, document %2, effective profile %3.")
                                     .arg(verification.membersChecked)
                                     .arg(documentDigest.left(12), manifest.value(QStringLiteral("effective_profile")).toObject().value(QStringLiteral("digest")).toString().left(12))
                               : QStringLiteral("The portable proof-of-preflight bundle does not verify: %1 attributable finding(s).")
                                     .arg(verification.findings.size());
    if (errorMessage)
    {
        *errorMessage = verification.summary;
    }
    return verification;
}

}   // namespace pdf
