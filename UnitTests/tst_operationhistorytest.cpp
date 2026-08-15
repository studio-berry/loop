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

#include "pdfartifactstore.h"
#include "pdfoperationhistorystore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>

#include <atomic>
#include <memory>

class OperationHistoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void canonicalJsonIsStableAndRedacted();
    void artifactStoreStreamsAndDetectsTampering();
    void lifecycleApprovalAndRollbackResolution();
    void rollbackPointsRetentionAndAtomicity();
    void externalPayloadTamperingCompromisesChain();
    void provenanceKindsRoundTripAndMiddleDeletionCompromisesChain();
    void schemaVersionPersistsAcrossReopen();
    void schemaV2MigratesOnceAndPreservesChain();
    void concurrentIdenticalArtifactImportsSucceed();
    void runningFailureAppendsTerminalFailedEvent();
};

void OperationHistoryTest::canonicalJsonIsStableAndRedacted()
{
    const QJsonObject first{ { QStringLiteral("z"), 1 }, { QStringLiteral("a"), QJsonObject{ { QStringLiteral("token"), QStringLiteral("secret") } } } };
    const QJsonObject second{ { QStringLiteral("a"), QJsonObject{ { QStringLiteral("token"), QStringLiteral("secret") } } }, { QStringLiteral("z"), 1 } };
    QCOMPARE(pdf::canonicalJson(first), pdf::canonicalJson(second));
    const QJsonObject redacted = pdf::redactSensitiveJson(first).toObject();
    QCOMPARE(redacted.value(QStringLiteral("a")).toObject().value(QStringLiteral("token")).toString(), QStringLiteral("[REDACTED]"));
}

void OperationHistoryTest::artifactStoreStreamsAndDetectsTampering()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore store(temporary.path());
    const QByteArray payload("immutable artifact payload");
    const pdf::PDFArtifactStoreResult first = store.importBytes(payload, { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    const QByteArray firstError = first.errorMessage.toUtf8();
    QVERIFY2(first.success, firstError.constData());
    QCOMPARE(first.artifact.sha256, QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));
    QVERIFY(first.artifact.isValid());
    QVERIFY(store.verify(first.artifact));
    {
        QFile published(store.pathFor(first.artifact));
        QVERIFY2(!published.open(QIODevice::WriteOnly | QIODevice::Append),
                 "Published artifact must not be writable");
    }

    const pdf::PDFArtifactStoreResult second = store.importBytes(payload, { QStringLiteral("application/pdf"), QStringLiteral("copy.pdf") });
    const QByteArray secondError = second.errorMessage.toUtf8();
    QVERIFY2(second.success, secondError.constData());
    QVERIFY(second.reused);
    QVERIFY(store.verify(second.artifact));

    QFile file(store.pathFor(first.artifact));
    QVERIFY(QFile::setPermissions(file.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(file.open(QIODevice::Append));
    QVERIFY(file.write("tamper") > 0);
    file.close();
    QVERIFY(QFile::setPermissions(file.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(!store.verify(first.artifact));
}

void OperationHistoryTest::lifecycleApprovalAndRollbackResolution()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const auto output = artifacts.importBytes("output", { QStringLiteral("application/pdf"), QStringLiteral("output.pdf") });
    const QByteArray inputError = input.errorMessage.toUtf8();
    const QByteArray outputError = output.errorMessage.toUtf8();
    QVERIFY2(input.success, inputError.constData());
    QVERIFY2(output.success, outputError.constData());

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QString openError;
    const pdf::PDFOperationResult opened = history.open(&openError);
    const QByteArray openErrorUtf8 = openError.toUtf8();
    QVERIFY2(opened, openErrorUtf8.constData());
    QVERIFY(history.registerArtifact(input.artifact));
    QVERIFY(history.registerArtifact(output.artifact));

    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("test.operation");
    execution.operationVersion = 2;
    execution.input = input.artifact;
    execution.sourceDocumentRevision = 7;
    execution.parameters = QJsonObject{ { QStringLiteral("password"), QStringLiteral("do-not-store") }, { QStringLiteral("mode"), QStringLiteral("safe") } };
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));

    pdf::PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.status = pdf::PDFOperationHistoryStatus::Running;
    QVERIFY(history.appendEvent(running));

    pdf::PDFOperationHistoryEvent accepted;
    accepted.executionId = executionId;
    accepted.status = pdf::PDFOperationHistoryStatus::Accepted;
    accepted.output = output.artifact;
    accepted.resultSummary = QJsonObject{ { QStringLiteral("password"), QStringLiteral("do-not-store") } };
    accepted.approval.kind = pdf::PDFApprovalKind::Human;
    accepted.approval.actorId = QStringLiteral("local-user:test");
    accepted.approval.decision = QStringLiteral("approve");
    accepted.approval.rationale = QStringLiteral("postflight passed");
    accepted.approval.evidenceSha256 = output.artifact.sha256;
    accepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    QVERIFY(history.appendEvent(accepted));

    const auto verification = history.verify();
    QVERIFY(verification.verified);
    QCOMPARE(verification.eventsChecked, qint64(2));
    const auto rows = history.events();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(1).resultSummary.value(QStringLiteral("password")).toString(), QStringLiteral("[REDACTED]"));
    QVERIFY(rows.at(1).approval.isValid());

    pdf::PDFRollbackRequest rollback;
    rollback.currentArtifactSha256 = input.artifact.sha256;
    rollback.targetArtifactSha256 = output.artifact.sha256;
    rollback.targetExecutionId = executionId;
    pdf::PDFArtifactIdentity resolved;
    QVERIFY(history.resolveRollbackTarget(rollback, &resolved));
    QCOMPARE(resolved.sha256, output.artifact.sha256);
    const QString rollbackPath = QDir(temporary.path()).filePath(QStringLiteral("rollback.pdf"));
    QVERIFY(artifacts.restoreToFile(resolved, rollbackPath).success);
    QFile rollbackFile(rollbackPath);
    QVERIFY(rollbackFile.open(QIODevice::ReadOnly));
    QCOMPARE(rollbackFile.readAll(), QByteArray("output"));
}

