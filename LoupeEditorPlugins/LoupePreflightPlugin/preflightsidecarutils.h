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

#ifndef PREFLIGHTSIDECARUTILS_H
#define PREFLIGHTSIDECARUTILS_H

#include "pdffixupregistry.h"
#include "pdfpreflightverdict.h"
#include "pdfschemaversion.h"

#include <cmath>
#include <memory>

#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>

#ifndef LOUPE_PREFLIGHT_SCHEMA_VERSION
#define LOUPE_PREFLIGHT_SCHEMA_VERSION 3
#endif

namespace pdfplugin::preflight
{

inline QString resolveBundlePath(const QString& applicationDirectory, const QString& relativePath)
{
    return QDir::cleanPath(QDir(applicationDirectory).filePath(relativePath));
}

inline QString getPdfToolFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("PdfTool.exe");
#else
    return QStringLiteral("PdfTool");
#endif
}

inline bool isExpectedPreflightExitCode(int exitCode)
{
    return exitCode == 0 || exitCode == 1 || exitCode == 8 || exitCode == 9;
}

inline constexpr int PREFLIGHT_SIDECAR_STDOUT_MAX_BYTES = 16 * 1024 * 1024;
inline constexpr int PREFLIGHT_SIDECAR_STDERR_MAX_BYTES = 256 * 1024;
inline constexpr int PREFLIGHT_SIDECAR_SPILL_WATERMARK_BYTES = 1024 * 1024;

/// Incrementally captures sidecar process output with an in-memory watermark and spill file.
class PreflightSidecarStreamBuffer
{
public:
    enum class AppendResult
    {
        Ok,
        Overflow
    };

    explicit PreflightSidecarStreamBuffer(int maxBytes) :
        m_maxBytes(maxBytes)
    {
    }

    AppendResult append(const QByteArray& chunk)
    {
        if (chunk.isEmpty())
        {
            return AppendResult::Ok;
        }

        const qint64 newTotal = m_totalSize + chunk.size();
        if (newTotal > m_maxBytes)
        {
            return AppendResult::Overflow;
        }

        m_memory.append(chunk);
        m_totalSize = newTotal;

        if (m_memory.size() >= PREFLIGHT_SIDECAR_SPILL_WATERMARK_BYTES)
        {
            if (!ensureSpillOpen())
            {
                return AppendResult::Overflow;
            }

            if (m_spillFile->write(m_memory) != m_memory.size())
            {
                return AppendResult::Overflow;
            }

            if (!m_spillFile->flush())
            {
                return AppendResult::Overflow;
            }

            m_spilledBytes += m_memory.size();
            m_memory.clear();
        }

        return AppendResult::Ok;
    }

    QByteArray takeData() const
    {
        QByteArray result;
        result.reserve(int(m_totalSize));

        if (m_spillFile && m_spillFile->isOpen())
        {
            m_spillFile->flush();
            const qint64 position = m_spillFile->pos();
            m_spillFile->seek(0);
            result.append(m_spillFile->readAll());
            m_spillFile->seek(position);
        }

        result.append(m_memory);
        return result;
    }

    void clear()
    {
        m_memory.clear();
        m_totalSize = 0;
        m_spilledBytes = 0;
        m_spillFile.reset();
    }

    qint64 totalSize() const
    {
        return m_totalSize;
    }

    qint64 spilledBytes() const
    {
        return m_spilledBytes;
    }

private:
    bool ensureSpillOpen()
    {
        if (m_spillFile)
        {
            return m_spillFile->isOpen();
        }

        auto spillFile = std::make_unique<QTemporaryFile>();
        spillFile->setAutoRemove(true);
        if (!spillFile->open())
        {
            return false;
        }

        m_spillFile = std::move(spillFile);
        return true;
    }

    int m_maxBytes = 0;
    QByteArray m_memory;
    qint64 m_totalSize = 0;
    qint64 m_spilledBytes = 0;
    mutable std::unique_ptr<QTemporaryFile> m_spillFile;
};

inline bool isImplementedFixupId(const QString& fixupId)
{
    return pdf::isImplementedFixupId(fixupId);
}

