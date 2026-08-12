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
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

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
};

void OperationHistoryTest::canonicalJsonIsStableAndRedacted()
{
    const QJsonObject first{{ QStringLiteral("z"), 1 }, { QStringLiteral("a"), QJsonObject{{ QStringLiteral("token"), QStringLiteral("secret") }} }};
    const QJsonObject second{{ QStringLiteral("a"), QJsonObject{{ QStringLiteral("token"), QStringLiteral("secret") }} }, { QStringLiteral("z"), 1 }};
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
    QVERIFY2(first.success, qPrintable(first.errorMessage));
    QCOMPARE(first.artifact.sha256, QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));
    QVERIFY(first.artifact.isValid());
    QVERIFY(store.verify(first.artifact));
    QVERIFY(!(QFileInfo(store.pathFor(first.artifact)).permissions() & QFileDevice::WriteOwner));

    const pdf::PDFArtifactStoreResult second = store.importBytes(payload, { QStringLiteral("application/pdf"), QStringLiteral("copy.pdf") });
    QVERIFY2(second.success, qPrintable(second.errorMessage));
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
    QVERIFY2(input.success, qPrintable(input.errorMessage));
    QVERIFY2(output.success, qPrintable(output.errorMessage));

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
    QVERIFY(history.registerArtifact(input.artifact));
    QVERIFY(history.registerArtifact(output.artifact));

    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("test.operation");
    execution.operationVersion = 2;
    execution.input = input.artifact;
    execution.sourceDocumentRevision = 7;
    execution.parameters = QJsonObject{{ QStringLiteral("password"), QStringLiteral("do-not-store") }, { QStringLiteral("mode"), QStringLiteral("safe") }};
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
    accepted.resultSummary = QJsonObject{{ QStringLiteral("password"), QStringLiteral("do-not-store") }};
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
    QVERIFY2(input.success, qPrintable(input.errorMessage));
    QVERIFY2(middle.success, qPrintable(middle.errorMessage));
    QVERIFY2(final.success, qPrintable(final.errorMessage));

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
    QVERIFY(history.registerOriginalInput(input.artifact));
    QVERIFY(history.registerArtifact(middle.artifact));
    QVERIFY(history.registerArtifact(final.artifact));

    auto appendAccepted = [&](const pdf::PDFArtifactIdentity& source,
                              const pdf::PDFArtifactIdentity& output,
                              const QString& operation,
                              bool approved,
                              QUuid* executionId) {
        pdf::PDFOperationHistoryExecution execution;
        execution.operationId = operation;
        execution.input = source;
        if (!history.beginExecution(execution, executionId)) return false;
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
    QVERIFY2(input.success, qPrintable(input.errorMessage));
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
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
    QVERIFY2(input.success, qPrintable(input.errorMessage));

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QString openError;
    QVERIFY2(history.open(&openError), qPrintable(openError));
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

QTEST_MAIN(OperationHistoryTest)
#include "tst_operationhistorytest.moc"