void OperationHistoryTest::rollbackPointsRetentionAndAtomicity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("input", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const auto middle = artifacts.importBytes("middle", { QStringLiteral("application/pdf"), QStringLiteral("middle.pdf") });
    const auto final = artifacts.importBytes("final", { QStringLiteral("application/pdf"), QStringLiteral("final.pdf") });
    const QByteArray inputError = input.errorMessage.toUtf8();
    const QByteArray middleError = middle.errorMessage.toUtf8();
    const QByteArray finalError = final.errorMessage.toUtf8();
    QVERIFY2(input.success, inputError.constData());
    QVERIFY2(middle.success, middleError.constData());
    QVERIFY2(final.success, finalError.constData());

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QString openError;
    const pdf::PDFOperationResult opened = history.open(&openError);
    const QByteArray openErrorUtf8 = openError.toUtf8();
    QVERIFY2(opened, openErrorUtf8.constData());
    QVERIFY(history.registerOriginalInput(input.artifact));
    QVERIFY(history.registerArtifact(middle.artifact));
    QVERIFY(history.registerArtifact(final.artifact));

    auto appendAccepted = [&](const pdf::PDFArtifactIdentity& source,
                              const pdf::PDFArtifactIdentity& output,
                              const QString& operation,
                              bool approved,
                              QUuid* executionId)
    {
        pdf::PDFOperationHistoryExecution execution;
        execution.operationId = operation;
        execution.input = source;
        if (!history.beginExecution(execution, executionId))
            return false;
        pdf::PDFOperationHistoryEvent event;
        event.executionId = *executionId;
        event.status = pdf::PDFOperationHistoryStatus::Accepted;
        event.output = output;
        if (approved)
        {
            event.approval.kind = pdf::PDFApprovalKind::Human;
            event.approval.actorId = QStringLiteral("test-user");
            event.approval.decision = QStringLiteral("approve");
            event.approval.decidedUtc = QDateTime::currentDateTimeUtc();
        }
        return static_cast<bool>(history.appendEvent(event));
    };

    QUuid middleExecution;
    QUuid finalExecution;
    QVERIFY(appendAccepted(input.artifact, middle.artifact, QStringLiteral("test.middle"), false, &middleExecution));
    QVERIFY(appendAccepted(middle.artifact, final.artifact, QStringLiteral("test.final"), true, &finalExecution));
    QCOMPARE(history.rollbackPoints().size(), 3);

    pdf::PDFHistoryRetentionPolicy policy;
    policy.maxPointsPerJob = 2;
    policy.maxBytesPerJob = 1024 * 1024;
    policy.maxAgeDays = 365;
    const auto retention = history.enforceRetention(policy, artifacts);
    QVERIFY(retention.success);
    QCOMPARE(retention.pointsEvicted, 1);
    QVERIFY(!artifacts.verify(middle.artifact));
    QVERIFY(artifacts.verify(input.artifact));
    QVERIFY(artifacts.verify(final.artifact));

    QFile current(QDir(temporary.path()).filePath(QStringLiteral("current.pdf")));
    QVERIFY(current.open(QIODevice::WriteOnly));
    QVERIFY(current.write("input") == 5);
    current.close();

    pdf::PDFRollbackRequest rollback;
    rollback.currentArtifactSha256 = input.artifact.sha256;
    rollback.targetArtifactSha256 = final.artifact.sha256;
    rollback.targetExecutionId = finalExecution;
    rollback.reason = QStringLiteral("test rollback");
    rollback.approval.kind = pdf::PDFApprovalKind::System;
    rollback.approval.actorId = QStringLiteral("test-system");
    rollback.approval.decision = QStringLiteral("approve");
    rollback.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    const auto eventsBeforeRollback = history.events();
    QCOMPARE(eventsBeforeRollback.size(), 2);
    QVERIFY(history.rollbackTo(rollback, artifacts, current.fileName()));
    const auto eventsAfterRollback = history.events();
    QCOMPARE(eventsAfterRollback.size(), 4);
    QCOMPARE(eventsAfterRollback.at(0).entryId, eventsBeforeRollback.at(0).entryId);
    QCOMPARE(eventsAfterRollback.at(1).entryId, eventsBeforeRollback.at(1).entryId);
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), QByteArray("final"));
    current.close();
    QVERIFY(history.verify().verified);

    QFile corrupt(artifacts.pathFor(final.artifact));
    QVERIFY(QFile::setPermissions(corrupt.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(corrupt.open(QIODevice::Append));
    QVERIFY(corrupt.write("corrupt") > 0);
    corrupt.close();
    QVERIFY(QFile::setPermissions(corrupt.fileName(),
                                  QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QVERIFY(!history.rollbackTo(rollback, artifacts, current.fileName()));
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), QByteArray("final"));
}

void OperationHistoryTest::externalPayloadTamperingCompromisesChain()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const QByteArray inputError = input.errorMessage.toUtf8();
    QVERIFY2(input.success, inputError.constData());
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QString openError;
    const pdf::PDFOperationResult opened = history.open(&openError);
    const QByteArray openErrorUtf8 = openError.toUtf8();
    QVERIFY2(opened, openErrorUtf8.constData());
    QVERIFY(history.registerArtifact(input.artifact));
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("tamper.test");
    execution.input = input.artifact;
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));
    pdf::PDFOperationHistoryEvent event;
    event.executionId = executionId;
    event.status = pdf::PDFOperationHistoryStatus::Rejected;
    QVERIFY(history.appendEvent(event));

    const QString connectionName = QStringLiteral("history-tamper-test");
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("UPDATE history_events SET result_json = '{\"changed\":true}' WHERE sequence = 1")));
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);

    const auto verification = history.verify();
    QVERIFY(!verification.verified);
    QCOMPARE(verification.integrity, QStringLiteral("compromised"));
}

