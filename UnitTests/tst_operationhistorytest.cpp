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
    void externalPayloadTamperingCompromisesChain();
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
    QVERIFY(first.success);
    QCOMPARE(first.artifact.sha256, QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));
    QVERIFY(first.artifact.isValid());
    QVERIFY(store.verify(first.artifact));

    const pdf::PDFArtifactStoreResult second = store.importBytes(payload, { QStringLiteral("application/pdf"), QStringLiteral("copy.pdf") });
    QVERIFY(second.success);
    QVERIFY(second.reused);
    QVERIFY(store.verify(second.artifact));

    QFile file(store.pathFor(first.artifact));
    QVERIFY(file.open(QIODevice::Append));
    QVERIFY(file.write("tamper") > 0);
    file.close();
    QVERIFY(!store.verify(first.artifact));
}

void OperationHistoryTest::lifecycleApprovalAndRollbackResolution()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    const auto output = artifacts.importBytes("output", { QStringLiteral("application/pdf"), QStringLiteral("output.pdf") });
    QVERIFY(input.success && output.success);

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());
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

void OperationHistoryTest::externalPayloadTamperingCompromisesChain()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto input = artifacts.importBytes("source", { QStringLiteral("application/pdf"), QStringLiteral("input.pdf") });
    QVERIFY(input.success);
    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QVERIFY(history.open());
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

QTEST_MAIN(OperationHistoryTest)
#include "tst_operationhistorytest.moc"
