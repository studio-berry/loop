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

#include "pdfschemaversion.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <utility>

#include <cmath>
#include <limits>

namespace pdf
{

namespace
{

QJsonObject loadCompatibilityMatrix()
{
    static const QJsonObject cached = []
    {
        QFile file(QStringLiteral(":/loop/schema-compatibility.json"));
        if (file.open(QIODevice::ReadOnly))
        {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error == QJsonParseError::NoError && document.isObject())
            {
                return document.object();
            }
        }
        return QJsonObject();
    }();
    return cached;
}

PDFSchemaVersion parseCurrentVersion(const QJsonObject& entry)
{
    bool ok = false;
    const PDFSchemaVersion version = PDFSchemaVersion::fromJsonValue(entry.value(QStringLiteral("current")), &ok);
    if (!ok)
    {
        // No readable `current` means no known current version. Deriving a major
        // from `supported_majors` would invent the version the matrix declined
        // to declare and relabel documents as current on its strength, so fail
        // closed with the same invalid version an absent entry yields.
        return {};
    }
    return version;
}

QString supportedMajorsText(const QJsonObject& matrix, PDFSchemaKind kind)
{
    const QJsonObject kinds = matrix.value(QStringLiteral("kinds")).toObject();
    const QJsonArray supported =
        kinds.value(pdfSchemaKindToString(kind)).toObject().value(QStringLiteral("supported_majors")).toArray();
    QStringList majors;
    for (const QJsonValue& major : supported)
    {
        majors.append(QString::number(major.toInt()));
    }
    return majors.join(QStringLiteral(", "));
}

QJsonObject migratePreflightReportV2ToV3(QJsonObject document)
{
    if (!document.contains(QStringLiteral("schema_kind")))
    {
        document.insert(QStringLiteral("schema_kind"), pdfSchemaKindToString(PDFSchemaKind::PreflightReport));
    }

    if (!document.contains(QStringLiteral("inspection_complete")))
    {
        const bool pass = document.value(QStringLiteral("pass")).toBool(false);
        const QJsonArray errors = document.value(QStringLiteral("errors")).toArray();
        document.insert(QStringLiteral("inspection_complete"), pass || !errors.isEmpty());
    }

    if (!document.contains(QStringLiteral("checks")))
    {
        document.insert(QStringLiteral("checks"), QJsonArray{});
    }

    if (!document.contains(QStringLiteral("verdict")))
    {
        const bool pass = document.value(QStringLiteral("pass")).toBool(false);
        const QJsonArray errors = document.value(QStringLiteral("errors")).toArray();
        QString state;
        QString reasonCode;
        QString reason;
        QJsonArray blockingFindingIds;
        if (pass)
        {
            state = QStringLiteral("pass");
            reasonCode = QStringLiteral("no-blocking-findings");
            reason = QStringLiteral("ok");
        }
        else if (!errors.isEmpty())
        {
            state = QStringLiteral("fail");
            reasonCode = QStringLiteral("blocking-findings");
            reason = QStringLiteral("Blocking findings were recorded.");
            for (const QJsonValue& item : errors)
            {
                const QString findingId = item.toObject().value(QStringLiteral("id")).toString();
                if (!findingId.isEmpty())
                {
                    blockingFindingIds.append(findingId);
                }
            }
        }
        else
        {
            state = QStringLiteral("incomplete");
            reasonCode = QStringLiteral("inspection-incomplete");
            reason = QStringLiteral("Required inspection evidence was not collected.");
        }
        document.insert(QStringLiteral("verdict"),
                        QJsonObject{
                            { QStringLiteral("state"), state },
                            { QStringLiteral("reason_code"), reasonCode },
                            { QStringLiteral("reason"), reason },
                            { QStringLiteral("blocking_finding_ids"), blockingFindingIds },
                            { QStringLiteral("waived_finding_ids"), QJsonArray{} } });
    }

    document.insert(QStringLiteral("schema_version"), 3);
    return document;
}

QJsonObject migratePreflightReportV1ToV2(QJsonObject document)
{
    if (!document.contains(QStringLiteral("schema_kind")))
    {
        document.insert(QStringLiteral("schema_kind"), pdfSchemaKindToString(PDFSchemaKind::PreflightReport));
    }
    document.insert(QStringLiteral("schema_version"), 2);
    return document;
}

QJsonObject migrateActionListV1ToV2(QJsonObject document)
{
    // Action List v2 keeps the v1 fields and adds selector-capable steps, so
    // the legacy recipe can advance without dropping unknown fields.
    document.insert(QStringLiteral("schema"), QStringLiteral("loop-action-list/2"));
    document.insert(QStringLiteral("schema_version"), 2);
    return document;
}

}   // namespace