void OperationHistoryTest::provenanceKindsRoundTripAndMiddleDeletionCompromisesChain()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const QByteArray inputError = input.errorMessage.toUtf8();
    QVERIFY2(input.success, inputError.constData());

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QString openError;
    const pdf::PDFOperationResult opened = history.open(&openError);
    const QByteArray openErrorUtf8 = openError.toUtf8();
    QVERIFY2(opened, openErrorUtf8.constData());
    QVERIFY(history.registerArtifact(input.artifact));

    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("provenance.test");
    execution.input = input.artifact;
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));

    const QString digest(64, QLatin1Char('a'));
    const QList<pdf::PDFOperationHistoryEventKind> kinds{
        pdf::PDFOperationHistoryEventKind::DocumentOpened,
        pdf::PDFOperationHistoryEventKind::PreflightRun,
        pdf::PDFOperationHistoryEventKind::FixApplied,
        pdf::PDFOperationHistoryEventKind::DecisionRecorded,
        pdf::PDFOperationHistoryEventKind::DecisionInvalidated,
        pdf::PDFOperationHistoryEventKind::CertificateIssued,
        pdf::PDFOperationHistoryEventKind::CertificateInvalidated
    };

    for (const auto kind : kinds)
    {
        pdf::PDFOperationHistoryEvent event;
        event.executionId = executionId;
        event.kind = kind;
        event.status = pdf::PDFOperationHistoryStatus::Rejected;
        event.operatorIdentity = QStringLiteral("local-user:test");
        event.documentRevisionDigest = digest;
        event.effectiveProfileDigest = digest;
        event.approval.decisionReference = QStringLiteral("decision-test");
        QVERIFY(history.appendEvent(event));
    }

    const auto rows = history.events();
    QCOMPARE(rows.size(), kinds.size());
    for (int index = 0; index < kinds.size(); ++index)
    {
        QCOMPARE(static_cast<int>(rows.at(index).kind), static_cast<int>(kinds.at(index)));
        QCOMPARE(rows.at(index).operatorIdentity, QStringLiteral("local-user:test"));
        QCOMPARE(rows.at(index).documentRevisionDigest, digest);
        QCOMPARE(rows.at(index).effectiveProfileDigest, digest);
        QCOMPARE(rows.at(index).approval.decisionReference, QStringLiteral("decision-test"));
    }
    QVERIFY(history.verify().verified);

    const QString connectionName = QStringLiteral("provenance-middle-delete-test");
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(history.databasePath());
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("DELETE FROM history_events WHERE sequence = 4")));
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);

    const auto verification = history.verify();
    QVERIFY(!verification.verified);
    QCOMPARE(verification.integrity, QStringLiteral("compromised"));
}

