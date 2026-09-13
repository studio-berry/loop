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
#include "pdfapplicationidentity.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentcontext.h"
#include "pdfjobscheduler.h"
#include "pdfoperationhistorystore.h"
#include "pdfsavepolicy.h"
#include "pdfworkloadenvelope.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QtTest>

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>

class LifecycleTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void boundedTraceGenerationIsDeterministic();
    void qualificationCorpusSchemasAreValid();
    void qualificationCorpusSeedsMatchGoldenTraces();
    void qualificationCorpusReplayPreservesInvariants();
    void deltaDebugShrinkPreservesFailure();
    void promotedFailureTracesMatchExpectedViolations();
    void crossPlatformCorpusReportIsStable();
    void seededSequencePreservesInvariants();
    void injectedStaleResultIsCaught();
    void injectedOverwriteIsCaught();
    void injectedRollbackHistoryDefectIsCaught();
};

namespace
{

constexpr int kMaxTraceCommands = 64;
constexpr quint64 kPrimarySeed = UINT64_C(0x20260821);

const QStringList kExpectedInvariants = {
    QStringLiteral("source-immutable"),
    QStringLiteral("cancel-is-terminal"),
    QStringLiteral("stale-results-rejected"),
    QStringLiteral("history-append-only"),
};

const QStringList kAllowedCommandKinds = {
    QStringLiteral("open"),
    QStringLiteral("render-preflight"),
    QStringLiteral("cancel"),
    QStringLiteral("replace-revision"),
    QStringLiteral("save-reopen"),
    QStringLiteral("rollback"),
    QStringLiteral("close"),
};

enum class TraceCommandKind
{
    Open,
    RenderPreflight,
    Cancel,
    ReplaceRevision,
    SaveReopen,
    Rollback,
    Close,
};

enum class TraceReplayProfile
{
    None,
    InjectStaleAcceptance,
    InjectSourceOverwrite,
    InjectHistoryMutation,
};

QString traceCommandName(TraceCommandKind kind)
{
    switch (kind)
    {
        case TraceCommandKind::Open:
            return QStringLiteral("open");
        case TraceCommandKind::RenderPreflight:
            return QStringLiteral("render-preflight");
        case TraceCommandKind::Cancel:
            return QStringLiteral("cancel");
        case TraceCommandKind::ReplaceRevision:
            return QStringLiteral("replace-revision");
        case TraceCommandKind::SaveReopen:
            return QStringLiteral("save-reopen");
        case TraceCommandKind::Rollback:
            return QStringLiteral("rollback");
        case TraceCommandKind::Close:
            return QStringLiteral("close");
    }
    return QStringLiteral("unknown");
}

std::optional<TraceCommandKind> traceCommandKindFromName(const QString& name)
{
    if (name == QStringLiteral("open"))
    {
        return TraceCommandKind::Open;
    }
    if (name == QStringLiteral("render-preflight"))
    {
        return TraceCommandKind::RenderPreflight;
    }
    if (name == QStringLiteral("cancel"))
    {
        return TraceCommandKind::Cancel;
    }
    if (name == QStringLiteral("replace-revision"))
    {
        return TraceCommandKind::ReplaceRevision;
    }
    if (name == QStringLiteral("save-reopen"))
    {
        return TraceCommandKind::SaveReopen;
    }
    if (name == QStringLiteral("rollback"))
    {
        return TraceCommandKind::Rollback;
    }
    if (name == QStringLiteral("close"))
    {
        return TraceCommandKind::Close;
    }
    return std::nullopt;
}

struct TraceCommand
{
    TraceCommandKind kind;
    quint64 argument = 0;
};

quint64 nextTraceRandom(quint64& state)
{
    state += UINT64_C(0x9e3779b97f4a7c15);
    quint64 value = state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

QVector<TraceCommand> generateTrace(quint64 seed, int maxCommands = kMaxTraceCommands)
{
    const QVector<TraceCommandKind> activeCoverage = {
        TraceCommandKind::Open,
        TraceCommandKind::RenderPreflight,
        TraceCommandKind::Cancel,
        TraceCommandKind::ReplaceRevision,
        TraceCommandKind::SaveReopen,
        TraceCommandKind::Rollback,
    };
    QVector<TraceCommand> trace;
    trace.reserve(maxCommands);
    quint64 state = seed;
    for (const TraceCommandKind kind : activeCoverage)
    {
        trace.append({ kind, nextTraceRandom(state) });
    }
    while (trace.size() < maxCommands - 1)
    {
        const auto kind = activeCoverage.at(static_cast<qsizetype>(nextTraceRandom(state) % activeCoverage.size()));
        trace.append({ kind, nextTraceRandom(state) });
    }
    trace.append({ TraceCommandKind::Close, nextTraceRandom(state) });
    return trace;
}

QJsonObject traceToJson(quint64 seed,
                        const QVector<TraceCommand>& trace,
                        const QString& observedResult,
                        const QJsonArray& shrinkHistory)
{
    QJsonArray commands;
    for (qsizetype index = 0; index < trace.size(); ++index)
    {
        commands.append(QJsonObject{
            { QStringLiteral("index"), static_cast<int>(index) },
            { QStringLiteral("kind"), traceCommandName(trace.at(index).kind) },
            { QStringLiteral("argument"), QString::number(trace.at(index).argument) } });
    }
    return QJsonObject{
        { QStringLiteral("schema_kind"), QStringLiteral("loop-lifecycle-trace") },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("seed"), static_cast<qint64>(seed) },
        { QStringLiteral("initial_artifact_digest"), pdf::PDFRunIdentity::digestBytes(QByteArrayLiteral("lifecycle-source-v1")) },
        { QStringLiteral("commands"), commands },
        { QStringLiteral("expected_invariants"), QJsonArray::fromStringList(kExpectedInvariants) },
        { QStringLiteral("observed_result"), observedResult },
        { QStringLiteral("shrink_history"), shrinkHistory },
    };
}

struct LifecycleState
{
    QString sourceDigest;
    quint64 lastRevision = 0;
    bool open = false;
    bool recovered = false;
    bool certified = false;
    bool lastCancelled = false;
    bool lastSucceeded = false;
    bool acceptedStale = false;
    bool sourceOverwritten = false;
    bool historyMutated = false;
    pdf::PDFSaveMode lastSaveMode = pdf::PDFSaveMode::IncrementalAppend;
    QList<QUuid> eventIds;
    pdf::PDFArtifactIdentity original;
    pdf::PDFArtifactIdentity current;
    QUuid lastAcceptedExecution;
};

QString invariantFailure(const LifecycleState& state, const pdf::PDFArtifactStore& artifacts, const pdf::PDFOperationHistoryStore& history)
{
    if (!state.sourceDigest.isEmpty() && state.original.sha256 != state.sourceDigest)
    {
        return QStringLiteral("source-digest-changed");
    }
    if (state.sourceOverwritten)
    {
        return QStringLiteral("source-overwritten");
    }
    if (state.acceptedStale)
    {
        return QStringLiteral("stale-result-accepted");
    }
    if (state.lastCancelled && state.lastSucceeded)
    {
        return QStringLiteral("cancel-marked-success");
    }
    if (state.recovered && state.certified)
    {
        return QStringLiteral("recovered-output-certified");
    }
    if (state.historyMutated)
    {
        return QStringLiteral("rollback-history-mutated");
    }

    const QList<pdf::PDFOperationHistoryEvent> events = history.events();
    QList<QUuid> ids;
    qint64 previous = 0;
    for (const pdf::PDFOperationHistoryEvent& event : events)
    {
        if (event.sequence <= previous && previous != 0)
        {
            return QStringLiteral("event-sequence-not-monotonic");
        }
        previous = event.sequence;
        ids.append(event.entryId);
    }
    if (ids.size() < state.eventIds.size())
    {
        return QStringLiteral("provenance-not-append-only");
    }
    if (state.open && !artifacts.verify(state.original))
    {
        return QStringLiteral("original-artifact-unverified");
    }
    return QString();
}

bool appendEvent(pdf::PDFOperationHistoryStore& history,
                 const pdf::PDFArtifactIdentity& input,
                 pdf::PDFOperationHistoryEventKind kind,
                 pdf::PDFOperationHistoryStatus status,
                 QUuid* executionId,
                 LifecycleState* state,
                 const std::optional<pdf::PDFArtifactIdentity>& output = std::nullopt)
{
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("lifecycle.test");
    execution.input = input;
    if (!history.beginExecution(execution, executionId))
    {
        return false;
    }
    pdf::PDFOperationHistoryEvent event;
    event.executionId = *executionId;
    event.kind = kind;
    event.status = status;
    event.output = output;
    if (!history.appendEvent(event))
    {
        return false;
    }
    const QList<pdf::PDFOperationHistoryEvent> events = history.events();
    if (events.isEmpty())
    {
        return false;
    }
    state->eventIds.append(events.last().entryId);
    return true;
}

struct ReplayEnvironment
{
    QTemporaryDir temporary;
    pdf::PDFArtifactStore artifacts;
    pdf::PDFOperationHistoryStore history;
    pdf::PDFDocumentBuilder builder;
    pdf::PDFDocument document;
    pdf::PDFDocumentContext context;
    pdf::PDFJobScheduler scheduler;
    LifecycleState state;
    QString activeJobId;
    TraceReplayProfile profile = TraceReplayProfile::None;