QString PDFSchemaVersion::toString() const
{
    return QStringLiteral("%1.%2").arg(major).arg(minor);
}

PDFSchemaVersion PDFSchemaVersion::fromJsonValue(const QJsonValue& value, bool* ok)
{
    PDFSchemaVersion version;
    bool parsed = false;
    if (value.isDouble())
    {
        const double major = value.toDouble();
        if (std::isfinite(major) && std::floor(major) == major && major >= 1.0 && major <= double(std::numeric_limits<quint16>::max()))
        {
            version.major = static_cast<quint16>(major);
            version.minor = 0;
            parsed = true;
        }
    }
    else if (value.isString())
    {
        const QString text = value.toString().trimmed();
        const QStringList parts = text.split(QLatin1Char('.'));
        auto parseComponent = [](const QString& component, bool allowZero, quint16* result)
        {
            if (component.isEmpty())
            {
                return false;
            }
            for (const QChar character : component)
            {
                if (!character.isDigit())
                {
                    return false;
                }
            }
            bool converted = false;
            const quint64 value = component.toULongLong(&converted);
            if (!converted || value > std::numeric_limits<quint16>::max() || (!allowZero && value == 0))
            {
                return false;
            }
            *result = static_cast<quint16>(value);
            return true;
        };

        if (parts.size() == 1 || parts.size() == 2)
        {
            quint16 major = 0;
            quint16 minor = 0;
            if (parseComponent(parts.at(0), false, &major) &&
                (parts.size() == 1 || parseComponent(parts.at(1), true, &minor)))
            {
                version.major = major;
                version.minor = minor;
                parsed = true;
            }
        }
    }
    if (ok)
    {
        *ok = parsed;
    }
    return version;
}

QJsonValue PDFSchemaVersion::toJsonValue() const
{
    if (minor == 0)
    {
        return int(major);
    }
    return toString();
}

QString pdfSchemaKindToString(PDFSchemaKind kind)
{
    switch (kind)
    {
        case PDFSchemaKind::PreflightReport:
            return QStringLiteral("preflight-report");
        case PDFSchemaKind::PreflightProfile:
            return QStringLiteral("preflight-profile");
        case PDFSchemaKind::EvidenceGraph:
            return QStringLiteral("evidence-graph");
        case PDFSchemaKind::OperationPlan:
            return QStringLiteral("operation-plan");
        case PDFSchemaKind::OperationResult:
            return QStringLiteral("operation-result");
        case PDFSchemaKind::ProvenanceEvent:
            return QStringLiteral("provenance-event");
        case PDFSchemaKind::Certificate:
            return QStringLiteral("certificate");
        case PDFSchemaKind::CapabilityDiscovery:
            return QStringLiteral("capability-discovery");
        case PDFSchemaKind::PackageManifest:
            return QStringLiteral("package-manifest");
        case PDFSchemaKind::ActionList:
            return QStringLiteral("action-list");
        case PDFSchemaKind::PdfToolEnvelope:
            return QStringLiteral("pdftool-envelope");
        case PDFSchemaKind::OcrReport:
            return QStringLiteral("ocr-report");
        case PDFSchemaKind::HistoryDb:
            return QStringLiteral("history-db");
        case PDFSchemaKind::PageMasterManifest:
            return QStringLiteral("pagemaster-manifest");
        case PDFSchemaKind::PreflightDecisions:
            return QStringLiteral("preflight-decisions");
        case PDFSchemaKind::Unknown:
            break;
    }
    return QStringLiteral("unknown");
}

PDFSchemaKind pdfSchemaKindFromString(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("preflight-report"))
        return PDFSchemaKind::PreflightReport;
    if (normalized == QStringLiteral("preflight-profile"))
        return PDFSchemaKind::PreflightProfile;
    if (normalized == QStringLiteral("evidence-graph"))
        return PDFSchemaKind::EvidenceGraph;
    if (normalized == QStringLiteral("operation-plan"))
        return PDFSchemaKind::OperationPlan;
    if (normalized == QStringLiteral("operation-result"))
        return PDFSchemaKind::OperationResult;
    if (normalized == QStringLiteral("provenance-event"))
        return PDFSchemaKind::ProvenanceEvent;
    if (normalized == QStringLiteral("certificate"))
        return PDFSchemaKind::Certificate;
    if (normalized == QStringLiteral("capability-discovery"))
        return PDFSchemaKind::CapabilityDiscovery;
    if (normalized == QStringLiteral("package-manifest"))
        return PDFSchemaKind::PackageManifest;
    if (normalized == QStringLiteral("action-list"))
        return PDFSchemaKind::ActionList;
    if (normalized == QStringLiteral("pdftool-envelope"))
        return PDFSchemaKind::PdfToolEnvelope;
    if (normalized == QStringLiteral("ocr-report"))
        return PDFSchemaKind::OcrReport;
    if (normalized == QStringLiteral("history-db"))
        return PDFSchemaKind::HistoryDb;
    if (normalized == QStringLiteral("pagemaster-manifest"))
        return PDFSchemaKind::PageMasterManifest;
    if (normalized == QStringLiteral("preflight-decisions"))
        return PDFSchemaKind::PreflightDecisions;
    return PDFSchemaKind::Unknown;
}

