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

#include "pdfoperationhistorystore.h"
#include "pdfartifactstore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <utility>

namespace pdf
{

namespace
{

constexpr int CurrentSchemaVersion = 3;

QString connectionName()
{
    return QStringLiteral("loupe-operation-history-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString queryError(const QSqlQuery& query)
{
    return query.lastError().text().isEmpty() ? QStringLiteral("SQLite operation failed.") : query.lastError().text();
}

QString databaseError(const QSqlDatabase& database)
{
    return database.lastError().text().isEmpty() ? QStringLiteral("SQLite database operation failed.") : database.lastError().text();
}

bool exec(QSqlDatabase& database, const QString& sql, QString* errorMessage)
{
    QSqlQuery query(database);
    if (query.exec(sql))
    {
        return true;
    }
    if (errorMessage)
    {
        *errorMessage = queryError(query);
    }
    return false;
}

QString dateTimeString(const QDateTime& value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime dateTimeFromString(const QString& value)
{
    return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
}

QByteArray decodeHash(const QString& value)
{
    return QByteArray::fromHex(value.toLatin1());
}

QJsonObject parseObject(const QString& value)
{
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8());
    return document.isObject() ? document.object() : QJsonObject();
}

}   // namespace

class PDFOperationHistoryStore::Impl
{
public:
    QString connection;
    QSqlDatabase database;
};

PDFOperationHistoryStore::PDFOperationHistoryStore(QString databasePath,
                                                   PDFOperationHistoryStoreOptions options) :
    m_impl(std::make_unique<Impl>()),
    m_databasePath(std::move(databasePath)),
    m_options(options)
{
}

PDFOperationHistoryStore::~PDFOperationHistoryStore()
{
    close();
}

PDFOperationResult PDFOperationHistoryStore::open(QString* errorMessage)
{
    if (isOpen())
    {
        return true;
    }
    if (m_databasePath.isEmpty())
    {
        const QString error = QStringLiteral("Operation history database path is empty.");
        if (errorMessage)
            *errorMessage = error;
        return PDFOperationResult(error);
    }

    if (m_databasePath != QStringLiteral(":memory:"))
    {
        const QFileInfo info(m_databasePath);
        if (!QDir().mkpath(info.absolutePath()))
        {
            const QString error = QStringLiteral("Could not create the operation history database directory.");
            if (errorMessage)
                *errorMessage = error;
            return PDFOperationResult(error);
        }
    }

    m_impl->connection = connectionName();
    m_impl->database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_impl->connection);
    m_impl->database.setDatabaseName(m_databasePath);
    if (!m_impl->database.open())
    {
        const QString error = databaseError(m_impl->database);
        if (errorMessage)
            *errorMessage = error;
        close();
        return PDFOperationResult(error);
    }

    QString error;
    const int busyTimeout = qBound(0, m_options.busyTimeoutMs, 60000);
    if (!exec(m_impl->database, QStringLiteral("PRAGMA foreign_keys = ON"), &error) ||
        !exec(m_impl->database, QStringLiteral("PRAGMA journal_mode = WAL"), &error) ||
        !exec(m_impl->database, QStringLiteral("PRAGMA busy_timeout = %1").arg(busyTimeout), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS schema_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"), &error))
    {
        if (errorMessage)
            *errorMessage = error;
        close();
        return PDFOperationResult(error);
    }

    int schemaVersion = 0;
    QSqlQuery versionQuery(m_impl->database);
    if (!versionQuery.exec(QStringLiteral("SELECT value FROM schema_meta WHERE key = 'schema_version'")))
    {
        error = queryError(versionQuery);
    }
    else if (versionQuery.next())
    {
        bool ok = false;
        schemaVersion = versionQuery.value(0).toInt(&ok);
        if (!ok || schemaVersion > CurrentSchemaVersion)
        {
            error = QStringLiteral("Operation history database schema is newer than this application.");
        }
    }
    if (!error.isEmpty())
    {
        if (errorMessage)
            *errorMessage = error;
        close();
        return PDFOperationResult(error);
    }

    if (!exec(m_impl->database, QStringLiteral("BEGIN IMMEDIATE"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS artifacts (sha256 TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL, media_type TEXT NOT NULL, logical_name TEXT, storage_token TEXT, created_utc TEXT NOT NULL, is_original_input INTEGER NOT NULL DEFAULT 0, artifact_evicted INTEGER NOT NULL DEFAULT 0)"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS executions (execution_id TEXT PRIMARY KEY, parent_execution_id TEXT, operation_id TEXT NOT NULL, operation_version INTEGER NOT NULL, source_sha256 TEXT NOT NULL, source_revision INTEGER NOT NULL, parameters_json TEXT NOT NULL, started_utc TEXT NOT NULL, FOREIGN KEY(source_sha256) REFERENCES artifacts(sha256), FOREIGN KEY(parent_execution_id) REFERENCES executions(execution_id))"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS history_events (sequence INTEGER PRIMARY KEY AUTOINCREMENT, entry_id TEXT NOT NULL UNIQUE, execution_id TEXT NOT NULL, event_kind TEXT NOT NULL DEFAULT 'operation', status TEXT NOT NULL, operator_identity TEXT NOT NULL DEFAULT '', document_revision_digest TEXT NOT NULL DEFAULT '', effective_profile_digest TEXT NOT NULL DEFAULT '', result_json TEXT NOT NULL, output_sha256 TEXT, finding_ids_json TEXT NOT NULL, report_sha256 TEXT, diff_sha256 TEXT, approval_json TEXT NOT NULL, previous_event_hash TEXT NOT NULL, event_hash TEXT NOT NULL, created_utc TEXT NOT NULL, FOREIGN KEY(execution_id) REFERENCES executions(execution_id), FOREIGN KEY(output_sha256) REFERENCES artifacts(sha256))"), &error) ||
        (schemaVersion == 1 && (!exec(m_impl->database, QStringLiteral("ALTER TABLE artifacts ADD COLUMN is_original_input INTEGER NOT NULL DEFAULT 0"), &error) ||
                                !exec(m_impl->database, QStringLiteral("ALTER TABLE artifacts ADD COLUMN artifact_evicted INTEGER NOT NULL DEFAULT 0"), &error))) ||
        (schemaVersion > 0 && schemaVersion < 3 &&
         (!exec(m_impl->database, QStringLiteral("ALTER TABLE history_events ADD COLUMN event_kind TEXT NOT NULL DEFAULT 'operation'"), &error) ||
          !exec(m_impl->database, QStringLiteral("ALTER TABLE history_events ADD COLUMN operator_identity TEXT NOT NULL DEFAULT ''"), &error) ||
          !exec(m_impl->database, QStringLiteral("ALTER TABLE history_events ADD COLUMN document_revision_digest TEXT NOT NULL DEFAULT ''"), &error) ||
          !exec(m_impl->database, QStringLiteral("ALTER TABLE history_events ADD COLUMN effective_profile_digest TEXT NOT NULL DEFAULT ''"), &error))) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS rollback_points (rollback_id TEXT PRIMARY KEY, audit_event_id TEXT, document_revision_digest TEXT NOT NULL, created_utc TEXT NOT NULL, artifact_path TEXT NOT NULL, artifact_bytes INTEGER NOT NULL, operation_id TEXT NOT NULL, plan_summary TEXT NOT NULL, is_original_input INTEGER NOT NULL DEFAULT 0, approved_output INTEGER NOT NULL DEFAULT 0, artifact_evicted INTEGER NOT NULL DEFAULT 0, evicted_utc TEXT, FOREIGN KEY(audit_event_id) REFERENCES history_events(entry_id), FOREIGN KEY(document_revision_digest) REFERENCES artifacts(sha256))"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_history_execution ON history_events(execution_id, sequence)"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_execution_source ON executions(source_sha256, source_revision)"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_execution_operation ON executions(operation_id, started_utc)"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_rollback_digest ON rollback_points(document_revision_digest)"), &error) ||
        !exec(m_impl->database, QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) VALUES('schema_version', '2')"), &error) ||
        !exec(m_impl->database, QStringLiteral("COMMIT"), &error))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        if (errorMessage)
            *errorMessage = error;
        close();
        return PDFOperationResult(error);
    }
    return true;
}

void PDFOperationHistoryStore::close()
{
    if (!m_impl || m_impl->connection.isEmpty())
    {
        return;
    }
    const QString connection = m_impl->connection;
    if (m_impl->database.isValid())
    {
        m_impl->database.close();
    }
    m_impl->database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    m_impl->connection.clear();
}

bool PDFOperationHistoryStore::isOpen() const
{
    return m_impl && m_impl->database.isValid() && m_impl->database.isOpen();
}

PDFOperationResult PDFOperationHistoryStore::registerArtifact(const PDFArtifactIdentity& artifact,
                                                              PDFArtifactRegistrationOptions options)
{
    if (!isOpen())
        return PDFOperationResult(QStringLiteral("Operation history store is not open."));
    if (!artifact.isValid())
        return PDFOperationResult(QStringLiteral("Artifact identity is invalid."));
    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO artifacts(sha256, size_bytes, media_type, logical_name, storage_token, created_utc, is_original_input, artifact_evicted) VALUES(?, ?, ?, ?, ?, ?, ?, 0)"));
    query.addBindValue(artifact.sha256.toLower());
    query.addBindValue(artifact.size);
    query.addBindValue(artifact.mediaType);
    query.addBindValue(sanitizeArtifactLogicalName(artifact.logicalName));
    query.addBindValue(artifact.storageToken);
    query.addBindValue(dateTimeString(QDateTime::currentDateTimeUtc()));
    query.addBindValue(options.isOriginalInput ? 1 : 0);
    if (!query.exec())
        return PDFOperationResult(queryError(query));

    QSqlQuery check(m_impl->database);
    check.prepare(QStringLiteral("UPDATE artifacts SET is_original_input = MAX(is_original_input, ?), artifact_evicted = 0 WHERE sha256 = ?"));
    check.addBindValue(options.isOriginalInput ? 1 : 0);
    check.addBindValue(artifact.sha256.toLower());
    if (!check.exec())
        return PDFOperationResult(queryError(check));

    QSqlQuery metadata(m_impl->database);
    metadata.prepare(QStringLiteral("SELECT size_bytes, media_type FROM artifacts WHERE sha256 = ?"));
    metadata.addBindValue(artifact.sha256.toLower());
    if (!metadata.exec() || !metadata.next())
        return PDFOperationResult(queryError(metadata));
    if (metadata.value(0).toLongLong() != artifact.size || metadata.value(1).toString() != artifact.mediaType)
    {
        return PDFOperationResult(QStringLiteral("Artifact digest is already registered with different immutable metadata."));
    }
    return true;
}

PDFOperationResult PDFOperationHistoryStore::registerOriginalInput(const PDFArtifactIdentity& artifact)
{
    if (const PDFOperationResult result = registerArtifact(artifact, { true }); !result)
    {
        return result;
    }

    QSqlQuery existing(m_impl->database);
    existing.prepare(QStringLiteral("SELECT 1 FROM rollback_points WHERE document_revision_digest = ? AND is_original_input = 1 LIMIT 1"));
    existing.addBindValue(artifact.sha256.toLower());
    if (!existing.exec())
        return PDFOperationResult(queryError(existing));
    if (existing.next())
        return true;

    QSqlQuery point(m_impl->database);
    point.prepare(QStringLiteral("INSERT INTO rollback_points(rollback_id, audit_event_id, document_revision_digest, created_utc, artifact_path, artifact_bytes, operation_id, plan_summary, is_original_input, approved_output, artifact_evicted) VALUES(?, NULL, ?, ?, ?, ?, ?, ?, 1, 0, 0)"));
    point.addBindValue(QStringLiteral("original-%1").arg(artifact.sha256.toLower()));
    point.addBindValue(artifact.sha256.toLower());
    point.addBindValue(dateTimeString(QDateTime::currentDateTimeUtc()));
    point.addBindValue(artifact.storageToken);
    point.addBindValue(artifact.size);
    point.addBindValue(QStringLiteral("input"));
    point.addBindValue(QStringLiteral("Original input"));
    return point.exec() ? PDFOperationResult(true) : PDFOperationResult(queryError(point));
}

PDFOperationResult PDFOperationHistoryStore::beginExecution(PDFOperationHistoryExecution execution,
                                                            QUuid* executionId)
{
    if (!isOpen())
        return PDFOperationResult(QStringLiteral("Operation history store is not open."));
    if (execution.executionId.isNull())
        execution.executionId = QUuid::createUuid();
    if (execution.operationId.trimmed().isEmpty() || execution.operationVersion <= 0 || !execution.input.isValid())
    {
        return PDFOperationResult(QStringLiteral("Execution identity or source artifact is invalid."));
    }
    if (!execution.startedUtc.isValid())
        execution.startedUtc = QDateTime::currentDateTimeUtc();

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("INSERT INTO executions(execution_id, parent_execution_id, operation_id, operation_version, source_sha256, source_revision, parameters_json, started_utc) VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(execution.executionId.toString(QUuid::WithoutBraces));
    query.addBindValue(execution.parentExecutionId
                           ? QVariant(execution.parentExecutionId->toString(QUuid::WithoutBraces))
                           : QVariant());
    query.addBindValue(execution.operationId.trimmed());
    query.addBindValue(execution.operationVersion);
    query.addBindValue(execution.input.sha256.toLower());
    query.addBindValue(qulonglong(execution.sourceDocumentRevision));
    query.addBindValue(QString::fromUtf8(canonicalJson(redactSensitiveJson(execution.parameters))));
    query.addBindValue(dateTimeString(execution.startedUtc));
    if (!query.exec())
        return PDFOperationResult(queryError(query));
    if (executionId)
        *executionId = execution.executionId;
    return true;
}

PDFOperationResult PDFOperationHistoryStore::appendEvent(PDFOperationHistoryEvent event,
                                                         qint64* sequence)
{
    if (!isOpen())
        return PDFOperationResult(QStringLiteral("Operation history store is not open."));
    if (event.entryId.isNull())
        event.entryId = QUuid::createUuid();
    if (event.executionId.isNull() || !event.approval.isValid())
        return PDFOperationResult(QStringLiteral("History event identity or approval is invalid."));
    if (!event.createdUtc.isValid())
        event.createdUtc = QDateTime::currentDateTimeUtc();
    if (event.kind == PDFOperationHistoryEventKind::Operation &&
        (event.status == PDFOperationHistoryStatus::Accepted || event.status == PDFOperationHistoryStatus::RolledBack) && !event.output)
    {
        return PDFOperationResult(QStringLiteral("Accepted history events require a durable output artifact."));
    }
    if (event.output && !event.output->isValid())
        return PDFOperationResult(QStringLiteral("History output artifact identity is invalid."));
    if ((!event.reportArtifactSha256.isEmpty() && !isPDFSha256(event.reportArtifactSha256)) ||
        (!event.diffArtifactSha256.isEmpty() && !isPDFSha256(event.diffArtifactSha256)) ||
        (!event.documentRevisionDigest.isEmpty() && !isPDFSha256(event.documentRevisionDigest)) ||
        (!event.effectiveProfileDigest.isEmpty() && !isPDFSha256(event.effectiveProfileDigest)))
    {
        return PDFOperationResult(QStringLiteral("History evidence digest is invalid."));
    }

    QString error;
    if (!exec(m_impl->database, QStringLiteral("BEGIN IMMEDIATE"), &error))
        return PDFOperationResult(error);
    QSqlQuery previousQuery(m_impl->database);
    if (!previousQuery.exec(QStringLiteral("SELECT event_hash FROM history_events ORDER BY sequence DESC LIMIT 1")))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        return PDFOperationResult(queryError(previousQuery));
    }
    const QByteArray previousHash = previousQuery.next() ? decodeHash(previousQuery.value(0).toString()) : QByteArray();
    event.previousEventHash = previousHash;

    // The event hash must be computed over exactly what verify() can later
    // reconstruct from storage. result_json is persisted redacted (see the
    // bind below), so the redaction has to happen before hashing too -
    // otherwise verify() can never reproduce the original hash for any
    // event with sensitive resultSummary fields, since the unredacted
    // content is never persisted anywhere to read back.
    event.resultSummary = redactSensitiveJson(event.resultSummary).toObject();
    event.eventHash = computeOperationHistoryEventHash(event, previousHash);

    // history_events.operator_identity/document_revision_digest/effective_profile_digest
    // are TEXT NOT NULL DEFAULT '', and previous_event_hash/event_hash are TEXT
    // NOT NULL (no default) - a default-constructed (null) QString binds as SQL
    // NULL via the SQLite driver even though QString::isEmpty() is true for it,
    // and DEFAULT only applies when a column is omitted from the INSERT, not
    // when it's explicitly bound to NULL. previous_event_hash in particular is
    // null on the very first event ever appended (QByteArray().toHex() -> a
    // null QString via fromLatin1), so coerce null QStrings to a non-null
    // empty string before binding.
    const auto nonNullString = [](const QString& value)
    {
        return value.isNull() ? QString(QLatin1String("")) : value;
    };

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("INSERT INTO history_events(entry_id, execution_id, event_kind, status, operator_identity, document_revision_digest, effective_profile_digest, result_json, output_sha256, finding_ids_json, report_sha256, diff_sha256, approval_json, previous_event_hash, event_hash, created_utc) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event.entryId.toString(QUuid::WithoutBraces));
    query.addBindValue(event.executionId.toString(QUuid::WithoutBraces));
    query.addBindValue(pdfOperationHistoryEventKindToString(event.kind));
    query.addBindValue(pdfOperationHistoryStatusToString(event.status));
    query.addBindValue(nonNullString(event.operatorIdentity));
    query.addBindValue(nonNullString(event.documentRevisionDigest.toLower()));
    query.addBindValue(nonNullString(event.effectiveProfileDigest.toLower()));
    query.addBindValue(QString::fromUtf8(canonicalJson(redactSensitiveJson(event.resultSummary))));
    query.addBindValue(event.output ? event.output->sha256.toLower() : QVariant());
    query.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(event.findingIds)).toJson(QJsonDocument::Compact)));
    query.addBindValue(event.reportArtifactSha256.toLower());
    query.addBindValue(event.diffArtifactSha256.toLower());
    query.addBindValue(QString::fromUtf8(canonicalJson(event.approval.toJson())));
    query.addBindValue(nonNullString(QString::fromLatin1(previousHash.toHex())));
    query.addBindValue(nonNullString(QString::fromLatin1(event.eventHash.toHex())));
    query.addBindValue(dateTimeString(event.createdUtc));
    if (!query.exec())
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        return PDFOperationResult(queryError(query));
    }
    if (event.status == PDFOperationHistoryStatus::Accepted || event.status == PDFOperationHistoryStatus::RolledBack)
    {
        QSqlQuery executionQuery(m_impl->database);
        executionQuery.prepare(QStringLiteral("SELECT operation_id FROM executions WHERE execution_id = ?"));
        executionQuery.addBindValue(event.executionId.toString(QUuid::WithoutBraces));
        if (!executionQuery.exec() || !executionQuery.next())
        {
            exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
            return PDFOperationResult(queryError(executionQuery));
        }

        QSqlQuery point(m_impl->database);
        point.prepare(QStringLiteral("INSERT INTO rollback_points(rollback_id, audit_event_id, document_revision_digest, created_utc, artifact_path, artifact_bytes, operation_id, plan_summary, is_original_input, approved_output, artifact_evicted) SELECT ?, ?, a.sha256, ?, a.storage_token, a.size_bytes, ?, ?, 0, ?, 0 FROM artifacts a WHERE a.sha256 = ?"));
        point.addBindValue(QStringLiteral("revision-%1").arg(event.entryId.toString(QUuid::WithoutBraces)));
        point.addBindValue(event.entryId.toString(QUuid::WithoutBraces));
        point.addBindValue(dateTimeString(event.createdUtc));
        point.addBindValue(executionQuery.value(0).toString());
        point.addBindValue(QString::fromUtf8(canonicalJson(redactSensitiveJson(event.resultSummary))));
        point.addBindValue(event.approval.kind != PDFApprovalKind::None && event.approval.decision.trimmed().compare(QStringLiteral("reject"), Qt::CaseInsensitive) != 0 ? 1 : 0);
        point.addBindValue(event.output->sha256.toLower());
        if (!point.exec() || point.numRowsAffected() != 1)
        {
            exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
            return PDFOperationResult(point.numRowsAffected() == 0
                                          ? QStringLiteral("History output artifact is not registered.")
                                          : queryError(point));
        }
    }
    if (!exec(m_impl->database, QStringLiteral("COMMIT"), &error))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        return PDFOperationResult(error);
    }
    event.sequence = query.lastInsertId().toLongLong();
    if (sequence)
        *sequence = event.sequence;
    return true;
}

QList<PDFOperationHistoryEvent> PDFOperationHistoryStore::events(QString* errorMessage) const
{
    QList<PDFOperationHistoryEvent> result;
    if (!isOpen())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Operation history store is not open.");
        return result;
    }
    QSqlQuery query(m_impl->database);
    if (!query.exec(QStringLiteral("SELECT h.sequence, h.entry_id, h.execution_id, h.event_kind, h.status, h.operator_identity, h.document_revision_digest, h.effective_profile_digest, h.result_json, h.output_sha256, h.finding_ids_json, h.report_sha256, h.diff_sha256, h.approval_json, h.previous_event_hash, h.event_hash, h.created_utc, a.size_bytes, a.media_type, a.logical_name, a.storage_token FROM history_events h LEFT JOIN artifacts a ON a.sha256 = h.output_sha256 ORDER BY h.sequence")))
    {
        if (errorMessage)
            *errorMessage = queryError(query);
        return result;
    }
    while (query.next())
    {
        PDFOperationHistoryEvent event;
        event.sequence = query.value(0).toLongLong();
        event.entryId = QUuid(query.value(1).toString());
        event.executionId = QUuid(query.value(2).toString());
        event.kind = pdfOperationHistoryEventKindFromString(query.value(3).toString());
        event.status = pdfOperationHistoryStatusFromString(query.value(4).toString());
        event.operatorIdentity = query.value(5).toString();
        event.documentRevisionDigest = query.value(6).toString();
        event.effectiveProfileDigest = query.value(7).toString();
        event.resultSummary = parseObject(query.value(8).toString());
        const QString outputSha = query.value(9).toString();
        if (!outputSha.isEmpty() && !query.value(17).isNull())
        {
            PDFArtifactIdentity artifact;
            artifact.sha256 = outputSha;
            artifact.size = query.value(17).toLongLong();
            artifact.mediaType = query.value(18).toString();
            artifact.logicalName = query.value(19).toString();
            artifact.storageToken = query.value(20).toString();
            event.output = artifact;
        }
        const QJsonDocument findings = QJsonDocument::fromJson(query.value(10).toString().toUtf8());
        for (const QJsonValue& finding : findings.array())
            event.findingIds.append(finding.toString());
        event.reportArtifactSha256 = query.value(11).toString();
        event.diffArtifactSha256 = query.value(12).toString();
        event.approval = PDFApprovalRecord::fromJson(parseObject(query.value(13).toString()));
        event.previousEventHash = decodeHash(query.value(14).toString());
        event.eventHash = decodeHash(query.value(15).toString());
        event.createdUtc = dateTimeFromString(query.value(16).toString());
        result.append(std::move(event));
    }
    return result;
}