    ReplayEnvironment() :
        artifacts(temporary.path()),
        history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"))),
        context(&document),
        scheduler(1)
    {
        builder.appendPage(QRectF(0, 0, 100, 100));
        document = builder.build();
        context.setDocument(&document);
    }

    bool isValid() const
    {
        return temporary.isValid();
    }
};

bool openDocument(ReplayEnvironment& environment)
{
    if (environment.state.open)
    {
        return true;
    }
    if (!environment.history.open())
    {
        return false;
    }
    const QByteArray originalBytes("lifecycle-source-v1");
    const auto imported = environment.artifacts.importBytes(originalBytes,
                                                            { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    if (!imported.success)
    {
        return false;
    }
    if (!environment.history.registerOriginalInput(imported.artifact))
    {
        return false;
    }
    environment.state.original = imported.artifact;
    environment.state.current = imported.artifact;
    environment.state.sourceDigest = imported.artifact.sha256;
    environment.state.open = true;
    environment.state.lastRevision = environment.context.getRevision().documentRevision;
    QUuid openedId;
    if (!appendEvent(environment.history, imported.artifact, pdf::PDFOperationHistoryEventKind::DocumentOpened,
                     pdf::PDFOperationHistoryStatus::Accepted, &openedId, &environment.state, imported.artifact))
    {
        return false;
    }
    if (environment.profile == TraceReplayProfile::InjectSourceOverwrite)
    {
        // importBytes publishes artifacts read-only, so re-enable the owner
        // write bit before corrupting; a silently failed append would make
        // the injected defect vanish on Unix-like hosts.
        const QString artifactPath = environment.artifacts.pathFor(imported.artifact);
        QFile::setPermissions(artifactPath,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther);
        QFile file(artifactPath);
        if (file.open(QIODevice::Append))
        {
            file.write("overwrite");
            file.close();
        }
        environment.state.sourceOverwritten = !environment.artifacts.verify(imported.artifact);
    }
    return true;
}

bool startPreflight(ReplayEnvironment& environment)
{
    if (!environment.state.open || !environment.activeJobId.isEmpty())
    {
        return true;
    }
    std::atomic_bool started = false;
    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.documentRevision = environment.context.getRevision().toString();
    environment.activeJobId = environment.scheduler.submit(spec, [&started](pdf::PDFJobContext& jobContext)
                                                           {
                                                               started = true;
                                                               while (!jobContext.isCancellationRequested())
                                                               {
                                                                   std::this_thread::yield();
                                                               } });
    for (int attempt = 0; attempt < 100 && !started.load(std::memory_order_acquire); ++attempt)
    {
        QThread::msleep(1);
    }
    return started.load(std::memory_order_acquire);
}

bool cancelPreflight(ReplayEnvironment& environment)
{
    if (environment.activeJobId.isEmpty())
    {
        return true;
    }
    if (!environment.scheduler.cancel(environment.activeJobId))
    {
        return false;
    }
    if (!environment.scheduler.waitForFinished(environment.activeJobId, 1000))
    {
        return false;
    }
    const pdf::PDFJobSnapshot snapshot = environment.scheduler.snapshot(environment.activeJobId);
    environment.state.lastCancelled = snapshot.status == pdf::PDFJobStatus::Cancelled;
    environment.state.lastSucceeded = snapshot.status == pdf::PDFJobStatus::Succeeded;
    QUuid cancelledId;
    appendEvent(environment.history, environment.state.current, pdf::PDFOperationHistoryEventKind::PreflightRun,
                pdf::PDFOperationHistoryStatus::Cancelled, &cancelledId, &environment.state);
    environment.activeJobId.clear();
    return true;
}

bool replaceRevision(ReplayEnvironment& environment, quint64 argument)
{
    Q_UNUSED(argument);
    if (!environment.state.open)
    {
        return false;
    }
    const pdf::PDFRevisionIdentity beforeEdit = environment.context.getRevision();
    environment.context.markModified(pdf::PDFModifiedDocument::PageContents);
    environment.state.lastRevision = environment.context.getRevision().documentRevision;
    return environment.context.getRevision().documentRevision > beforeEdit.documentRevision;
}

bool saveReopen(ReplayEnvironment& environment, quint64 argument)
{
    if (!environment.state.open)
    {
        return false;
    }
    const pdf::PDFOperationSavePolicy policy = (argument % 2 == 0)
                                                   ? pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("edit"))
                                                   : pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("export"));
    environment.state.lastSaveMode = policy.mode;
    const QByteArray payload = QByteArray("lifecycle-source-v1-") + QByteArray::number(argument);
    const auto saved = environment.artifacts.importBytes(payload,
                                                         { QStringLiteral("application/pdf"), QStringLiteral("edited.pdf") });
    if (!saved.success)
    {
        return false;
    }
    if (!environment.history.registerArtifact(saved.artifact))
    {
        return false;
    }
    environment.state.current = saved.artifact;
    QUuid savedId;
    return appendEvent(environment.history, environment.state.original, pdf::PDFOperationHistoryEventKind::FixApplied,
                       pdf::PDFOperationHistoryStatus::Accepted, &savedId, &environment.state, saved.artifact);
}

bool rollbackRevision(ReplayEnvironment& environment, quint64 argument)
{
    Q_UNUSED(argument);
    if (!environment.state.open)
    {
        return false;
    }
    QUuid rollbackId;
    if (!appendEvent(environment.history, environment.state.current, pdf::PDFOperationHistoryEventKind::FixApplied,
                     pdf::PDFOperationHistoryStatus::RolledBack, &rollbackId, &environment.state, environment.state.current))
    {
        return false;
    }
    const quint64 revisionBeforeRollback = environment.context.getRevision().documentRevision;
    environment.context.markModified(pdf::PDFModifiedDocument::PageContents);
    environment.state.recovered = true;
    environment.state.certified = false;
    return environment.context.getRevision().documentRevision > revisionBeforeRollback;
}

bool closeDocument(ReplayEnvironment& environment)
{
    if (!environment.state.open)
    {
        return true;
    }
    if (!environment.activeJobId.isEmpty())
    {
        if (!cancelPreflight(environment))
        {
            return false;
        }
    }
    environment.state.open = false;
    return true;
}

void applyHistoryMutationInjection(ReplayEnvironment& environment)
{
    if (environment.profile != TraceReplayProfile::InjectHistoryMutation)
    {
        return;
    }
    if (environment.history.events().size() < 2)
    {
        return;
    }
    const int eventCountBefore = environment.history.events().size();
    const QString databasePath = environment.history.databasePath();
    const QString connectionName = QStringLiteral("lifecycle-history-mutate-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    if (!database.open())
    {
        return;
    }
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("DELETE FROM history_events WHERE sequence = 1")))
    {
        environment.state.historyMutated = environment.history.events().size() < eventCountBefore;
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
}

bool executeTraceCommand(ReplayEnvironment& environment, const TraceCommand& command)
{
    switch (command.kind)
    {
        case TraceCommandKind::Open:
            return openDocument(environment);
        case TraceCommandKind::RenderPreflight:
            return startPreflight(environment);
        case TraceCommandKind::Cancel:
            return cancelPreflight(environment);
        case TraceCommandKind::ReplaceRevision:
            return replaceRevision(environment, command.argument);
        case TraceCommandKind::SaveReopen:
            return saveReopen(environment, command.argument);
        case TraceCommandKind::Rollback:
            return rollbackRevision(environment, command.argument);
        case TraceCommandKind::Close:
            return closeDocument(environment);
    }
    return false;
}

QString replayTrace(const QVector<TraceCommand>& trace, TraceReplayProfile profile = TraceReplayProfile::None)
{
    ReplayEnvironment environment;
    if (!environment.isValid())
    {
        return QStringLiteral("replay-environment-invalid");
    }
    environment.profile = profile;
    if (profile == TraceReplayProfile::InjectStaleAcceptance)
    {
        environment.state.acceptedStale = true;
    }
    for (const TraceCommand& command : trace)
    {
        if (!executeTraceCommand(environment, command))
        {
            return QStringLiteral("replay-command-failed");
        }
        const QString failure = invariantFailure(environment.state, environment.artifacts, environment.history);
        if (!failure.isEmpty())
        {
            return failure;
        }
    }
    applyHistoryMutationInjection(environment);
    return invariantFailure(environment.state, environment.artifacts, environment.history);
}

struct ShrinkResult
{
    QVector<TraceCommand> minimized;
    QJsonArray shrinkHistory;
};

ShrinkResult shrinkTrace(QVector<TraceCommand> trace, TraceReplayProfile profile, const QString& expectedViolation)
{
    const auto reproduces = [&](const QVector<TraceCommand>& candidate)
    {
        return replayTrace(candidate, profile) == expectedViolation;
    };

    ShrinkResult result;
    result.shrinkHistory.append(static_cast<int>(trace.size()));
    if (!reproduces(trace))
    {
        result.minimized = trace;
        return result;
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (int index = 0; index < trace.size(); ++index)
        {
            QVector<TraceCommand> candidate = trace;
            candidate.removeAt(index);
            if (candidate.isEmpty())
            {
                continue;
            }
            if (reproduces(candidate))
            {
                trace = candidate;
                result.shrinkHistory.append(static_cast<int>(trace.size()));
                changed = true;
                break;
            }
        }
    }
    result.minimized = trace;
    return result;
}

QString lifecycleCorpusDirectory()
{
    return QStringLiteral(LOOP_UNITTEST_SOURCE_DIR "/testdata/lifecycle");
}

QString validateTraceSchemaObject(const QJsonObject& object)
{
    if (object.value(QStringLiteral("schema_kind")).toString() != QStringLiteral("loop-lifecycle-trace"))
    {
        return QStringLiteral("schema_kind must be loop-lifecycle-trace");
    }
    if (object.value(QStringLiteral("schema_version")).toInt() != 1)
    {
        return QStringLiteral("schema_version must be 1");
    }
    if (!object.contains(QStringLiteral("seed")))
    {
        return QStringLiteral("seed is required");
    }
    if (object.value(QStringLiteral("initial_artifact_digest")).toString().isEmpty())
    {
        return QStringLiteral("initial_artifact_digest is required");
    }
    if (object.value(QStringLiteral("observed_result")).toString().isEmpty())
    {
        return QStringLiteral("observed_result is required");
    }
    if (!object.contains(QStringLiteral("shrink_history")) || !object.value(QStringLiteral("shrink_history")).isArray())
    {
        return QStringLiteral("shrink_history must be an array");
    }
    const QJsonArray commands = object.value(QStringLiteral("commands")).toArray();
    if (commands.isEmpty() || commands.size() > kMaxTraceCommands)
    {
        return QStringLiteral("commands must contain 1..64 entries");
    }
    for (int index = 0; index < commands.size(); ++index)
    {
        const QJsonObject command = commands.at(index).toObject();
        if (command.value(QStringLiteral("index")).toInt() != index)
        {
            return QStringLiteral("command index mismatch");
        }
        if (!kAllowedCommandKinds.contains(command.value(QStringLiteral("kind")).toString()))
        {
            return QStringLiteral("unknown command kind");
        }
    }
    const QJsonArray expected = object.value(QStringLiteral("expected_invariants")).toArray();
    for (const QJsonValue& value : expected)
    {
        if (!kExpectedInvariants.contains(value.toString()))
        {
            return QStringLiteral("unexpected invariant name");
        }
    }
    return QString();
}

std::optional<QVector<TraceCommand>> commandsFromJsonObject(const QJsonObject& object)
{
    QVector<TraceCommand> trace;
    const QJsonArray commands = object.value(QStringLiteral("commands")).toArray();
    trace.reserve(commands.size());
    for (const QJsonValue& value : commands)
    {
        const QJsonObject command = value.toObject();
        const std::optional<TraceCommandKind> kind = traceCommandKindFromName(command.value(QStringLiteral("kind")).toString());
        if (!kind.has_value())
        {
            return std::nullopt;
        }
        trace.append({ *kind, command.value(QStringLiteral("argument")).toString().toULongLong() });
    }
    return trace;
}

QJsonObject loadJsonObject(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        *error = QStringLiteral("unable to open %1").arg(path);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        *error = QStringLiteral("invalid JSON in %1").arg(path);
        return {};
    }
    return document.object();
}

