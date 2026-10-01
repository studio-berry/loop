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

#include "pdfpreflightverdict.h"
#include "preflightprofileresolver.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace pdf
{
namespace
{
QString receiptIdentity(const PreflightInspectionReceipt& receipt)
{
    const QJsonObject identity{
        { QStringLiteral("kind"), QStringLiteral("loop.inspection-receipt-identity.v1") },
        { QStringLiteral("input_digest"), receipt.inputDigest },
        { QStringLiteral("effective_profile_digest"), receipt.effectiveProfileDigest },
        { QStringLiteral("coverage_scope"), receipt.coverageScope }
    };
    return QString::fromLatin1(QCryptographicHash::hash(canonicalJson(identity), QCryptographicHash::Sha256).toHex());
}

bool strings(const QJsonValue& value, QStringList& output)
{
    if (!value.isArray())
    {
        return false;
    }
    for (const QJsonValue item : value.toArray())
    {
        if (!item.isString() || item.toString().isEmpty())
        {
            return false;
        }
        output.append(item.toString());
    }
    return true;
}

bool counter(const QJsonValue& value, quint64& output)
{
    if (!value.isString())
    {
        return false;
    }
    const QString text = value.toString();
    bool ok = false;
    output = text.toULongLong(&ok);
    return ok && text == QString::number(output);
}
}

QJsonObject PreflightInspectionReceipt::toJson() const
{
    QJsonArray checkArray;
    for (const PreflightReceiptCheck& check : checks)
    {
        checkArray.append(QJsonObject{
            { QStringLiteral("id"), check.id }, { QStringLiteral("required"), check.required }, { QStringLiteral("complete"), check.complete }, { QStringLiteral("status"), check.status }, { QStringLiteral("reason"), check.reason } });
    }
    return QJsonObject{
        { QStringLiteral("schema"), QStringLiteral("loop.inspection-receipt.v1") },
        { QStringLiteral("identity"), identity },
        { QStringLiteral("input_digest"), inputDigest },
        { QStringLiteral("revision"), QJsonObject{
                                          { QStringLiteral("source_sha256"), QString::fromLatin1(revision.document.sourceDataHash.toHex()) },
                                          { QStringLiteral("document_id"), revision.document.documentId },
                                          { QStringLiteral("document_revision"), QString::number(revision.documentRevision) },
                                          { QStringLiteral("cache_generation"), QString::number(revision.cacheGeneration) },
                                          { QStringLiteral("effective_profile_identity"), revision.effectiveProfileIdentity } } },
        { QStringLiteral("effective_profile_digest"), effectiveProfileDigest },
        { QStringLiteral("profile_identity"), profileIdentity },
        { QStringLiteral("coverage_scope"), coverageScope },
        { QStringLiteral("checks"), checkArray },
        { QStringLiteral("evidence_refs"), QJsonArray::fromStringList(evidenceRefs) },
        { QStringLiteral("fidelity"), fidelity },
        { QStringLiteral("limitations"), QJsonArray::fromStringList(limitations) },
        { QStringLiteral("verdict"), verdict.toJson() }
    };
}

bool preflightInspectionReceiptFromJson(const QJsonObject& object, PreflightInspectionReceipt& receipt, QString& errorMessage)
{
    receipt = {};
    errorMessage = QStringLiteral("Invalid inspection receipt.");
    if (object.value(QStringLiteral("schema")) != QJsonValue(QStringLiteral("loop.inspection-receipt.v1")))
    {
        return false;
    }
    for (const QString& key : { QStringLiteral("identity"), QStringLiteral("input_digest"),
                                QStringLiteral("effective_profile_digest"), QStringLiteral("fidelity") })
    {
        if (!object.value(key).isString())
        {
            return false;
        }
    }
    for (const QString& key : { QStringLiteral("revision"), QStringLiteral("profile_identity"),
                                QStringLiteral("coverage_scope"), QStringLiteral("verdict") })
    {
        if (!object.value(key).isObject())
        {
            return false;
        }
    }
    PreflightInspectionReceipt candidate;
    candidate.identity = object.value(QStringLiteral("identity")).toString();
    candidate.inputDigest = object.value(QStringLiteral("input_digest")).toString();
    candidate.effectiveProfileDigest = object.value(QStringLiteral("effective_profile_digest")).toString();
    candidate.fidelity = object.value(QStringLiteral("fidelity")).toString();
    candidate.profileIdentity = object.value(QStringLiteral("profile_identity")).toObject();
    candidate.coverageScope = object.value(QStringLiteral("coverage_scope")).toObject();
    const QJsonObject revision = object.value(QStringLiteral("revision")).toObject();
    const QString source = revision.value(QStringLiteral("source_sha256")).toString();
    if (!revision.value(QStringLiteral("source_sha256")).isString() || (!source.isEmpty() && (!isPDFSha256(source) || source != candidate.inputDigest)) ||
        !revision.value(QStringLiteral("document_id")).isString() ||
        !revision.value(QStringLiteral("effective_profile_identity")).isString() ||
        !counter(revision.value(QStringLiteral("document_revision")), candidate.revision.documentRevision) ||
        !counter(revision.value(QStringLiteral("cache_generation")), candidate.revision.cacheGeneration))
    {
        return false;
    }
    candidate.revision.document.sourceDataHash = QByteArray::fromHex(source.toLatin1());
    candidate.revision.document.documentId = revision.value(QStringLiteral("document_id")).toString();
    candidate.revision.effectiveProfileIdentity = revision.value(QStringLiteral("effective_profile_identity")).toString();
    if (!candidate.revision.isValid())
        return false;
    if ((!candidate.inputDigest.isEmpty() && !isPDFSha256(candidate.inputDigest)) ||
        (!candidate.effectiveProfileDigest.isEmpty() && !isPDFSha256(candidate.effectiveProfileDigest)) ||
        candidate.inputDigest != candidate.inputDigest.toLower() ||
        candidate.effectiveProfileDigest != candidate.effectiveProfileDigest.toLower() ||
        candidate.identity != receiptIdentity(candidate) ||
        !strings(object.value(QStringLiteral("evidence_refs")), candidate.evidenceRefs) ||
        !strings(object.value(QStringLiteral("limitations")), candidate.limitations))
    {
        return false;
    }
    const QJsonObject verdict = object.value(QStringLiteral("verdict")).toObject();
    const QString state = verdict.value(QStringLiteral("state")).toString();
    if (!QStringList{ QStringLiteral("pass"), QStringLiteral("fail"), QStringLiteral("incomplete"), QStringLiteral("error") }.contains(state) ||
        !verdict.value(QStringLiteral("reason_code")).isString() || !verdict.value(QStringLiteral("reason")).isString())
    {
        return false;
    }
    candidate.verdict = preflightVerdictFromJson(verdict);
    candidate.verdict.blockingFindingIds.clear();
    candidate.verdict.waivedFindingIds.clear();
    if (!strings(verdict.value(QStringLiteral("blocking_finding_ids")), candidate.verdict.blockingFindingIds) ||
        !strings(verdict.value(QStringLiteral("waived_finding_ids")), candidate.verdict.waivedFindingIds) ||
        !object.value(QStringLiteral("checks")).isArray())
    {
        return false;
    }
    QSet<QString> ids;
    bool incomplete = candidate.coverageScope.isEmpty();
    const QStringList statuses{ QString(), QStringLiteral("ok"), QStringLiteral("warning"), QStringLiteral("failed"),
                                QStringLiteral("incomplete"), QStringLiteral("unsupported"), QStringLiteral("skipped"),
                                QStringLiteral("not_inspected"), QStringLiteral("not_applicable") };
    for (const QJsonValue value : object.value(QStringLiteral("checks")).toArray())
    {
        if (!value.isObject())
        {
            return false;
        }
        const QJsonObject check = value.toObject();
        PreflightReceiptCheck parsed;
        if (!check.value(QStringLiteral("id")).isString() || !check.value(QStringLiteral("required")).isBool() ||
            !check.value(QStringLiteral("complete")).isBool() || !check.value(QStringLiteral("status")).isString() ||
            !check.value(QStringLiteral("reason")).isString())
        {
            return false;
        }
        parsed.id = check.value(QStringLiteral("id")).toString();
        parsed.required = check.value(QStringLiteral("required")).toBool();
        parsed.complete = check.value(QStringLiteral("complete")).toBool();
        parsed.status = check.value(QStringLiteral("status")).toString();
        parsed.reason = check.value(QStringLiteral("reason")).toString();
        const bool completedStatus = parsed.status == QLatin1String("ok") || parsed.status == QLatin1String("warning") ||
                                     parsed.status == QLatin1String("failed");
        if (parsed.id.isEmpty() || ids.contains(parsed.id) || !statuses.contains(parsed.status) || (parsed.complete && !completedStatus))
        {
            return false;
        }
        ids.insert(parsed.id);
        incomplete |= !parsed.complete;
        candidate.checks.append(parsed);
    }
    incomplete |= candidate.checks.isEmpty() || candidate.evidenceRefs.isEmpty() || candidate.fidelity == QLatin1String("unsupported") ||
                  candidate.fidelity == QLatin1String("not-recorded");
    if (!QStringList{ QStringLiteral("exact"), QStringLiteral("sampled"), QStringLiteral("catalog"),
                      QStringLiteral("unsupported"), QStringLiteral("not-recorded") }
             .contains(candidate.fidelity) ||
        (candidate.verdict.isPass() && (incomplete || !candidate.verdict.blockingFindingIds.isEmpty())) ||
        (candidate.verdict.state == PreflightVerdictState::Fail && candidate.verdict.blockingFindingIds.isEmpty()))
    {
        return false;
    }
    const bool unknownIdentity = candidate.inputDigest.isEmpty() || candidate.effectiveProfileDigest.isEmpty();
    if (unknownIdentity &&
        (candidate.verdict.state != PreflightVerdictState::Incomplete || !candidate.coverageScope.isEmpty() ||
         !candidate.checks.isEmpty() || !candidate.evidenceRefs.isEmpty() || !candidate.profileIdentity.isEmpty() ||
         candidate.fidelity != QLatin1String("not-recorded") || candidate.limitations.isEmpty()))
    {
        return false;
    }
    receipt = std::move(candidate);
    errorMessage.clear();
    return true;
}

bool validatePreflightInspectionReceipt(const PreflightInspectionReceipt& receipt, const QString& inputDigest,
                                        const PDFRevisionIdentity& revision, const PreflightProfileData& profile,
                                        QString& errorMessage)
{
    PreflightInspectionReceipt parsed;
    if (!preflightInspectionReceiptFromJson(receipt.toJson(), parsed, errorMessage))
    {
        return false;
    }
    errorMessage = QStringLiteral("Inspection receipt does not match the requested input, revision or profile coverage.");
    QJsonObject expectedCoverage = profile.coverageScope;
    if (!profile.restrictions.isUnrestricted())
    {
        expectedCoverage.insert(QStringLiteral("scope_restrictions"), profile.restrictions.toJson());
    }
    if (expectedCoverage.isEmpty() || parsed.inputDigest != inputDigest || parsed.revision != revision ||
        parsed.effectiveProfileDigest != profile.effectiveDigest || parsed.profileIdentity != profile.profileIdentity ||
        parsed.coverageScope != expectedCoverage)
    {
        return false;
    }
    QSet<QString> expected;
    for (const PreflightCheckConfig& check : profile.checks)
    {
        if (!check.enabled)
        {
            continue;
        }
        if (expected.contains(check.id))
        {
            return false;
        }
        expected.insert(check.id);
        const auto found = std::find_if(parsed.checks.cbegin(), parsed.checks.cend(),
                                        [&](const PreflightReceiptCheck& value)
                                        { return value.id == check.id; });
        if (found == parsed.checks.cend() || found->required != check.required)
        {
            return false;
        }
    }
    if (profile.pdfx.has_value())
    {
        expected.insert(QStringLiteral("pdfx"));
        const auto found = std::find_if(parsed.checks.cbegin(), parsed.checks.cend(),
                                        [](const PreflightReceiptCheck& check)
                                        { return check.id == QLatin1String("pdfx"); });
        if (found == parsed.checks.cend() || !found->required)
            return false;
    }
    if (expected.size() != parsed.checks.size())
    {
        return false;
    }
    errorMessage.clear();
    return true;
}

PreflightInspectionReceipt buildTerminalPreflightReceipt(const QString& inputDigest, const PDFRevisionIdentity& revision,
                                                         const QString& profileDigest, const QString& reasonCode)
{
    PreflightInspectionReceipt receipt;
    receipt.inputDigest = inputDigest;
    receipt.revision = revision;
    receipt.effectiveProfileDigest = profileDigest;
    receipt.fidelity = QStringLiteral("not-recorded");
    receipt.limitations = { QStringLiteral("Inspection did not complete; no coverage is admitted.") };
    receipt.verdict.state = PreflightVerdictState::Incomplete;
    receipt.verdict.reasonCode = reasonCode;
    receipt.verdict.reason = QStringLiteral("Isolated inspection did not complete.");
    receipt.identity = receiptIdentity(receipt);
    return receipt;
}
}   // namespace pdf
