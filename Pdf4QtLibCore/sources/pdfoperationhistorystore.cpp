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

constexpr int CurrentSchemaVersion = 1;

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

} // namespace

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
        if (errorMessage) *errorMessage = error;
        return PDFOperationResult(error);
    }

    if (m_databasePath != QStringLiteral(":memory:"))
    {
        const QFileInfo info(m_databasePath);
        if (!QDir().mkpath(info.absolutePath()))
        {
            const QString error = QStringLiteral("Could not create the operation history database directory.");
            if (errorMessage) *errorMessage = error;
            return PDFOperationResult(error);
        }
    }

    m_impl->connection = connectionName();
    m_impl->database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_impl->connection);
    m_impl->database.setDatabaseName(m_databasePath);
    if (!m_impl->database.open())
    {
        const QString error = databaseError(m_impl->database);
        if (errorMessage) *errorMessage = error;
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
        if (errorMessage) *errorMessage = error;
        close();
        return PDFOperationResult(error);
    }

    QSqlQuery versionQuery(m_impl->database);
    if (!versionQuery.exec(QStringLiteral("SELECT value FROM schema_meta WHERE key = 'schema_version'")))
    {
        error = queryError(versionQuery);
    }
    else if (versionQuery.next())
    {
        bool ok = false;
        const int version = versionQuery.value(0).toInt(&ok);
        if (!ok || version > CurrentSchemaVersion)
        {
            error = QStringLiteral("Operation history database schema is newer than this application.");
        }
    }
    if (!error.isEmpty())
    {
        if (errorMessage) *errorMessage = error;
        close();
        return PDFOperationResult(error);
    }

    if (!exec(m_impl->database, QStringLiteral("BEGIN IMMEDIATE"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS artifacts (sha256 TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL, media_type TEXT NOT NULL, logical_name TEXT, storage_token TEXT, created_utc TEXT NOT NULL)"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS executions (execution_id TEXT PRIMARY KEY, parent_execution_id TEXT, operation_id TEXT NOT NULL, operation_version INTEGER NOT NULL, source_sha256 TEXT NOT NULL, source_revision INTEGER NOT NULL, parameters_json TEXT NOT NULL, started_utc TEXT NOT NULL, FOREIGN KEY(source_sha256) REFERENCES artifacts(sha256), FOREIGN KEY(parent_execution_id) REFERENCES executions(execution_id))"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE TABLE IF NOT EXISTS history_events (sequence INTEGER PRIMARY KEY AUTOINCREMENT, entry_id TEXT NOT NULL UNIQUE, execution_id TEXT NOT NULL, status TEXT NOT NULL, result_json TEXT NOT NULL, output_sha256 TEXT, finding_ids_json TEXT NOT NULL, report_sha256 TEXT, diff_sha256 TEXT, approval_json TEXT NOT NULL, previous_event_hash TEXT NOT NULL, event_hash TEXT NOT NULL, created_utc TEXT NOT NULL, FOREIGN KEY(execution_id) REFERENCES executions(execution_id), FOREIGN KEY(output_sha256) REFERENCES artifacts(sha256))"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_history_execution ON history_events(execution_id, sequence)"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_execution_source ON executions(source_sha256, source_revision)"), &error) ||
        !exec(m_impl->database, QStringLiteral("CREATE INDEX IF NOT EXISTS idx_execution_operation ON executions(operation_id, started_utc)"), &error) ||
        !exec(m_impl->database, QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) VALUES('schema_version', '1')"), &error) ||
        !exec(m_impl->database, QStringLiteral("COMMIT"), &error))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        if (errorMessage) *errorMessage = error;
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

PDFOperationResult PDFOperationHistoryStore::registerArtifact(const PDFArtifactIdentity& artifact)
{
    if (!isOpen()) return PDFOperationResult(QStringLiteral("Operation history store is not open."));
    if (!artifact.isValid()) return PDFOperationResult(QStringLiteral("Artifact identity is invalid."));
    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO artifacts(sha256, size_bytes, media_type, logical_name, storage_token, created_utc) VALUES(?, ?, ?, ?, ?, ?)"));
    query.addBindValue(artifact.sha256.toLower());
    query.addBindValue(artifact.size);
    query.addBindValue(artifact.mediaType);
    query.addBindValue(sanitizeArtifactLogicalName(artifact.logicalName));
    query.addBindValue(artifact.storageToken);
    query.addBindValue(dateTimeString(QDateTime::currentDateTimeUtc()));
    if (!query.exec()) return PDFOperationResult(queryError(query));

    QSqlQuery check(m_impl->database);
    check.prepare(QStringLiteral("SELECT size_bytes, media_type FROM artifacts WHERE sha256 = ?"));
    check.addBindValue(artifact.sha256.toLower());
    if (!check.exec() || !check.next()) return PDFOperationResult(queryError(check));
    if (check.value(0).toLongLong() != artifact.size || check.value(1).toString() != artifact.mediaType)
    {
        return PDFOperationResult(QStringLiteral("Artifact digest is already registered with different immutable metadata."));
    }
    return true;
}

PDFOperationResult PDFOperationHistoryStore::beginExecution(PDFOperationHistoryExecution execution,
                                                             QUuid* executionId)
{
    if (!isOpen()) return PDFOperationResult(QStringLiteral("Operation history store is not open."));
    if (execution.executionId.isNull()) execution.executionId = QUuid::createUuid();
    if (execution.operationId.trimmed().isEmpty() || execution.operationVersion <= 0 || !execution.input.isValid())
    {
        return PDFOperationResult(QStringLiteral("Execution identity or source artifact is invalid."));
    }
    if (!execution.startedUtc.isValid()) execution.startedUtc = QDateTime::currentDateTimeUtc();

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
    if (!query.exec()) return PDFOperationResult(queryError(query));
    if (executionId) *executionId = execution.executionId;
    return true;
}

PDFOperationResult PDFOperationHistoryStore::appendEvent(PDFOperationHistoryEvent event,
                                                         qint64* sequence)
{
    if (!isOpen()) return PDFOperationResult(QStringLiteral("Operation history store is not open."));
    if (event.entryId.isNull()) event.entryId = QUuid::createUuid();
    if (event.executionId.isNull() || !event.approval.isValid()) return PDFOperationResult(QStringLiteral("History event identity or approval is invalid."));
    if (!event.createdUtc.isValid()) event.createdUtc = QDateTime::currentDateTimeUtc();
    if ((event.status == PDFOperationHistoryStatus::Accepted || event.status == PDFOperationHistoryStatus::RolledBack) && !event.output)
    {
        return PDFOperationResult(QStringLiteral("Accepted history events require a durable output artifact."));
    }
    if (event.output && !event.output->isValid()) return PDFOperationResult(QStringLiteral("History output artifact identity is invalid."));
    if ((!event.reportArtifactSha256.isEmpty() && !isPDFSha256(event.reportArtifactSha256)) ||
        (!event.diffArtifactSha256.isEmpty() && !isPDFSha256(event.diffArtifactSha256)))
    {
        return PDFOperationResult(QStringLiteral("History evidence digest is invalid."));
    }

    QString error;
    if (!exec(m_impl->database, QStringLiteral("BEGIN IMMEDIATE"), &error)) return PDFOperationResult(error);
    QSqlQuery previousQuery(m_impl->database);
    if (!previousQuery.exec(QStringLiteral("SELECT event_hash FROM history_events ORDER BY sequence DESC LIMIT 1")))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        return PDFOperationResult(queryError(previousQuery));
    }
    const QByteArray previousHash = previousQuery.next() ? decodeHash(previousQuery.value(0).toString()) : QByteArray();
    event.previousEventHash = previousHash;
    event.eventHash = computeOperationHistoryEventHash(event, previousHash);

    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("INSERT INTO history_events(entry_id, execution_id, status, result_json, output_sha256, finding_ids_json, report_sha256, diff_sha256, approval_json, previous_event_hash, event_hash, created_utc) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event.entryId.toString(QUuid::WithoutBraces));
    query.addBindValue(event.executionId.toString(QUuid::WithoutBraces));
    query.addBindValue(pdfOperationHistoryStatusToString(event.status));
    query.addBindValue(QString::fromUtf8(canonicalJson(redactSensitiveJson(event.resultSummary))));
    query.addBindValue(event.output ? event.output->sha256.toLower() : QVariant());
    query.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(event.findingIds)).toJson(QJsonDocument::Compact)));
    query.addBindValue(event.reportArtifactSha256.toLower());
    query.addBindValue(event.diffArtifactSha256.toLower());
    query.addBindValue(QString::fromUtf8(canonicalJson(event.approval.toJson())));
    query.addBindValue(QString::fromLatin1(previousHash.toHex()));
    query.addBindValue(QString::fromLatin1(event.eventHash.toHex()));
    query.addBindValue(dateTimeString(event.createdUtc));
    if (!query.exec())
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        return PDFOperationResult(queryError(query));
    }
    if (!exec(m_impl->database, QStringLiteral("COMMIT"), &error))
    {
        exec(m_impl->database, QStringLiteral("ROLLBACK"), nullptr);
        return PDFOperationResult(error);
    }
    event.sequence = query.lastInsertId().toLongLong();
    if (sequence) *sequence = event.sequence;
    return true;
}