PDFSchemaCompatibility checkSchemaCompatibilityWithMatrix(PDFSchemaKind kind,
                                                          PDFSchemaVersion version,
                                                          const QJsonObject& matrix)
{
    if (kind == PDFSchemaKind::Unknown)
    {
        return PDFSchemaCompatibility::UnknownKind;
    }
    if (!version.isValid())
    {
        return PDFSchemaCompatibility::Invalid;
    }

    const QJsonObject kinds = matrix.value(QStringLiteral("kinds")).toObject();
    QJsonObject entry = kinds.value(pdfSchemaKindToString(kind)).toObject();
    if (entry.isEmpty())
    {
        return PDFSchemaCompatibility::UnsupportedMajor;
    }

    const QJsonArray supported = entry.value(QStringLiteral("supported_majors")).toArray();
    bool majorSupported = false;
    for (const QJsonValue& item : supported)
    {
        if (item.toInt() == int(version.major))
        {
            majorSupported = true;
            break;
        }
    }
    if (!majorSupported)
    {
        return PDFSchemaCompatibility::UnsupportedMajor;
    }
    return PDFSchemaCompatibility::Compatible;
}

PDFSchemaCompatibility checkSchemaCompatibility(PDFSchemaKind kind, PDFSchemaVersion version)
{
    return checkSchemaCompatibilityWithMatrix(kind, version, loadCompatibilityMatrix());
}

QString pdfSchemaCompatibilityToString(PDFSchemaCompatibility compatibility)
{
    switch (compatibility)
    {
        case PDFSchemaCompatibility::Compatible:
            return QStringLiteral("compatible");
        case PDFSchemaCompatibility::UnsupportedMajor:
            return QStringLiteral("unsupported-major");
        case PDFSchemaCompatibility::UnknownKind:
            return QStringLiteral("unknown-kind");
        case PDFSchemaCompatibility::Invalid:
            break;
    }
    return QStringLiteral("invalid");
}

PDFSchemaCompatibilityDiagnostic schemaCompatibilityDiagnostic(PDFSchemaKind kind, PDFSchemaVersion version)
{
    PDFSchemaCompatibilityDiagnostic diagnostic;
    diagnostic.kind = kind;
    diagnostic.version = version;
    diagnostic.compatibility = checkSchemaCompatibility(kind, version);

    switch (diagnostic.compatibility)
    {
        case PDFSchemaCompatibility::Compatible:
            diagnostic.code = QStringLiteral("schema.compatible");
            diagnostic.message = QStringLiteral("Schema kind '%1' version %2 is supported.")
                                     .arg(pdfSchemaKindToString(kind), version.toString());
            break;
        case PDFSchemaCompatibility::UnsupportedMajor:
            diagnostic.code = QStringLiteral("schema.unsupported-major");
            // The message names the unsupported major; the full MAJOR.MINOR is
            // carried by `diagnostic.version` and reported as `schema_version`.
            diagnostic.message =
                QStringLiteral("Unsupported schema major: kind '%1' version %2; this build supports major(s) %3.")
                    .arg(pdfSchemaKindToString(kind), QString::number(version.major),
                         supportedMajorsText(loadCompatibilityMatrix(), kind));
            break;
        case PDFSchemaCompatibility::UnknownKind:
            diagnostic.code = QStringLiteral("schema.unknown-kind");
            diagnostic.message =
                QStringLiteral("Unknown schema kind: the document declares no recognised 'schema_kind'.");
            break;
        case PDFSchemaCompatibility::Invalid:
            diagnostic.code = QStringLiteral("schema.invalid-version");
            diagnostic.message = QStringLiteral(
                "Invalid schema version: 'schema_version' must be an integer major or a \"MAJOR.MINOR\" string.");
            break;
    }
    return diagnostic;
}