PDFOperationHistoryVerification PDFOperationHistoryStore::verify() const
{
    PDFOperationHistoryVerification verification;
    QString error;
    const QList<PDFOperationHistoryEvent> history = events(&error);
    if (!error.isEmpty())
    {
        verification.errorMessage = error;
        verification.integrity = QStringLiteral("unavailable");
        return verification;
    }
    verification.verified = true;
    verification.integrity = QStringLiteral("verified");
    verification.eventsChecked = history.size();
    if (!history.isEmpty())
    {
        verification.firstSequence = history.front().sequence;
        verification.lastSequence = history.back().sequence;
    }
    QByteArray previous;
    qint64 expectedSequence = history.isEmpty() ? 0 : 1;
    for (const PDFOperationHistoryEvent& event : history)
    {
        if (event.sequence != expectedSequence || event.previousEventHash != previous ||
            event.eventHash != computeOperationHistoryEventHash(event, previous))
        {
            verification.verified = false;
            verification.integrity = QStringLiteral("compromised");
            verification.errorMessage = QStringLiteral("Operation history hash chain verification failed at sequence %1.").arg(event.sequence);
            return verification;
        }
        previous = event.eventHash;
        ++expectedSequence;
    }
    return verification;
}

QList<PDFRollbackPoint> PDFOperationHistoryStore::rollbackPoints(QString* errorMessage) const
{
    QList<PDFRollbackPoint> result;
    if (!isOpen())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Operation history store is not open.");
        return result;
    }

    QSqlQuery query(m_impl->database);
    if (!query.exec(QStringLiteral("SELECT rollback_id, audit_event_id, document_revision_digest, created_utc, artifact_path, artifact_bytes, operation_id, plan_summary, is_original_input, approved_output, artifact_evicted, evicted_utc FROM rollback_points ORDER BY created_utc, rollback_id")))
    {
        if (errorMessage)
            *errorMessage = queryError(query);
        return result;
    }
    while (query.next())
    {
        PDFRollbackPoint point;
        point.rollbackId = query.value(0).toString();
        point.auditEventId = QUuid(query.value(1).toString());
        point.documentRevisionDigest = query.value(2).toString();
        point.createdAtUtc = dateTimeFromString(query.value(3).toString());
        point.artifactPath = query.value(4).toString();
        point.artifactBytes = query.value(5).toLongLong();
        point.operationId = query.value(6).toString();
        point.planSummary = query.value(7).toString();
        point.isOriginalInput = query.value(8).toInt() != 0;
        point.approvedOutput = query.value(9).toInt() != 0;
        point.artifactEvicted = query.value(10).toInt() != 0;
        point.evictedAtUtc = dateTimeFromString(query.value(11).toString());
        result.append(std::move(point));
    }
    return result;
}