QList<PDFOperationHistoryEvent> PDFOperationHistoryStore::events(QString* errorMessage) const
{
    QList<PDFOperationHistoryEvent> result;
    if (!isOpen())
    {
        if (errorMessage) *errorMessage = QStringLiteral("Operation history store is not open.");
        return result;
    }
    QSqlQuery query(m_impl->database);
    if (!query.exec(QStringLiteral("SELECT h.sequence, h.entry_id, h.execution_id, h.status, h.result_json, h.output_sha256, h.finding_ids_json, h.report_sha256, h.diff_sha256, h.approval_json, h.previous_event_hash, h.event_hash, h.created_utc, a.size_bytes, a.media_type, a.logical_name, a.storage_token FROM history_events h LEFT JOIN artifacts a ON a.sha256 = h.output_sha256 ORDER BY h.sequence")))
    {
        if (errorMessage) *errorMessage = queryError(query);
        return result;
    }
    while (query.next())
    {
        PDFOperationHistoryEvent event;
        event.sequence = query.value(0).toLongLong();
        event.entryId = QUuid(query.value(1).toString());
        event.executionId = QUuid(query.value(2).toString());
        event.status = pdfOperationHistoryStatusFromString(query.value(3).toString());
        event.resultSummary = parseObject(query.value(4).toString());
        const QString outputSha = query.value(5).toString();
        if (!outputSha.isEmpty() && !query.value(13).isNull())
        {
            PDFArtifactIdentity artifact;
            artifact.sha256 = outputSha;
            artifact.size = query.value(13).toLongLong();
            artifact.mediaType = query.value(14).toString();
            artifact.logicalName = query.value(15).toString();
            artifact.storageToken = query.value(16).toString();
            event.output = artifact;
        }
        const QJsonDocument findings = QJsonDocument::fromJson(query.value(6).toString().toUtf8());
        for (const QJsonValue& finding : findings.array()) event.findingIds.append(finding.toString());
        event.reportArtifactSha256 = query.value(7).toString();
        event.diffArtifactSha256 = query.value(8).toString();
        event.approval = PDFApprovalRecord::fromJson(parseObject(query.value(9).toString()));
        event.previousEventHash = decodeHash(query.value(10).toString());
        event.eventHash = decodeHash(query.value(11).toString());
        event.createdUtc = dateTimeFromString(query.value(12).toString());
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

PDFOperationResult PDFOperationHistoryStore::resolveRollbackTarget(const PDFRollbackRequest& request,
                                                                    PDFArtifactIdentity* targetArtifact) const
{
    if (!isOpen() || !targetArtifact) return PDFOperationResult(QStringLiteral("Operation history store or rollback target is invalid."));
    if (!isPDFSha256(request.targetArtifactSha256) || request.targetExecutionId.isNull()) return PDFOperationResult(QStringLiteral("Rollback target identity is invalid."));
    QSqlQuery query(m_impl->database);
    query.prepare(QStringLiteral("SELECT a.sha256, a.size_bytes, a.media_type, a.logical_name, a.storage_token FROM history_events h JOIN artifacts a ON a.sha256 = h.output_sha256 WHERE h.execution_id = ? AND h.status = 'accepted' AND h.output_sha256 = ? ORDER BY h.sequence DESC LIMIT 1"));
    query.addBindValue(request.targetExecutionId.toString(QUuid::WithoutBraces));
    query.addBindValue(request.targetArtifactSha256.toLower());
    if (!query.exec() || !query.next()) return PDFOperationResult(QStringLiteral("Rollback target is not an accepted immutable artifact."));
    targetArtifact->sha256 = query.value(0).toString();
    targetArtifact->size = query.value(1).toLongLong();
    targetArtifact->mediaType = query.value(2).toString();
    targetArtifact->logicalName = query.value(3).toString();
    targetArtifact->storageToken = query.value(4).toString();
    return targetArtifact->isValid() ? PDFOperationResult(true) : PDFOperationResult(QStringLiteral("Rollback target artifact metadata is invalid."));
}

} // namespace pdf