QJsonObject schemaCompatibilityMatrix()
{
    return loadCompatibilityMatrix();
}

PDFSchemaVersion currentSchemaVersion(PDFSchemaKind kind)
{
    return currentSchemaVersionWithMatrix(kind, loadCompatibilityMatrix());
}

PDFSchemaVersion currentSchemaVersionWithMatrix(PDFSchemaKind kind, const QJsonObject& matrix)
{
    const QJsonObject kinds = matrix.value(QStringLiteral("kinds")).toObject();
    const QJsonObject entry = kinds.value(pdfSchemaKindToString(kind)).toObject();
    if (entry.isEmpty())
    {
        return {};
    }
    return parseCurrentVersion(entry);
}

QJsonObject migrateSchemaDocument(PDFSchemaKind kind, PDFSchemaVersion from, QJsonObject document)
{
    if (kind == PDFSchemaKind::PreflightReport)
    {
        if (from.major == 1)
        {
            document = migratePreflightReportV1ToV2(std::move(document));
            from = { 2, 0 };
        }
        if (from.major == 2)
        {
            document = migratePreflightReportV2ToV3(std::move(document));
        }
        return document;
    }

    if (kind == PDFSchemaKind::ActionList && from.major == 1)
    {
        return migrateActionListV1ToV2(std::move(document));
    }

    Q_UNUSED(from);
    return document;
}

PDFSchemaMigrationResult prepareSchemaDocument(PDFSchemaKind kind, QJsonObject document)
{
    PDFSchemaMigrationResult result;
    result.document = std::move(document);

    PDFSchemaEnvelope envelope = readSchemaEnvelope(result.document);
    if (envelope.kind == PDFSchemaKind::Unknown && kind != PDFSchemaKind::Unknown)
    {
        envelope.kind = kind;
    }
    if (envelope.kind == PDFSchemaKind::Unknown)
    {
        // Neither the document nor the caller identifies the contract. Guessing
        // a kind would interpret unknown bytes as a preflight report.
        result.document = {};
        return result;
    }

    if (!envelope.version.isValid())
    {
        bool ok = false;
        envelope.version = PDFSchemaVersion::fromJsonValue(result.document.value(QStringLiteral("schema_version")), &ok);
        if (!ok)
        {
            return result;
        }
    }

    if (checkSchemaCompatibility(envelope.kind, envelope.version) != PDFSchemaCompatibility::Compatible)
    {
        result.document = {};
        return result;
    }

    const PDFSchemaVersion target = currentSchemaVersion(envelope.kind);
    result.fromVersion = envelope.version;
    // toVersion is the version the document is at when this returns; only a
    // migration moves it. Reporting the matrix target here would tell a caller
    // it holds current bytes while it holds a newer minor payload.
    result.toVersion = envelope.version;

    while (envelope.version.major < target.major)
    {
        const PDFSchemaVersion previousVersion = envelope.version;
        result.document = migrateSchemaDocument(envelope.kind, envelope.version, result.document);
        envelope = readSchemaEnvelope(result.document);
        if (!envelope.version.isValid())
        {
            envelope.version = PDFSchemaVersion::fromJsonValue(result.document.value(QStringLiteral("schema_version")));
        }

        // A kind can be declared compatible before its migrator ships. Never
        // spin forever or silently relabel such a document as current: a
        // migration step must produce a valid, strictly newer supported major.
        if (!envelope.version.isValid() || envelope.version.major <= previousVersion.major ||
            envelope.version.major > target.major)
        {
            result.document = {};
            result.migrated = false;
            return result;
        }
        result.migrated = true;
    }

    if (result.migrated)
    {
        writeSchemaEnvelope(result.document, envelope.kind, target);
        result.toVersion = target;
    }

    return result;
}

PDFSchemaEnvelope readSchemaEnvelope(const QJsonObject& document)
{
    PDFSchemaEnvelope envelope;
    envelope.kind = pdfSchemaKindFromString(document.value(QStringLiteral("schema_kind")).toString());
    bool ok = false;
    envelope.version = PDFSchemaVersion::fromJsonValue(document.value(QStringLiteral("schema_version")), &ok);
    if (!ok)
    {
        envelope.version = {};
    }
    return envelope;
}

void writeSchemaEnvelope(QJsonObject& document, PDFSchemaKind kind, PDFSchemaVersion version)
{
    document.insert(QStringLiteral("schema_kind"), pdfSchemaKindToString(kind));
    document.insert(QStringLiteral("schema_version"), version.toJsonValue());
}

}   // namespace pdf