PDFHistoryRetentionResult PDFOperationHistoryStore::enforceRetention(const PDFHistoryRetentionPolicy& policy,
                                                                     const PDFArtifactStore& artifacts,
                                                                     QDateTime nowUtc)
{
    PDFHistoryRetentionResult result;
    if (!isOpen())
    {
        result.errorMessage = QStringLiteral("Operation history store is not open.");
        return result;
    }
    if (policy.maxPointsPerJob < 0 || policy.maxBytesPerJob < 0 || policy.maxAgeDays < 0)
    {
        result.errorMessage = QStringLiteral("History retention limits must not be negative.");
        return result;
    }
    if (!nowUtc.isValid())
        nowUtc = QDateTime::currentDateTimeUtc();

    QString error;
    if (!exec(m_impl->database, QStringLiteral("BEGIN IMMEDIATE"), &error))
    {
        result.errorMessage = error;
        return result;
    }

    QSqlQuery query(m_impl->database);
    if (!query.exec(QStringLiteral("SELECT rollback_id, document_revision_digest, created_utc, artifact_bytes, is_original_input, approved_output, artifact_evicted FROM rollback_points WHERE artifact_evicted = 0 ORDER BY created_utc, rollback_id")))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        result.errorMessage = queryError(query);
        return result;
    }

    struct Candidate
    {
        QString id;
        QString digest;
        QDateTime created;
        qint64 bytes = 0;
        bool original = false;
        bool approved = false;
    };
    QList<Candidate> points;
    qint64 activeBytes = 0;
    while (query.next())
    {
        Candidate candidate;
        candidate.id = query.value(0).toString();
        candidate.digest = query.value(1).toString();
        candidate.created = dateTimeFromString(query.value(2).toString());
        candidate.bytes = query.value(3).toLongLong();
        candidate.original = query.value(4).toInt() != 0;
        candidate.approved = query.value(5).toInt() != 0;
        activeBytes += candidate.bytes;
        points.append(std::move(candidate));
    }

    const QDateTime cutoff = nowUtc.addDays(-policy.maxAgeDays);
    for (const Candidate& candidate : points)
    {
        const bool protectedPoint = (policy.keepOriginalInput && candidate.original) ||
                                    (policy.keepApprovedOutputs && candidate.approved);
        const bool overCount = points.size() - result.pointsEvicted > policy.maxPointsPerJob;
        const bool overBytes = activeBytes > policy.maxBytesPerJob;
        const bool expired = candidate.created.isValid() && candidate.created < cutoff;
        if (protectedPoint || (!overCount && !overBytes && !expired))
        {
            continue;
        }

        QSqlQuery references(m_impl->database);
        references.prepare(QStringLiteral("SELECT COUNT(*) FROM rollback_points WHERE document_revision_digest = ? AND artifact_evicted = 0 AND rollback_id <> ?"));
        references.addBindValue(candidate.digest);
        references.addBindValue(candidate.id);
        if (!references.exec() || !references.next())
        {
            exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
            result.errorMessage = queryError(references);
            return result;
        }
        const bool shared = references.value(0).toInt() > 0;
        if (!shared && artifacts.contains(PDFArtifactIdentity{ candidate.digest, candidate.bytes, QStringLiteral("application/pdf"), {}, {} }))
        {
            PDFArtifactIdentity identity;
            identity.sha256 = candidate.digest;
            identity.size = candidate.bytes;
            identity.mediaType = QStringLiteral("application/pdf");
            if (!artifacts.remove(identity))
            {
                exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
                result.errorMessage = QStringLiteral("Could not evict rollback artifact '%1'.").arg(candidate.digest);
                return result;
            }
            QSqlQuery artifactUpdate(m_impl->database);
            artifactUpdate.prepare(QStringLiteral("UPDATE artifacts SET artifact_evicted = 1 WHERE sha256 = ?"));
            artifactUpdate.addBindValue(candidate.digest);
            if (!artifactUpdate.exec())
            {
                exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
                result.errorMessage = queryError(artifactUpdate);
                return result;
            }
        }

        QSqlQuery pointUpdate(m_impl->database);
        pointUpdate.prepare(QStringLiteral("UPDATE rollback_points SET artifact_evicted = 1, evicted_utc = ? WHERE rollback_id = ?"));
        pointUpdate.addBindValue(dateTimeString(nowUtc));
        pointUpdate.addBindValue(candidate.id);
        if (!pointUpdate.exec())
        {
            exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
            result.errorMessage = queryError(pointUpdate);
            return result;
        }
        ++result.pointsEvicted;
        result.bytesEvicted += candidate.bytes;
        activeBytes -= candidate.bytes;
    }

    if (!exec(m_impl->database, QStringLiteral("COMMIT"), &error))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        result.errorMessage = error;
        return result;
    }
    result.success = true;
    return result;
}