QJsonArray loadCorpusManifestSeeds()
{
    QString error;
    const QJsonObject manifest = loadJsonObject(lifecycleCorpusDirectory() + QStringLiteral("/manifest.json"), &error);
    if (!error.isEmpty())
    {
        return {};
    }
    return manifest.value(QStringLiteral("passing_traces")).toArray();
}

QJsonArray loadCorpusManifestFailures()
{
    QString error;
    const QJsonObject manifest = loadJsonObject(lifecycleCorpusDirectory() + QStringLiteral("/manifest.json"), &error);
    if (!error.isEmpty())
    {
        return {};
    }
    return manifest.value(QStringLiteral("failure_traces")).toArray();
}

TraceReplayProfile profileFromName(const QString& name)
{
    if (name == QStringLiteral("inject-stale-acceptance"))
    {
        return TraceReplayProfile::InjectStaleAcceptance;
    }
    if (name == QStringLiteral("inject-source-overwrite"))
    {
        return TraceReplayProfile::InjectSourceOverwrite;
    }
    if (name == QStringLiteral("inject-history-mutation"))
    {
        return TraceReplayProfile::InjectHistoryMutation;
    }
    return TraceReplayProfile::None;
}

}   // namespace

void LifecycleTest::initTestCase()
{
    // The Loop identity contract (scripts/ci/check_loop_identity.py) forbids
    // direct QCoreApplication identity mutation outside
    // LoopLibCore/sources/pdfapplicationidentity.cpp, so tests that need a
    // stable QSettings namespace use the sanctioned core entry point instead.
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopEditor);
}

