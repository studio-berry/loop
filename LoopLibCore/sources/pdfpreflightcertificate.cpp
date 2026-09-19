// MIT License

#include "pdfpreflightcertificate.h"

#include "pdfpreflightverdict.h"
#include "pdfutils.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QUuid>

namespace pdf
{

namespace
{

QString dateTimeString(const QDateTime& value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime dateTimeFromString(const QString& value)
{
    return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
}

QString digest(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool validDigest(const QString& value)
{
    return isPDFSha256(value);
}

QString reportDigest(const QJsonObject& report)
{
    return digest(canonicalJson(report));
}

bool verifyHistoryChain(const QList<PDFOperationHistoryEvent>& history, QString* errorMessage = nullptr)
{
    QByteArray previousHash;
    qint64 expectedSequence = 1;
    for (const PDFOperationHistoryEvent& event : history)
    {
        if (event.sequence != expectedSequence)
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("The operation history sequence is discontinuous.");
            return false;
        }
        if (event.previousEventHash != previousHash ||
            event.eventHash != computeOperationHistoryEventHash(event, previousHash))
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("The operation history hash chain is broken.");
            return false;
        }
        previousHash = event.eventHash;
        ++expectedSequence;
    }
    if (errorMessage)
        errorMessage->clear();
    return true;
}

int eventIndex(const QList<PDFOperationHistoryEvent>& history, const QString& entryId)
{
    for (int index = 0; index < history.size(); ++index)
    {
        if (history.at(index).entryId.toString(QUuid::WithoutBraces) == entryId)
            return index;
    }
    return -1;
}

bool decisionIsActiveAt(const QList<PDFOperationHistoryEvent>& history,
                        const QString& decisionId,
                        int throughIndex)
{
    bool active = false;
    const int lastIndex = qMin(throughIndex, static_cast<int>(history.size()) - 1);
    for (int index = 0; index <= lastIndex; ++index)
    {
        const PDFOperationHistoryEvent& event = history.at(index);
        if (event.kind == PDFOperationHistoryEventKind::DecisionRecorded &&
            event.resultSummary.value(QStringLiteral("decision_id")).toString() == decisionId)
        {
            active = true;
        }
        else if (event.kind == PDFOperationHistoryEventKind::DecisionInvalidated &&
                 (event.resultSummary.value(QStringLiteral("decision_id")).toString() == decisionId ||
                  event.approval.decisionReference == decisionId))
        {
            active = false;
        }
    }
    return active;
}

bool decisionWasInvalidatedAfter(const QList<PDFOperationHistoryEvent>& history,
                                 const QString& decisionId,
                                 int afterIndex)
{
    for (int index = afterIndex + 1; index < history.size(); ++index)
    {
        const PDFOperationHistoryEvent& event = history.at(index);
        if (event.kind == PDFOperationHistoryEventKind::DecisionInvalidated &&
            (event.resultSummary.value(QStringLiteral("decision_id")).toString() == decisionId ||
             event.approval.decisionReference == decisionId))
        {
            return true;
        }
    }
    return false;
}

}   // namespace

QString preflightCertificateStateToString(PreflightCertificateState state)
{
    switch (state)
    {
        case PreflightCertificateState::Valid:
            return QStringLiteral("valid");
        case PreflightCertificateState::InvalidDocumentChanged:
            return QStringLiteral("invalid-document-changed");
        case PreflightCertificateState::InvalidAuditChainBroken:
            return QStringLiteral("invalid-audit-chain-broken");
        case PreflightCertificateState::InvalidDecisionStale:
            return QStringLiteral("invalid-decision-stale");
        case PreflightCertificateState::InvalidProfileProvisional:
            return QStringLiteral("invalid-profile-provisional");
        case PreflightCertificateState::InvalidInspection:
            return QStringLiteral("invalid-inspection");
        case PreflightCertificateState::InvalidCertificate:
            return QStringLiteral("invalid-certificate");
    }
    return QStringLiteral("invalid-certificate");
}

QJsonObject PreflightCertificate::toJson() const
{
    return QJsonObject{
        { QStringLiteral("certificate_id"), certificateId },
        { QStringLiteral("issued_at_utc"), dateTimeString(issuedAtUtc) },
        { QStringLiteral("issued_by"), issuedBy },
        { QStringLiteral("document_revision_digest"), documentRevisionDigest },
        { QStringLiteral("effective_profile_digest"), effectiveProfileDigest },
        { QStringLiteral("report_digest"), reportDigest },
        { QStringLiteral("audit_chain_head_event_id"), auditChainHeadEventId },
        { QStringLiteral("error_count"), errorCount },
        { QStringLiteral("waived_error_count"), waivedErrorCount },
        { QStringLiteral("covering_decision_ids"), QJsonArray::fromStringList(coveringDecisionIds) },
        { QStringLiteral("type"), QStringLiteral("loop-certified-preflight") },
        { QStringLiteral("trust_model"), QStringLiteral("tamper-evident attribution, not a digital signature") }
    };
}

bool PreflightCertificate::fromJson(const QJsonObject& object,
                                    PreflightCertificate& certificate,
                                    QString& errorMessage)
{
    certificate = {};
    certificate.certificateId = object.value(QStringLiteral("certificate_id")).toString().trimmed();
    certificate.issuedAtUtc = dateTimeFromString(object.value(QStringLiteral("issued_at_utc")).toString());
    certificate.issuedBy = object.value(QStringLiteral("issued_by")).toString().trimmed();
    certificate.documentRevisionDigest = object.value(QStringLiteral("document_revision_digest")).toString().trimmed().toLower();
    certificate.effectiveProfileDigest = object.value(QStringLiteral("effective_profile_digest")).toString().trimmed().toLower();
    certificate.reportDigest = object.value(QStringLiteral("report_digest")).toString().trimmed().toLower();
    certificate.auditChainHeadEventId = object.value(QStringLiteral("audit_chain_head_event_id")).toString().trimmed();
    certificate.errorCount = object.value(QStringLiteral("error_count")).toInt(-1);
    certificate.waivedErrorCount = object.value(QStringLiteral("waived_error_count")).toInt(-1);
    for (const QJsonValue& value : object.value(QStringLiteral("covering_decision_ids")).toArray())
        certificate.coveringDecisionIds.append(value.toString());

    if (object.value(QStringLiteral("type")).toString() != QLatin1String("loop-certified-preflight") ||
        certificate.certificateId.isEmpty() || !certificate.issuedAtUtc.isValid() || certificate.issuedBy.isEmpty() ||
        !validDigest(certificate.documentRevisionDigest) || !validDigest(certificate.effectiveProfileDigest) ||
        !validDigest(certificate.reportDigest) || certificate.auditChainHeadEventId.isEmpty() ||
        certificate.errorCount < 0 || certificate.waivedErrorCount < 0)
    {
        errorMessage = QStringLiteral("Certificate fields are missing or invalid.");
        return false;
    }
    errorMessage.clear();
    return true;
}

QJsonObject PreflightCertificateVerification::toJson() const
{
    return QJsonObject{
        { QStringLiteral("state"), preflightCertificateStateToString(state) },
        { QStringLiteral("valid"), isValid() },
        { QStringLiteral("reason"), reason }
    };
}

QString preflightDecisionIdentity(const PreflightDecision& decision)
{
    return digest(canonicalJson(decision.toJson(decision.documentRevisionDigest,
                                                decision.effectiveProfileDigest)));
}

bool issuePreflightCertificate(const PreflightResult& result,
                               const QJsonObject& report,
                               const QByteArray& documentBytes,
                               const QList<PDFOperationHistoryEvent>& history,
                               const QString& issuedBy,
                               PreflightCertificate& certificate,
                               QString& errorMessage)
{
    certificate = {};
    const QString documentDigest = digest(documentBytes);
    if (result.documentRevisionDigest.compare(documentDigest, Qt::CaseInsensitive) != 0)
    {
        errorMessage = QStringLiteral("The document changed before certification.");
        return false;
    }
    if (!preflightAllowsCertification(result))
    {
        errorMessage = QStringLiteral("The preflight result does not meet certification requirements.");
        return false;
    }
    if (issuedBy.trimmed().isEmpty() || !validDigest(result.effectiveProfileDigest))
    {
        errorMessage = QStringLiteral("Certification requires an operator and an effective profile digest.");
        return false;
    }
    if (history.isEmpty() || !verifyHistoryChain(history, &errorMessage))
    {
        if (errorMessage.isEmpty())
            errorMessage = QStringLiteral("Certification requires a verified audit history.");
        return false;
    }

    bool matchingPreflightFound = false;
    for (const PDFOperationHistoryEvent& event : history)
    {
        if (event.kind == PDFOperationHistoryEventKind::PreflightRun &&
            event.status == PDFOperationHistoryStatus::Accepted &&
            event.documentRevisionDigest.compare(documentDigest, Qt::CaseInsensitive) == 0 &&
            event.effectiveProfileDigest.compare(result.effectiveProfileDigest, Qt::CaseInsensitive) == 0)
        {
            matchingPreflightFound = true;
        }
    }
    if (!matchingPreflightFound)
    {
        errorMessage = QStringLiteral("Certification requires an accepted preflight event in the audit chain.");
        return false;
    }

    const PreflightVerdict verdict = reducePreflightVerdict(result);
    QStringList coveringDecisionIds;
    for (const QString& waivedFindingId : verdict.waivedFindingIds)
    {
        bool covered = false;
        for (const PreflightDecision& decision : result.decisions)
        {
            if (decision.findingId != waivedFindingId ||
                !decision.countsForSignoff(documentDigest, result.effectiveProfileDigest))
            {
                continue;
            }

            const QString decisionId = preflightDecisionIdentity(decision);
            if (!decisionIsActiveAt(history, decisionId, history.size() - 1))
            {
                errorMessage = QStringLiteral("Certification requires every covering decision to be recorded as active in the audit chain.");
                return false;
            }
            coveringDecisionIds.append(decisionId);
            covered = true;
            break;
        }
        if (!covered)
        {
            errorMessage = QStringLiteral("Certification could not resolve an active decision for a waived error.");
            return false;
        }
    }
    coveringDecisionIds.removeDuplicates();

    certificate.certificateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    certificate.issuedAtUtc = QDateTime::currentDateTimeUtc();
    certificate.issuedBy = issuedBy.trimmed();
    certificate.documentRevisionDigest = documentDigest;
    certificate.effectiveProfileDigest = result.effectiveProfileDigest.toLower();
    certificate.reportDigest = reportDigest(report);
    certificate.auditChainHeadEventId = history.back().entryId.toString(QUuid::WithoutBraces);
    certificate.errorCount = result.errors.size();
    certificate.waivedErrorCount = verdict.waivedFindingIds.size();
    certificate.coveringDecisionIds = coveringDecisionIds;
    errorMessage.clear();
    return true;
}

PreflightCertificateVerification verifyPreflightCertificate(const PreflightCertificate& certificate,
                                                            const QByteArray& documentBytes,
                                                            const QList<PDFOperationHistoryEvent>& history)
{
    PreflightCertificateVerification result;
    if (certificate.certificateId.isEmpty() || !certificate.issuedAtUtc.isValid() ||
        !validDigest(certificate.documentRevisionDigest) || !validDigest(certificate.effectiveProfileDigest) ||
        !validDigest(certificate.reportDigest) || certificate.auditChainHeadEventId.isEmpty())
    {
        result.state = PreflightCertificateState::InvalidCertificate;
        result.reason = QStringLiteral("Certificate fields are missing or invalid.");
        return result;
    }
    if (digest(documentBytes).compare(certificate.documentRevisionDigest, Qt::CaseInsensitive) != 0)
    {
        result.state = PreflightCertificateState::InvalidDocumentChanged;
        result.reason = QStringLiteral("The document bytes do not match the certified revision.");
        return result;
    }

    QString chainError;
    if (history.isEmpty() || !verifyHistoryChain(history, &chainError))
    {
        result.state = PreflightCertificateState::InvalidAuditChainBroken;
        result.reason = chainError.isEmpty() ? QStringLiteral("The operation history is unavailable.") : chainError;
        return result;
    }

    const int headIndex = eventIndex(history, certificate.auditChainHeadEventId);
    if (headIndex < 0)
    {
        result.state = PreflightCertificateState::InvalidAuditChainBroken;
        result.reason = QStringLiteral("The certificate audit-chain head is not present.");
        return result;
    }

    const int issuanceIndex = headIndex + 1;
    if (issuanceIndex >= history.size())
    {
        result.state = PreflightCertificateState::InvalidCertificate;
        result.reason = QStringLiteral("The certificate issuance event is missing.");
        return result;
    }

    const PDFOperationHistoryEvent& issuance = history.at(issuanceIndex);
    const QJsonObject retainedCertificate = issuance.resultSummary.value(QStringLiteral("certificate")).toObject();
    if (issuance.kind != PDFOperationHistoryEventKind::CertificateIssued ||
        issuance.approval.decisionReference != certificate.certificateId ||
        issuance.documentRevisionDigest.compare(certificate.documentRevisionDigest, Qt::CaseInsensitive) != 0 ||
        issuance.effectiveProfileDigest.compare(certificate.effectiveProfileDigest, Qt::CaseInsensitive) != 0 ||
        issuance.resultSummary.value(QStringLiteral("report_digest")).toString().compare(certificate.reportDigest, Qt::CaseInsensitive) != 0 ||
        retainedCertificate.isEmpty() ||
        canonicalJson(retainedCertificate) != canonicalJson(certificate.toJson()))
    {
        result.state = PreflightCertificateState::InvalidCertificate;
        result.reason = QStringLiteral("The certificate issuance event does not match the certificate.");
        return result;
    }

    for (int index = issuanceIndex + 1; index < history.size(); ++index)
    {
        const PDFOperationHistoryEvent& event = history.at(index);
        if (event.kind == PDFOperationHistoryEventKind::CertificateInvalidated &&
            event.approval.decisionReference == certificate.certificateId)
        {
            result.state = PreflightCertificateState::InvalidCertificate;
            result.reason = event.resultSummary.value(QStringLiteral("reason")).toString(
                QStringLiteral("The certificate was invalidated by a later operation."));
            return result;
        }
    }

    for (const QString& decisionId : certificate.coveringDecisionIds)
    {
        if (!decisionIsActiveAt(history, decisionId, headIndex) ||
            decisionWasInvalidatedAfter(history, decisionId, issuanceIndex))
        {
            result.state = PreflightCertificateState::InvalidDecisionStale;
            result.reason = QStringLiteral("A decision covering this certificate is stale or missing.");
            return result;
        }
    }

    result.state = PreflightCertificateState::Valid;
    result.reason = QStringLiteral("Certified preflight matches the document and verified audit chain.");
    return result;
}

std::optional<PreflightCertificate> latestPreflightCertificate(const QList<PDFOperationHistoryEvent>& history,
                                                               QString* errorMessage)
{
    if (errorMessage)
        errorMessage->clear();

    for (int index = history.size() - 1; index >= 0; --index)
    {
        const PDFOperationHistoryEvent& event = history.at(index);
        if (event.kind != PDFOperationHistoryEventKind::CertificateIssued)
            continue;

        const QString certificateId = event.resultSummary.value(QStringLiteral("certificate_id")).toString();
        if (certificateId.isEmpty())
        {
            // Older repair history used CertificateIssued for governed repair
            // completion. It is not a certified-preflight record.
            continue;
        }

        const QJsonObject object = event.resultSummary.value(QStringLiteral("certificate")).toObject();
        if (object.isEmpty())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("The retained certificate record is incomplete.");
            return std::nullopt;
        }

        PreflightCertificate certificate;
        QString parseError;
        if (!PreflightCertificate::fromJson(object, certificate, parseError))
        {
            if (errorMessage)
                *errorMessage = parseError;
            return std::nullopt;
        }
        return certificate;
    }
    return std::nullopt;
}

}   // namespace pdf