inline QJsonObject filterAdvertisedFixups(const QJsonObject& report)
{
    const QJsonArray fixups = report.value(QStringLiteral("fixups_available")).toArray();
    QJsonArray filteredFixups;
    for (const QJsonValue& fixupValue : fixups)
    {
        const QJsonObject fixupObject = fixupValue.toObject();
        if (isImplementedFixupId(fixupObject.value(QStringLiteral("id")).toString()))
        {
            filteredFixups.append(fixupObject);
        }
    }

    QJsonObject filteredReport = report;
    filteredReport.insert(QStringLiteral("fixups_available"), filteredFixups);
    return filteredReport;
}

inline bool setValidationError(QString* errorMessage, const QString& message)
{
    if (errorMessage)
    {
        *errorMessage = message;
    }

    return false;
}

inline bool hasOnlyProperties(const QJsonObject& object,
                              const QSet<QString>& allowedProperties,
                              const QString& context,
                              QString* errorMessage,
                              bool allowUnknownFields = false)
{
    if (allowUnknownFields)
    {
        return true;
    }

    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator)
    {
        if (!allowedProperties.contains(iterator.key()))
        {
            return setValidationError(errorMessage,
                                      QStringLiteral("%1 contains unsupported property '%2'.").arg(context, iterator.key()));
        }
    }

    return true;
}

inline bool isInteger(const QJsonValue& value)
{
    const double number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && std::floor(number) == number;
}

inline bool isContractIdentifier(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[a-z][a-z0-9-]*$"));
    return expression.match(value).hasMatch();
}

inline bool isStableFindingId(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{16}$"));
    return expression.match(value).hasMatch();
}

inline bool isSupportedSchemaVersion(int schemaVersion)
{
    return schemaVersion >= 1 && schemaVersion <= LOUPE_PREFLIGHT_SCHEMA_VERSION;
}

inline bool validateBboxValue(const QJsonValue& bboxValue, const QString& context, QString* errorMessage)
{
    if (!bboxValue.isArray() || bboxValue.toArray().size() != 4)
    {
        return setValidationError(errorMessage, QStringLiteral("%1.bbox must contain four numbers.").arg(context));
    }

    const QJsonArray bbox = bboxValue.toArray();
    for (const QJsonValue& coordinate : bbox)
    {
        if (!coordinate.isDouble())
        {
            return setValidationError(errorMessage, QStringLiteral("%1.bbox must contain four numbers.").arg(context));
        }
    }

    if (bbox.at(0).toDouble() > bbox.at(2).toDouble() || bbox.at(1).toDouble() > bbox.at(3).toDouble())
    {
        return setValidationError(errorMessage, QStringLiteral("%1.bbox coordinates are not ordered.").arg(context));
    }

    return true;
}

inline bool validateFindingCommonFields(const QJsonObject& finding, const QString& context, QString* errorMessage)
{
    const QString type = finding.value(QStringLiteral("type")).toString();
    if (!finding.value(QStringLiteral("type")).isString() || !isContractIdentifier(type))
    {
        return setValidationError(errorMessage, QStringLiteral("%1.type must be a non-empty kebab-case identifier.").arg(context));
    }

    const QJsonValue id = finding.value(QStringLiteral("id"));
    if (!id.isUndefined() && (!id.isString() || !isStableFindingId(id.toString())))
    {
        return setValidationError(errorMessage, QStringLiteral("%1.id must be a 16-character lowercase hexadecimal stable finding id.").arg(context));
    }

    const QString severity = finding.value(QStringLiteral("severity")).toString();
    if (severity != QStringLiteral("error") && severity != QStringLiteral("warning") && severity != QStringLiteral("info"))
    {
        return setValidationError(errorMessage, QStringLiteral("%1.severity is invalid.").arg(context));
    }

    if (!finding.value(QStringLiteral("message")).isString() || finding.value(QStringLiteral("message")).toString().isEmpty())
    {
        return setValidationError(errorMessage, QStringLiteral("%1.message must be a non-empty string.").arg(context));
    }

    const QJsonValue objectId = finding.value(QStringLiteral("object_id"));
    if (!objectId.isUndefined() && !objectId.isNull() && !objectId.isString())
    {
        return setValidationError(errorMessage, QStringLiteral("%1.object_id must be a string or null.").arg(context));
    }

    const QJsonValue checkId = finding.value(QStringLiteral("check_id"));
    if (!checkId.isUndefined() && !checkId.isString())
    {
        return setValidationError(errorMessage, QStringLiteral("%1.check_id must be a string.").arg(context));
    }

    return true;
}