void LifecycleTest::boundedTraceGenerationIsDeterministic()
{
    const QVector<TraceCommand> first = generateTrace(kPrimarySeed);
    const QVector<TraceCommand> second = generateTrace(kPrimarySeed);
    QCOMPARE(first.size(), kMaxTraceCommands);
    QCOMPARE(QJsonDocument(traceToJson(kPrimarySeed, first, QStringLiteral("invariants-held"), QJsonArray())).toJson(QJsonDocument::Compact),
             QJsonDocument(traceToJson(kPrimarySeed, second, QStringLiteral("invariants-held"), QJsonArray())).toJson(QJsonDocument::Compact));

    const QString goldenPath = lifecycleCorpusDirectory() + QStringLiteral("/seed-20260821.json");
    QString error;
    const QJsonObject goldenObject = loadJsonObject(goldenPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(QJsonDocument(traceToJson(kPrimarySeed, first, QStringLiteral("invariants-held"), QJsonArray())).toJson(QJsonDocument::Compact),
             QJsonDocument(goldenObject).toJson(QJsonDocument::Compact));
}

void LifecycleTest::qualificationCorpusSchemasAreValid()
{
    const QJsonArray seeds = loadCorpusManifestSeeds();
    QVERIFY2(!seeds.isEmpty(), "lifecycle corpus manifest must list passing_traces");
    for (const QJsonValue& seedEntry : seeds)
    {
        const QJsonObject entry = seedEntry.toObject();
        const QString fileName = entry.value(QStringLiteral("trace_file")).toString();
        QVERIFY(!fileName.isEmpty());
        const QString path = lifecycleCorpusDirectory() + QStringLiteral("/") + fileName;
        QString error;
        const QJsonObject object = loadJsonObject(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QString schemaError = validateTraceSchemaObject(object);
        QVERIFY2(schemaError.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(fileName, schemaError)));
        QCOMPARE(object.value(QStringLiteral("commands")).toArray().size(), entry.value(QStringLiteral("command_count")).toInt());
    }

    const QJsonArray failures = loadCorpusManifestFailures();
    for (const QJsonValue& failureEntry : failures)
    {
        const QJsonObject entry = failureEntry.toObject();
        const QString fileName = entry.value(QStringLiteral("trace_file")).toString();
        const QString path = lifecycleCorpusDirectory() + QStringLiteral("/") + fileName;
        QString error;
        const QJsonObject object = loadJsonObject(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QString schemaError = validateTraceSchemaObject(object);
        QVERIFY2(schemaError.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(fileName, schemaError)));
    }
}

void LifecycleTest::qualificationCorpusSeedsMatchGoldenTraces()
{
    const QJsonArray seeds = loadCorpusManifestSeeds();
    for (const QJsonValue& seedEntry : seeds)
    {
        const QJsonObject entry = seedEntry.toObject();
        const quint64 seed = static_cast<quint64>(entry.value(QStringLiteral("seed")).toVariant().toULongLong());
        const QVector<TraceCommand> generated = generateTrace(seed);
        const QJsonObject generatedObject = traceToJson(seed, generated, QStringLiteral("invariants-held"), QJsonArray());
        const QString path = lifecycleCorpusDirectory() + QStringLiteral("/") + entry.value(QStringLiteral("trace_file")).toString();
        QString error;
        const QJsonObject goldenObject = loadJsonObject(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QJsonDocument(generatedObject).toJson(QJsonDocument::Compact),
                 QJsonDocument(goldenObject).toJson(QJsonDocument::Compact));
    }
}

void LifecycleTest::qualificationCorpusReplayPreservesInvariants()
{
    const QJsonArray seeds = loadCorpusManifestSeeds();
    for (const QJsonValue& seedEntry : seeds)
    {
        const QJsonObject entry = seedEntry.toObject();
        const QString path = lifecycleCorpusDirectory() + QStringLiteral("/") + entry.value(QStringLiteral("trace_file")).toString();
        QString error;
        const QJsonObject object = loadJsonObject(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const std::optional<QVector<TraceCommand>> trace = commandsFromJsonObject(object);
        QVERIFY(trace.has_value());
        const QString failure = replayTrace(*trace);
        QCOMPARE(failure, QString());
    }
}

void LifecycleTest::deltaDebugShrinkPreservesFailure()
{
    const QVector<TraceCommand> seedTrace = generateTrace(kPrimarySeed);
    const ShrinkResult staleShrink = shrinkTrace(seedTrace, TraceReplayProfile::InjectStaleAcceptance,
                                                 QStringLiteral("stale-result-accepted"));
    QVERIFY(staleShrink.minimized.size() < seedTrace.size());
    QCOMPARE(replayTrace(staleShrink.minimized, TraceReplayProfile::InjectStaleAcceptance),
             QStringLiteral("stale-result-accepted"));
    QVERIFY(staleShrink.shrinkHistory.size() >= 2);

    const ShrinkResult overwriteShrink = shrinkTrace(seedTrace, TraceReplayProfile::InjectSourceOverwrite,
                                                     QStringLiteral("source-overwritten"));
    QVERIFY(overwriteShrink.minimized.size() < seedTrace.size());
    QCOMPARE(replayTrace(overwriteShrink.minimized, TraceReplayProfile::InjectSourceOverwrite),
             QStringLiteral("source-overwritten"));

    const ShrinkResult historyShrink = shrinkTrace(seedTrace, TraceReplayProfile::InjectHistoryMutation,
                                                   QStringLiteral("rollback-history-mutated"));
    QVERIFY(historyShrink.minimized.size() < seedTrace.size());
    QCOMPARE(replayTrace(historyShrink.minimized, TraceReplayProfile::InjectHistoryMutation),
             QStringLiteral("rollback-history-mutated"));
}

void LifecycleTest::promotedFailureTracesMatchExpectedViolations()
{
    const QJsonArray failures = loadCorpusManifestFailures();
    QVERIFY2(!failures.isEmpty(), "promoted failure traces must be listed in manifest.json");
    for (const QJsonValue& failureEntry : failures)
    {
        const QJsonObject entry = failureEntry.toObject();
        const QString path = lifecycleCorpusDirectory() + QStringLiteral("/") + entry.value(QStringLiteral("trace_file")).toString();
        QString error;
        const QJsonObject object = loadJsonObject(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const std::optional<QVector<TraceCommand>> trace = commandsFromJsonObject(object);
        QVERIFY(trace.has_value());
        const TraceReplayProfile profile = profileFromName(entry.value(QStringLiteral("replay_profile")).toString());
        const QString expected = entry.value(QStringLiteral("expected_violation")).toString();
        QCOMPARE(replayTrace(*trace, profile), expected);
        QCOMPARE(object.value(QStringLiteral("observed_result")).toString(), expected);
        const QJsonArray shrinkHistory = object.value(QStringLiteral("shrink_history")).toArray();
        QVERIFY2(!shrinkHistory.isEmpty(), "promoted traces must record shrink_history");
        QCOMPARE(shrinkHistory.last().toInt(), static_cast<int>(trace->size()));
    }
}

void LifecycleTest::crossPlatformCorpusReportIsStable()
{
    QJsonArray seedResults;
    const QJsonArray seeds = loadCorpusManifestSeeds();
    for (const QJsonValue& seedEntry : seeds)
    {
        const QJsonObject entry = seedEntry.toObject();
        const QString path = lifecycleCorpusDirectory() + QStringLiteral("/") + entry.value(QStringLiteral("trace_file")).toString();
        QString error;
        const QJsonObject object = loadJsonObject(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const std::optional<QVector<TraceCommand>> trace = commandsFromJsonObject(object);
        QVERIFY(trace.has_value());
        const QString failure = replayTrace(*trace);
        seedResults.append(QJsonObject{
            { QStringLiteral("seed"), entry.value(QStringLiteral("seed")) },
            { QStringLiteral("file"), entry.value(QStringLiteral("trace_file")) },
            { QStringLiteral("observed_result"), failure.isEmpty() ? QStringLiteral("invariants-held") : failure },
            { QStringLiteral("passed"), failure.isEmpty() },
        });
    }

    QJsonArray failureResults;
    const QJsonArray failures = loadCorpusManifestFailures();
    for (const QJsonValue& failureEntry : failures)
    {
        const QJsonObject entry = failureEntry.toObject();
        const QString path = lifecycleCorpusDirectory() + QStringLiteral("/") + entry.value(QStringLiteral("trace_file")).toString();
        QString error;
        const QJsonObject object = loadJsonObject(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const std::optional<QVector<TraceCommand>> trace = commandsFromJsonObject(object);
        QVERIFY(trace.has_value());
        const TraceReplayProfile profile = profileFromName(entry.value(QStringLiteral("replay_profile")).toString());
        const QString observed = replayTrace(*trace, profile);
        failureResults.append(QJsonObject{
            { QStringLiteral("file"), entry.value(QStringLiteral("trace_file")) },
            { QStringLiteral("expected_violation"), observed },
            { QStringLiteral("passed"), observed == entry.value(QStringLiteral("expected_violation")).toString() },
        });
    }

    const QJsonObject report{
        { QStringLiteral("schema_kind"), QStringLiteral("loop-lifecycle-corpus-report") },
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("platform"), QSysInfo::productType() },
        { QStringLiteral("kernel"), QSysInfo::kernelType() },
        { QStringLiteral("cpu_arch"), QSysInfo::currentCpuArchitecture() },
        { QStringLiteral("seed_results"), seedResults },
        { QStringLiteral("failure_results"), failureResults },
    };
    QVERIFY(!report.value(QStringLiteral("platform")).toString().isEmpty());
    for (const QJsonValue& value : seedResults)
    {
        QVERIFY(value.toObject().value(QStringLiteral("passed")).toBool());
    }
    for (const QJsonValue& value : failureResults)
    {
        QVERIFY(value.toObject().value(QStringLiteral("passed")).toBool());
    }
}

void LifecycleTest::seededSequencePreservesInvariants()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentContext context(&document);
    const QByteArray originalBytes("lifecycle-source-v1");
    const auto imported = artifacts.importBytes(originalBytes, { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    QVERIFY(imported.success);
    QVERIFY(history.registerOriginalInput(imported.artifact));

    LifecycleState state;
    state.original = imported.artifact;
    state.current = imported.artifact;
    state.sourceDigest = imported.artifact.sha256;
    state.open = true;
    state.lastRevision = context.getRevision().documentRevision;
    QUuid openedId;
    QVERIFY(appendEvent(history, imported.artifact, pdf::PDFOperationHistoryEventKind::DocumentOpened,
                        pdf::PDFOperationHistoryStatus::Accepted, &openedId, &state, imported.artifact));

    const pdf::PDFRevisionIdentity beforeEdit = context.getRevision();
    context.markModified(pdf::PDFModifiedDocument::PageContents);
    QVERIFY(context.getRevision().documentRevision > beforeEdit.documentRevision);
    state.lastRevision = context.getRevision().documentRevision;

    pdf::PDFJobScheduler scheduler(1);
    std::atomic_bool started = false;
    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Preflight;
    spec.documentRevision = context.getRevision().toString();
    const QString jobId = scheduler.submit(spec, [&started](pdf::PDFJobContext& jobContext)
                                           {
                                               started = true;
                                               while (!jobContext.isCancellationRequested())
                                               {
                                                   std::this_thread::yield();
                                               } });
    QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 1000);
    QVERIFY(scheduler.cancel(jobId));
    QVERIFY(scheduler.waitForFinished(jobId, 1000));
    QCOMPARE(scheduler.snapshot(jobId).status, pdf::PDFJobStatus::Cancelled);
    state.lastCancelled = true;
    state.lastSucceeded = false;
    QUuid cancelledId;
    QVERIFY(appendEvent(history, state.current, pdf::PDFOperationHistoryEventKind::PreflightRun,
                        pdf::PDFOperationHistoryStatus::Cancelled, &cancelledId, &state));

    const pdf::PDFOperationSavePolicy incremental = pdf::PDFOperationSavePolicy::incrementalAppend(QStringLiteral("edit"));
    QCOMPARE(incremental.mode, pdf::PDFSaveMode::IncrementalAppend);
    state.lastSaveMode = incremental.mode;
    const auto saved = artifacts.importBytes(QByteArray("lifecycle-source-v1-incremental"),
                                             { QStringLiteral("application/pdf"), QStringLiteral("edited.pdf") });
    QVERIFY(saved.success);
    QVERIFY(history.registerArtifact(saved.artifact));
    QVERIFY(artifacts.verify(state.original));
    state.current = saved.artifact;
    QUuid savedId;
    QVERIFY(appendEvent(history, state.original, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::Accepted, &savedId, &state, saved.artifact));
    state.lastAcceptedExecution = savedId;

    const pdf::PDFOperationSavePolicy saveAs = pdf::PDFOperationSavePolicy::saveAsNewArtifact(QStringLiteral("export"));
    QCOMPARE(saveAs.mode, pdf::PDFSaveMode::SaveAsNewArtifact);
    state.lastSaveMode = saveAs.mode;

    const quint64 revisionBeforeRollback = context.getRevision().documentRevision;
    QUuid rollbackId;
    QVERIFY(appendEvent(history, state.current, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::RolledBack, &rollbackId, &state, state.current));
    context.markModified(pdf::PDFModifiedDocument::PageContents);
    QVERIFY(context.getRevision().documentRevision > revisionBeforeRollback);

    state.recovered = true;
    state.certified = false;
    QVERIFY(artifacts.verify(state.original));
    QCOMPARE(invariantFailure(state, artifacts, history), QString());
}

void LifecycleTest::injectedStaleResultIsCaught()
{
    pdf::PDFJobScheduler scheduler(1);
    scheduler.setCurrentRevision(QStringLiteral("doc"), QStringLiteral("revision-2"));
    std::atomic_bool ran = false;
    pdf::PDFJobSpec spec;
    spec.documentKey = QStringLiteral("doc");
    spec.documentRevision = QStringLiteral("revision-1");
    spec.staleResultPolicy = pdf::PDFJobStaleResultPolicy::Discard;
    const QString jobId = scheduler.submit(spec, [&ran](pdf::PDFJobContext&)
                                           { ran = true; });
    QVERIFY(scheduler.waitForFinished(jobId, 1000));
    QCOMPARE(scheduler.snapshot(jobId).status, pdf::PDFJobStatus::Stale);
    QVERIFY(!ran.load(std::memory_order_acquire));

    LifecycleState state;
    state.acceptedStale = true;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());
    QCOMPARE(invariantFailure(state, artifacts, history), QStringLiteral("stale-result-accepted"));
}

void LifecycleTest::injectedOverwriteIsCaught()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto imported = artifacts.importBytes("protected-source", { QStringLiteral("application/pdf"), QStringLiteral("source.pdf") });
    QVERIFY(imported.success);
    QVERIFY(artifacts.verify(imported.artifact));

    QFile file(artifacts.pathFor(imported.artifact));
    QVERIFY(QFile::setPermissions(file.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(file.open(QIODevice::Append));
    QVERIFY(file.write("overwrite") > 0);
    file.close();

    LifecycleState state;
    state.original = imported.artifact;
    state.sourceDigest = imported.artifact.sha256;
    state.open = true;
    state.sourceOverwritten = !artifacts.verify(imported.artifact);
    QVERIFY(state.sourceOverwritten);

    pdf::PDFOperationHistoryStore history(QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(history.open());
    QCOMPARE(invariantFailure(state, artifacts, history), QStringLiteral("source-overwritten"));
}

void LifecycleTest::injectedRollbackHistoryDefectIsCaught()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pdf::PDFArtifactStore artifacts(temporary.path());
    const auto first = artifacts.importBytes("one", { QStringLiteral("application/pdf"), QStringLiteral("one.pdf") });
    const auto second = artifacts.importBytes("two", { QStringLiteral("application/pdf"), QStringLiteral("two.pdf") });
    QVERIFY(first.success);
    QVERIFY(second.success);

    const QString databasePath = QDir(temporary.path()).filePath(QStringLiteral("history.sqlite3"));
    pdf::PDFOperationHistoryStore history(databasePath);
    QVERIFY(history.open());
    QVERIFY(history.registerArtifact(first.artifact));
    QVERIFY(history.registerArtifact(second.artifact));

    LifecycleState state;
    QUuid firstId;
    QUuid secondId;
    QVERIFY(appendEvent(history, first.artifact, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::Accepted, &firstId, &state, first.artifact));
    QVERIFY(appendEvent(history, second.artifact, pdf::PDFOperationHistoryEventKind::FixApplied,
                        pdf::PDFOperationHistoryStatus::Accepted, &secondId, &state, second.artifact));
    const int before = history.events().size();

    const QString connectionName = QStringLiteral("lifecycle-history-mutate");
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("DELETE FROM history_events WHERE sequence = 1")));
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);

    QVERIFY(history.events().size() < before);
    state.historyMutated = true;
    QCOMPARE(invariantFailure(state, artifacts, history), QStringLiteral("rollback-history-mutated"));
}

QTEST_GUILESS_MAIN(LifecycleTest)
#include "tst_lifecycletest.moc"