QString historySchemaVersion(const QString& databasePath)
{
    const QString connectionName = QStringLiteral("schema-version-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open())
    {
        return {};
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT value FROM schema_meta WHERE key = 'schema_version'")) || !query.next())
    {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return {};
    }
    const QString version = query.value(0).toString();
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return version;
}

bool downgradeHistoryDatabaseToV2(const QString& databasePath, QString* error)
{
    const QString connectionName = QStringLiteral("schema-downgrade-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open())
    {
        if (error)
            *error = QStringLiteral("Could not open history database for v2 downgrade.");
        return false;
    }

    auto fail = [&](const QString& message)
    {
        if (error)
            *error = message;
        QSqlQuery(database).exec(QStringLiteral("ROLLBACK"));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return false;
    };

    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA foreign_keys = OFF")) ||
        !query.exec(QStringLiteral("BEGIN")) ||
        !query.exec(QStringLiteral(
            "CREATE TABLE history_events_v2 ("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT, "
            "entry_id TEXT NOT NULL UNIQUE, "
            "execution_id TEXT NOT NULL, "
            "status TEXT NOT NULL, "
            "result_json TEXT NOT NULL, "
            "output_sha256 TEXT, "
            "finding_ids_json TEXT NOT NULL, "
            "report_sha256 TEXT, "
            "diff_sha256 TEXT, "
            "approval_json TEXT NOT NULL, "
            "previous_event_hash TEXT NOT NULL, "
            "event_hash TEXT NOT NULL, "
            "created_utc TEXT NOT NULL, "
            "FOREIGN KEY(execution_id) REFERENCES executions(execution_id), "
            "FOREIGN KEY(output_sha256) REFERENCES artifacts(sha256))")) ||
        !query.exec(QStringLiteral(
            "INSERT INTO history_events_v2("
            "sequence, entry_id, execution_id, status, result_json, output_sha256, "
            "finding_ids_json, report_sha256, diff_sha256, approval_json, "
            "previous_event_hash, event_hash, created_utc) "
            "SELECT sequence, entry_id, execution_id, status, result_json, output_sha256, "
            "finding_ids_json, report_sha256, diff_sha256, approval_json, "
            "previous_event_hash, event_hash, created_utc FROM history_events")) ||
        !query.exec(QStringLiteral("DROP TABLE history_events")) ||
        !query.exec(QStringLiteral("ALTER TABLE history_events_v2 RENAME TO history_events")) ||
        !query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_history_execution ON history_events(execution_id, sequence)")) ||
        !query.exec(QStringLiteral("UPDATE schema_meta SET value = '2' WHERE key = 'schema_version'")) ||
        !query.exec(QStringLiteral("COMMIT")))
    {
        return fail(query.lastError().text().isEmpty()
                        ? QStringLiteral("Could not downgrade history database to schema v2.")
                        : query.lastError().text());
    }

    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return true;
}

bool historyEventsTableHasColumn(const QString& databasePath, const QString& column)
{
    const QString connectionName = QStringLiteral("schema-columns-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open())
    {
        return false;
    }
    QSqlQuery query(database);
    const bool ok = query.exec(QStringLiteral("PRAGMA table_info(history_events)"));
    bool found = false;
    while (ok && query.next())
    {
        if (query.value(1).toString().compare(column, Qt::CaseInsensitive) == 0)
        {
            found = true;
            break;
        }
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return found;
}

void OperationHistoryTest::schemaVersionPersistsAcrossReopen()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const auto output = artifacts.importBytes("output", { QStringLiteral("application/pdf"), QStringLiteral("output.pdf") });
    QVERIFY(input.success);
    QVERIFY(output.success);

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    {
        pdf::PDFOperationHistoryStore history(databasePath);
        QString openError;
        QVERIFY2(history.open(&openError), qPrintable(openError));
        QVERIFY(history.registerArtifact(input.artifact));
        QVERIFY(history.registerArtifact(output.artifact));
        pdf::PDFOperationHistoryExecution execution;
        execution.operationId = QStringLiteral("schema.reopen");
        execution.input = input.artifact;
        QUuid executionId;
        QVERIFY(history.beginExecution(execution, &executionId));
        pdf::PDFOperationHistoryEvent accepted;
        accepted.executionId = executionId;
        accepted.status = pdf::PDFOperationHistoryStatus::Accepted;
        accepted.output = output.artifact;
        accepted.approval.kind = pdf::PDFApprovalKind::Human;
        accepted.approval.actorId = QStringLiteral("local-user:test");
        accepted.approval.decision = QStringLiteral("approve");
        accepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();
        QVERIFY(history.appendEvent(accepted));
        history.close();
    }

    QCOMPARE(historySchemaVersion(databasePath), QStringLiteral("3"));

    pdf::PDFOperationHistoryStore reopened(databasePath);
    QString reopenError;
    QVERIFY2(reopened.open(&reopenError), qPrintable(reopenError));
    QCOMPARE(historySchemaVersion(databasePath), QStringLiteral("3"));
    QVERIFY(reopened.verify().verified);
    QCOMPARE(reopened.events().size(), 1);
    QVERIFY(reopened.events().front().approval.isValid());
    QCOMPARE(reopened.rollbackPoints().size(), 1);
}

void OperationHistoryTest::schemaV2MigratesOnceAndPreservesChain()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const auto output = artifacts.importBytes("output", { QStringLiteral("application/pdf"), QStringLiteral("output.pdf") });
    QVERIFY(input.success);
    QVERIFY(output.success);

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    QUuid executionId;
    QUuid entryId;
    QByteArray eventHash;
    {
        pdf::PDFOperationHistoryStore history(databasePath);
        QString openError;
        QVERIFY2(history.open(&openError), qPrintable(openError));
        QVERIFY(history.registerOriginalInput(input.artifact));
        QVERIFY(history.registerArtifact(output.artifact));
        pdf::PDFOperationHistoryExecution execution;
        execution.operationId = QStringLiteral("schema.migrate");
        execution.input = input.artifact;
        QVERIFY(history.beginExecution(execution, &executionId));
        pdf::PDFOperationHistoryEvent accepted;
        accepted.executionId = executionId;
        accepted.status = pdf::PDFOperationHistoryStatus::Accepted;
        accepted.output = output.artifact;
        accepted.approval.kind = pdf::PDFApprovalKind::Human;
        accepted.approval.actorId = QStringLiteral("local-user:test");
        accepted.approval.decision = QStringLiteral("approve");
        accepted.approval.rationale = QStringLiteral("keep after migrate");
        accepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();
        QVERIFY(history.appendEvent(accepted));
        const auto rows = history.events();
        QCOMPARE(rows.size(), 1);
        entryId = rows.front().entryId;
        eventHash = rows.front().eventHash;
        QVERIFY(history.verify().verified);
        history.close();
    }

    QString downgradeError;
    QVERIFY2(downgradeHistoryDatabaseToV2(databasePath, &downgradeError), qPrintable(downgradeError));
    QCOMPARE(historySchemaVersion(databasePath), QStringLiteral("2"));
    QVERIFY(!historyEventsTableHasColumn(databasePath, QStringLiteral("event_kind")));
    QVERIFY(!historyEventsTableHasColumn(databasePath, QStringLiteral("operator_identity")));
    QVERIFY(!historyEventsTableHasColumn(databasePath, QStringLiteral("document_revision_digest")));
    QVERIFY(!historyEventsTableHasColumn(databasePath, QStringLiteral("effective_profile_digest")));

    {
        pdf::PDFOperationHistoryStore migrated(databasePath);
        QString migrateError;
        QVERIFY2(migrated.open(&migrateError), qPrintable(migrateError));
        QCOMPARE(historySchemaVersion(databasePath), QStringLiteral("3"));
        QVERIFY(historyEventsTableHasColumn(databasePath, QStringLiteral("event_kind")));
        QVERIFY(migrated.verify().verified);
        const auto rows = migrated.events();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.front().entryId, entryId);
        QCOMPARE(rows.front().eventHash, eventHash);
        QCOMPARE(rows.front().approval.rationale, QStringLiteral("keep after migrate"));
        QCOMPARE(migrated.rollbackPoints().size(), 2);
        migrated.close();
    }

    QCOMPARE(historySchemaVersion(databasePath), QStringLiteral("3"));
    pdf::PDFOperationHistoryStore secondOpen(databasePath);
    QString secondError;
    QVERIFY2(secondOpen.open(&secondError), qPrintable(secondError));
    QCOMPARE(historySchemaVersion(databasePath), QStringLiteral("3"));
    QVERIFY(secondOpen.verify().verified);
    QCOMPARE(secondOpen.events().front().eventHash, eventHash);
}

void OperationHistoryTest::concurrentIdenticalArtifactImportsSucceed()
{
    const QByteArray payload(256 * 1024, char('A'));
    const QByteArray expectedSha = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();

    for (int iteration = 0; iteration < 24; ++iteration)
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        pdf::PDFArtifactStore store(temporary.path());
        std::atomic_int ready{ 0 };
        std::atomic_bool go{ false };
        pdf::PDFArtifactStoreResult results[2];

        auto worker = [&](int index)
        {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire))
            {
                QThread::yieldCurrentThread();
            }
            results[index] = store.importBytes(payload, { QStringLiteral("application/octet-stream"),
                                                          QStringLiteral("worker-%1.bin").arg(index) });
        };

        std::unique_ptr<QThread> first(QThread::create(worker, 0));
        std::unique_ptr<QThread> second(QThread::create(worker, 1));
        first->start();
        second->start();
        while (ready.load(std::memory_order_acquire) < 2)
        {
            QThread::yieldCurrentThread();
        }
        go.store(true, std::memory_order_release);
        QVERIFY(first->wait(30000));
        QVERIFY(second->wait(30000));

        QVERIFY2(results[0].success, qPrintable(results[0].errorMessage));
        QVERIFY2(results[1].success, qPrintable(results[1].errorMessage));
        QCOMPARE(results[0].artifact.sha256, QString::fromLatin1(expectedSha));
        QCOMPARE(results[1].artifact.sha256, results[0].artifact.sha256);
        QCOMPARE(results[0].artifact.size, qint64(payload.size()));
        QCOMPARE(results[1].artifact.size, results[0].artifact.size);
        QVERIFY(store.verify(results[0].artifact));
        QVERIFY(store.verify(results[1].artifact));
        QCOMPARE(store.pathFor(results[0].artifact), store.pathFor(results[1].artifact));
    }
}