/// Keys accepted on a schema-version-1 finding. Must stay in sync with
/// loupe-preflight/schemas/report.schema.json's finding_v1 definition; see
/// normalizedReportAllowedProperties() for why this is a named accessor.
inline const QSet<QString>& findingV1AllowedProperties()
{
    static const QSet<QString> allowedProperties = {
        QStringLiteral("page"),
        QStringLiteral("object_id"),
        QStringLiteral("type"),
        QStringLiteral("severity"),
        QStringLiteral("message"),
        QStringLiteral("bbox"),
        QStringLiteral("check_id"),
        QStringLiteral("evidence"),
        QStringLiteral("evidence_ids")
    };
    return allowedProperties;
}

/// Keys accepted on a schema-version-2+ finding. Must stay in sync with
/// loupe-preflight/schemas/report.schema.json's finding_v2 definition; see
/// normalizedReportAllowedProperties() for why this is a named accessor.
inline const QSet<QString>& findingV2AllowedProperties()
{
    static const QSet<QString> allowedProperties = {
        QStringLiteral("scope"),
        QStringLiteral("id"),
        QStringLiteral("page"),
        QStringLiteral("object_id"),
        QStringLiteral("type"),
        QStringLiteral("severity"),
        QStringLiteral("message"),
        QStringLiteral("bbox"),
        QStringLiteral("check_id"),
        QStringLiteral("evidence"),
        QStringLiteral("evidence_ids")
    };
    return allowedProperties;
}

inline bool validateFindingV1(const QJsonObject& finding, const QString& context, QString* errorMessage)
{
    if (!hasOnlyProperties(finding, findingV1AllowedProperties(), context, errorMessage))
    {
        return false;
    }

    const QJsonValue page = finding.value(QStringLiteral("page"));
    if (!isInteger(page) || page.toInt() < 1)
    {
        return setValidationError(errorMessage, QStringLiteral("%1.page must be a positive integer.").arg(context));
    }

    if (!validateFindingCommonFields(finding, context, errorMessage))
    {
        return false;
    }

    return validateBboxValue(finding.value(QStringLiteral("bbox")), context, errorMessage);
}

inline bool validateFindingV2(const QJsonObject& finding, const QString& context, QString* errorMessage)
{
    if (!hasOnlyProperties(finding, findingV2AllowedProperties(), context, errorMessage))
    {
        return false;
    }

    const QString scope = finding.value(QStringLiteral("scope")).toString();
    if (scope != QStringLiteral("document") && scope != QStringLiteral("page") && scope != QStringLiteral("object"))
    {
        return setValidationError(errorMessage, QStringLiteral("%1.scope must be document, page, or object.").arg(context));
    }

    if (!validateFindingCommonFields(finding, context, errorMessage))
    {
        return false;
    }

    const bool hasPage = finding.contains(QStringLiteral("page"));
    const bool hasBbox = finding.contains(QStringLiteral("bbox"));

    if (scope == QStringLiteral("document"))
    {
        if (hasPage)
        {
            return setValidationError(errorMessage, QStringLiteral("%1.page must be absent for document scope.").arg(context));
        }

        if (hasBbox)
        {
            return setValidationError(errorMessage, QStringLiteral("%1.bbox must be absent for document scope.").arg(context));
        }

        return true;
    }

    const QJsonValue page = finding.value(QStringLiteral("page"));
    if (!isInteger(page) || page.toInt() < 1)
    {
        return setValidationError(errorMessage, QStringLiteral("%1.page must be a positive integer for page/object scope.").arg(context));
    }

    if (hasBbox && !validateBboxValue(finding.value(QStringLiteral("bbox")), context, errorMessage))
    {
        return false;
    }

    return true;
}