PDFOperationResult PDFOperationHistoryStore::rollbackTo(const PDFRollbackRequest& request,
                                                        const PDFArtifactStore& artifacts,
                                                        const QString& destinationPath,
                                                        qint64* sequence)
{
    if (!isOpen() || destinationPath.isEmpty())
    {
        return PDFOperationResult(QStringLiteral("Operation history store or rollback destination is invalid."));
    }
    if (!request.approval.isValid() || request.approval.kind == PDFApprovalKind::None)
    {
        return PDFOperationResult(QStringLiteral("Rollback requires a valid approval record."));
    }

    PDFArtifactIdentity target;
    if (const PDFOperationResult resolveResult = resolveRollbackTarget(request, &target); !resolveResult)
    {
        return resolveResult;
    }
    if (!artifacts.verify(target))
    {
        return PDFOperationResult(QStringLiteral("Rollback artifact failed integrity verification; current document was not changed."));
    }

    PDFArtifactIdentity current;
    QSqlQuery currentQuery(m_impl->database);
    currentQuery.prepare(QStringLiteral("SELECT sha256, size_bytes, media_type, logical_name, storage_token FROM artifacts WHERE sha256 = ? AND artifact_evicted = 0"));
    currentQuery.addBindValue(request.currentArtifactSha256.toLower());
    if (!currentQuery.exec() || !currentQuery.next())
    {
        return PDFOperationResult(QStringLiteral("Current document artifact is not registered."));
    }
    current.sha256 = currentQuery.value(0).toString();
    current.size = currentQuery.value(1).toLongLong();
    current.mediaType = currentQuery.value(2).toString();
    current.logicalName = currentQuery.value(3).toString();
    current.storageToken = currentQuery.value(4).toString();

    PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("history.rollback");
    execution.operationVersion = 1;
    execution.input = current;
    execution.parameters = request.toJson();
    QUuid executionId;
    if (const PDFOperationResult beginResult = beginExecution(execution, &executionId); !beginResult)
    {
        return beginResult;
    }
    PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.status = PDFOperationHistoryStatus::Running;
    if (const PDFOperationResult eventResult = appendEvent(running); !eventResult)
    {
        return eventResult;
    }

    const PDFArtifactRestoreResult restoreResult = artifacts.restoreToFile(target, destinationPath);
    if (!restoreResult.success)
    {
        PDFOperationHistoryEvent failed;
        failed.executionId = executionId;
        failed.status = PDFOperationHistoryStatus::Failed;
        failed.resultSummary = QJsonObject{ { QStringLiteral("error"), restoreResult.errorMessage } };
        appendEvent(failed);
        return PDFOperationResult(restoreResult.errorMessage);
    }

    PDFOperationHistoryEvent complete;
    complete.executionId = executionId;
    complete.status = PDFOperationHistoryStatus::RolledBack;
    complete.output = target;
    complete.resultSummary = QJsonObject{
        { QStringLiteral("targetArtifactSha256"), target.sha256 },
        { QStringLiteral("reason"), request.reason }
    };
    complete.approval = request.approval;
    return appendEvent(complete, sequence);
}