void OperationHistoryTest::runningFailureAppendsTerminalFailedEvent()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    QVERIFY2(input.success, qPrintable(input.errorMessage));

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QString openError;
    QVERIFY(history.open(&openError));
    QVERIFY(history.registerArtifact(input.artifact));

    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("repair");
    execution.operationVersion = 1;
    execution.input = input.artifact;
    QUuid executionId;
    QVERIFY(history.beginExecution(execution, &executionId));

    pdf::PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.status = pdf::PDFOperationHistoryStatus::Running;
    QVERIFY(history.appendEvent(running));

    pdf::PDFOperationHistoryEvent failed;
    failed.executionId = executionId;
    failed.status = pdf::PDFOperationHistoryStatus::Failed;
    failed.resultSummary = QJsonObject{
        { QStringLiteral("error_code"), QStringLiteral("repair.output-mismatch") },
        { QStringLiteral("error"), QStringLiteral("mismatch") }
    };
    QVERIFY(history.appendEvent(failed));

    const QList<pdf::PDFOperationHistoryEvent> events = history.events();
    QCOMPARE(events.size(), 2);
    QCOMPARE(events.last().status, pdf::PDFOperationHistoryStatus::Failed);
}

QTEST_MAIN(OperationHistoryTest)
#include "tst_operationhistorytest.moc"