inline bool findingHasVisualOverlay(const QJsonObject& finding, int schemaVersion)
{
    if (schemaVersion == 1)
    {
        return finding.contains(QStringLiteral("bbox"));
    }

    const QString scope = finding.value(QStringLiteral("scope")).toString();
    if (scope == QStringLiteral("document"))
    {
        return false;
    }

    return finding.contains(QStringLiteral("bbox"));
}

inline bool validateFinding(const QJsonValue& value, const QString& section, int index, int schemaVersion, QString* errorMessage)
{
    const QString context = QStringLiteral("%1[%2]").arg(section).arg(index);
    if (!value.isObject())
    {
        return setValidationError(errorMessage, QStringLiteral("%1 must be an object.").arg(context));
    }

    const QJsonObject finding = value.toObject();
    if (schemaVersion == 1)
    {
        return validateFindingV1(finding, context, errorMessage);
    }

    return validateFindingV2(finding, context, errorMessage);
}

inline bool validateFixup(const QJsonValue& value, int index, QString* errorMessage)
{
    const QString context = QStringLiteral("fixups_available[%1]").arg(index);
    if (!value.isObject())
    {
        return setValidationError(errorMessage, QStringLiteral("%1 must be an object.").arg(context));
    }

    const QJsonObject fixup = value.toObject();
    static const QSet<QString> allowedProperties = {
        QStringLiteral("id"),
        QStringLiteral("safe"),
        QStringLiteral("description"),
        QStringLiteral("params")
    };
    if (!hasOnlyProperties(fixup, allowedProperties, context, errorMessage))
    {
        return false;
    }

    const QString id = fixup.value(QStringLiteral("id")).toString();
    if (!fixup.value(QStringLiteral("id")).isString() || !isContractIdentifier(id))
    {
        return setValidationError(errorMessage, QStringLiteral("%1.id must be a non-empty kebab-case identifier.").arg(context));
    }

    if (!fixup.value(QStringLiteral("safe")).isBool())
    {
        return setValidationError(errorMessage, QStringLiteral("%1.safe must be a boolean.").arg(context));
    }

    const QJsonValue params = fixup.value(QStringLiteral("params"));
    if (!params.isUndefined() && !params.isNull() && !params.isObject())
    {
        return setValidationError(errorMessage, QStringLiteral("%1.params must be an object.").arg(context));
    }

    if (!fixup.value(QStringLiteral("description")).isString() || fixup.value(QStringLiteral("description")).toString().isEmpty())
    {
        return setValidationError(errorMessage, QStringLiteral("%1.description must be a non-empty string.").arg(context));
    }

    return true;
}

/// Top-level keys accepted on a normalized preflight report. Must stay in sync with
/// the "properties" object in loupe-preflight/schemas/report.schema.json; a parity
/// test (UnitTestsOperatorAcceptance) cross-checks the two so producer/validator
/// contract drift like coverage_scope/profile_identity/variable_bindings/error being
/// added to PreflightResult::toJson() without a matching allow-list update fails CI
/// instead of silently rejecting every real preflight report.
inline const QSet<QString>& normalizedReportAllowedProperties()
{
    static const QSet<QString> allowedProperties = {
        QStringLiteral("schema_kind"),
        QStringLiteral("schema_version"),
        QStringLiteral("inspection_complete"),
        QStringLiteral("pass"),
        QStringLiteral("profile"),
        QStringLiteral("engine_version"),
        QStringLiteral("pdf"),
        QStringLiteral("pdfx"),
        QStringLiteral("profile_resolution"),
        QStringLiteral("document_revision_digest"),
        QStringLiteral("effective_profile_digest"),
        QStringLiteral("decisions"),
        QStringLiteral("errors"),
        QStringLiteral("warnings"),
        QStringLiteral("fixups_available"),
        QStringLiteral("checks"),
        QStringLiteral("verdict"),
        QStringLiteral("error"),
        QStringLiteral("profile_identity"),
        QStringLiteral("coverage_scope"),
        QStringLiteral("variable_bindings")
    };
    return allowedProperties;
}