PDFOperationResult PDFOperationHistoryStore::resolveRollbackTarget(const PDFRollbackRequest& request,
                                                                   PDFArtifactIdentity* targetArtifact) const
{
    if (!isOpen() || !targetArtifact)
        return PDFOperationResult(QStringLiteral("Operation history store or rollback target is invalid."));
    if (!isPDFSha256(request.targetArtifactSha256) || request.targetExecutionId.isNull())
        return PDFOperationResult(QStringLiteral("Rollback target identity is invalid."));
    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("SELECT a.sha256, a.size_bytes, a.media_type, a.logical_name, a.storage_token FROM history_events h JOIN artifacts a ON a.sha256 = h.output_sha256 JOIN rollback_points p ON p.audit_event_id = h.entry_id WHERE h.execution_id = ? AND h.status = 'accepted' AND h.output_sha256 = ? AND p.artifact_evicted = 0 ORDER BY h.sequence DESC LIMIT 1"));
    query.addBindValue(request.targetExecutionId.toString(QUuid::WithoutBraces));
    query.addBindValue(request.targetArtifactSha256.toLower());
    if (!query.exec() || !query.next())
        return PDFOperationResult(QStringLiteral("Rollback target is not an accepted immutable artifact."));
    targetArtifact->sha256 = query.value(0).toString();
    targetArtifact->size = query.value(1).toLongLong();
    targetArtifact->mediaType = query.value(2).toString();
    targetArtifact->logicalName = query.value(3).toString();
    targetArtifact->storageToken = query.value(4).toString();
    return targetArtifact->isValid() ? PDFOperationResult(true) : PDFOperationResult(QStringLiteral("Rollback target artifact metadata is invalid."));
}

}   // namespace pdf