inline bool validateNormalizedReport(const QJsonObject& report, QString* errorMessage = nullptr)
{
    if (errorMessage)
    {
        errorMessage->clear();
    }

    const pdf::PDFSchemaEnvelope envelope = pdf::readSchemaEnvelope(report);
    pdf::PDFSchemaKind kind = envelope.kind;
    if (kind == pdf::PDFSchemaKind::Unknown)
    {
        kind = pdf::PDFSchemaKind::PreflightReport;
    }

    pdf::PDFSchemaVersion version = envelope.version;
    if (!version.isValid())
    {
        bool ok = false;
        version = pdf::PDFSchemaVersion::fromJsonValue(report.value(QStringLiteral("schema_version")), &ok);
        if (!ok)
        {
            return setValidationError(errorMessage, QStringLiteral("schema_version must be a supported version."));
        }
    }

    if (pdf::checkSchemaCompatibility(kind, version) == pdf::PDFSchemaCompatibility::UnsupportedMajor)
    {
        return setValidationError(errorMessage, QStringLiteral("schema_version is not supported."));
    }

    const pdf::PDFSchemaMigrationResult prepared = pdf::prepareSchemaDocument(kind, report);
    if (prepared.document.isEmpty())
    {
        return setValidationError(errorMessage, QStringLiteral("schema_version is not supported."));
    }

    const QJsonObject normalizedReport = prepared.document;
    const bool allowUnknownFields = pdf::checkSchemaCompatibility(kind, version) == pdf::PDFSchemaCompatibility::Compatible;

    if (!hasOnlyProperties(normalizedReport,
                           normalizedReportAllowedProperties(),
                           QStringLiteral("report"),
                           errorMessage,
                           allowUnknownFields))
    {
        return false;
    }

    const QJsonValue schemaVersion = normalizedReport.value(QStringLiteral("schema_version"));
    if (!isInteger(schemaVersion) || !isSupportedSchemaVersion(schemaVersion.toInt()))
    {
        return setValidationError(errorMessage, QStringLiteral("schema_version must be between 1 and %1.").arg(LOUPE_PREFLIGHT_SCHEMA_VERSION));
    }

    const int schemaVersionValue = schemaVersion.toInt();

    if (!normalizedReport.value(QStringLiteral("pass")).isBool())
    {
        return setValidationError(errorMessage, QStringLiteral("pass must be a boolean."));
    }

    if (schemaVersionValue >= 3)
    {
        if (!normalizedReport.value(QStringLiteral("inspection_complete")).isBool())
        {
            return setValidationError(errorMessage, QStringLiteral("inspection_complete must be a boolean."));
        }

        const QJsonValue checksValue = normalizedReport.value(QStringLiteral("checks"));
        if (!checksValue.isArray())
        {
            return setValidationError(errorMessage, QStringLiteral("checks must be an array."));
        }

        const QJsonArray checks = checksValue.toArray();
        for (int i = 0; i < checks.size(); ++i)
        {
            const QJsonObject checkObject = checks.at(i).toObject();
            if (!checkObject.value(QStringLiteral("id")).isString())
            {
                return setValidationError(errorMessage, QStringLiteral("checks[%1].id must be a string.").arg(i));
            }
            if (!checkObject.value(QStringLiteral("status")).isString())
            {
                return setValidationError(errorMessage, QStringLiteral("checks[%1].status must be a string.").arg(i));
            }
        }

        const QJsonObject verdict = normalizedReport.value(QStringLiteral("verdict")).toObject();
        const QString state = verdict.value(QStringLiteral("state")).toString();
        if (!QSet<QString>{ QStringLiteral("pass"), QStringLiteral("fail"), QStringLiteral("incomplete"), QStringLiteral("error") }.contains(state))
        {
            return setValidationError(errorMessage, QStringLiteral("verdict.state must be pass, fail, incomplete, or error."));
        }
        if (!verdict.value(QStringLiteral("reason_code")).isString() || !verdict.value(QStringLiteral("reason")).isString() || !verdict.value(QStringLiteral("blocking_finding_ids")).isArray() || !verdict.value(QStringLiteral("waived_finding_ids")).isArray())
        {
            return setValidationError(errorMessage, QStringLiteral("verdict must contain machine-readable reason and finding arrays."));
        }
    }

    if (!normalizedReport.value(QStringLiteral("profile")).isString() || normalizedReport.value(QStringLiteral("profile")).toString().isEmpty())
    {
        return setValidationError(errorMessage, QStringLiteral("profile must be a non-empty string."));
    }

    for (const QString& optionalString : { QStringLiteral("engine_version"), QStringLiteral("pdf") })
    {
        const QJsonValue value = normalizedReport.value(optionalString);
        if (!value.isUndefined() && !value.isString())
        {
            return setValidationError(errorMessage, QStringLiteral("%1 must be a string.").arg(optionalString));
        }
    }

    for (const QString& digestName : { QStringLiteral("document_revision_digest"), QStringLiteral("effective_profile_digest") })
    {
        const QJsonValue value = normalizedReport.value(digestName);
        if (!value.isUndefined() && (!value.isString() || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(value.toString()).hasMatch()))
        {
            return setValidationError(errorMessage, QStringLiteral("%1 must be a lowercase SHA-256 digest.").arg(digestName));
        }
    }

    const QJsonValue decisionsValue = normalizedReport.value(QStringLiteral("decisions"));
    if (!decisionsValue.isUndefined())
    {
        if (!decisionsValue.isArray())
        {
            return setValidationError(errorMessage, QStringLiteral("decisions must be an array."));
        }
        const QJsonArray decisions = decisionsValue.toArray();
        for (int i = 0; i < decisions.size(); ++i)
        {
            if (!decisions.at(i).isObject())
            {
                return setValidationError(errorMessage, QStringLiteral("decisions[%1] must be an object.").arg(i));
            }
        }
    }

    for (const QString& section : { QStringLiteral("errors"), QStringLiteral("warnings") })
    {
        const QJsonValue sectionValue = normalizedReport.value(section);
        if (!sectionValue.isArray())
        {
            return setValidationError(errorMessage, QStringLiteral("%1 must be an array.").arg(section));
        }

        const QJsonArray findings = sectionValue.toArray();
        for (int i = 0; i < findings.size(); ++i)
        {
            if (!validateFinding(findings.at(i), section, i, schemaVersionValue, errorMessage))
            {
                return false;
            }
        }
    }

    const QJsonValue fixupsValue = normalizedReport.value(QStringLiteral("fixups_available"));
    if (!fixupsValue.isArray())
    {
        return setValidationError(errorMessage, QStringLiteral("fixups_available must be an array."));
    }

    const QJsonArray fixups = fixupsValue.toArray();
    for (int i = 0; i < fixups.size(); ++i)
    {
        if (!validateFixup(fixups.at(i), i, errorMessage))
        {
            return false;
        }
    }

    bool expectedPass = normalizedReport.value(QStringLiteral("pass")).toBool();
    if (schemaVersionValue >= 3)
    {
        const QString state = normalizedReport.value(QStringLiteral("verdict")).toObject().value(QStringLiteral("state")).toString();
        expectedPass = state == QStringLiteral("pass");
    }

    if (normalizedReport.value(QStringLiteral("pass")).toBool() != expectedPass)
    {
        return setValidationError(errorMessage, QStringLiteral("pass must be derived from verdict.state."));
    }

    return true;
}

inline bool isNormalizedReport(const QJsonObject& report)
{
    return validateNormalizedReport(report);
}

/// Builds the overprint disclosure text shown in the report panel summary.
/// Standard page rendering never simulates overprint compositing (MIC-320) —
/// overprint-accurate compositing exists only in the transparency renderer
/// behind Output Preview. The preflight engine only detects the unsafe
/// white/near-white case; ordinary overprint (spot-over-process, rich black
/// over an image) produces no finding at all. So the base notice is always
/// shown once a report is loaded (MIC-330/R-002), regardless of findings,
/// with an additional specific warning appended when the white-overprint
/// finding is present.
inline QString overprintDisclosureText(bool hasWhiteOverprintFinding)
{
    QString text = QObject::tr("Page view does not simulate overprint — use Output Preview to proof "
                               "overprint-sensitive documents accurately.");

    if (hasWhiteOverprintFinding)
    {
        text += QStringLiteral(" ");
        text += QObject::tr("This document uses white or near-white overprint, which can "
                            "cause unintended knockouts in print.");
    }

    return text;
}

}   // namespace pdfplugin::preflight

#endif   // PREFLIGHTSIDECARUTILS_H
