#include "editorhost.h"
#include "operatorhelp.h"

#include "commanddescriptor.h"
#include "inspectormodel.h"
#include "preflightcontroller.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"

#include <QSet>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSettings>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <memory>

namespace
{

constexpr EditorHost::LoopWorkspace kWorkspaces[] = {
    EditorHost::Document,
    EditorHost::Preflight,
    EditorHost::ProductionPreview,
    EditorHost::Pages,
    EditorHost::Inspect,
    EditorHost::Fix,
    EditorHost::Compare,
};

}   // namespace

class ShellWorkspaceTest : public QObject
{
    Q_OBJECT

private slots:
    void workspaceTransitionsPreserveClosedDocumentState();
    void workspaceTransitionsPreserveOpenDocumentAndPreflightState();
    void compareWorkspaceIsReachable();
    void menuPolicyRoutesVisibleActions();
    void developerDiagnosticsFollowReleaseProfile();
    void helpMatchesEveryCatalogFinding();
    void welcomePreferenceSurvivesRestart();
    void sampleCopiesPreserveOperatorEdits();
    void samplesExerciseRealCoreOutcomes();
    void correctionSampleRunsGovernedCore();
    void helpDialogShowsCatalogLimitsAndWelcome();
};

void ShellWorkspaceTest::workspaceTransitionsPreserveClosedDocumentState()
{
    EditorHost host;
    QCOMPARE(host.workspace(), EditorHost::Document);
    QCOMPARE(host.documentShellStatus(), QStringLiteral("NO_DOCUMENT"));
    QCOMPARE(host.preflightStateName(), QStringLiteral("not-checked"));

    for (const EditorHost::LoopWorkspace from : kWorkspaces)
    {
        host.setWorkspace(from);
        for (const EditorHost::LoopWorkspace to : kWorkspaces)
        {
            QSignalSpy workspaceSpy(&host, &EditorHost::workspaceChanged);
            host.setWorkspace(to);
            if (from != to)
            {
                QCOMPARE(workspaceSpy.size(), 1);
            }
            QVERIFY(!host.hasDocument());
            QCOMPARE(host.documentShellStatus(), QStringLiteral("NO_DOCUMENT"));
            QCOMPARE(host.preflightStateName(), QStringLiteral("not-checked"));
        }
    }
}

void ShellWorkspaceTest::workspaceTransitionsPreserveOpenDocumentAndPreflightState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 612, 792));
    const pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentWriter writer(nullptr);
    const QString path = directory.filePath(QStringLiteral("shell.pdf"));
    QVERIFY(writer.write(path, &document, true));

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY(host.hasDocument());

    auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    QVERIFY(preflight != nullptr);
    const QString revisionBefore = preflight->documentRevision();
    const QString preflightStateBefore = host.preflightStateName();
    const int pageCountBefore = host.pageCount();

    for (const EditorHost::LoopWorkspace from : kWorkspaces)
    {
        host.setWorkspace(from);
        for (const EditorHost::LoopWorkspace to : kWorkspaces)
        {
            host.setWorkspace(to);
            QVERIFY(host.hasDocument());
            QCOMPARE(host.pageCount(), pageCountBefore);
            QCOMPARE(preflight->documentRevision(), revisionBefore);
            QCOMPARE(host.preflightStateName(), preflightStateBefore);
            QVERIFY(!host.documentShellStatus().isEmpty());
            QVERIFY(!host.productionStateName().isEmpty());
        }
    }
}

void ShellWorkspaceTest::compareWorkspaceIsReachable()
{
    EditorHost host;
    QCOMPARE(host.workspace(), EditorHost::Document);
    QVERIFY(host.isWorkspaceEnabled(EditorHost::Compare));

    QSignalSpy workspaceSpy(&host, &EditorHost::workspaceChanged);
    host.setWorkspace(EditorHost::Compare);

    QCOMPARE(host.workspace(), EditorHost::Compare);
    QCOMPARE(workspaceSpy.size(), 1);
    QVERIFY(host.isWorkspaceEnabled(EditorHost::Document));
    QVERIFY(host.isWorkspaceEnabled(EditorHost::Inspect));
    QVERIFY(host.isWorkspaceEnabled(EditorHost::Fix));
}

void ShellWorkspaceTest::menuPolicyRoutesVisibleActions()
{
    EditorHost host;
    const QVariantList descriptors = host.commandDescriptors();
    QVERIFY(descriptors.size() > 100);

    static const QSet<QString> allowedGroups = {
        QStringLiteral("File"),
        QStringLiteral("Edit"),
        QStringLiteral("View"),
        QStringLiteral("Document"),
        QStringLiteral("Production"),
        QStringLiteral("Preflight"),
        QStringLiteral("Help"),
        QStringLiteral("Advanced"),
    };

    for (const QVariant& entryVariant : descriptors)
    {
        const QVariantMap entry = entryVariant.toMap();
        const QString disposition = entry.value(QStringLiteral("disposition")).toString();
        if (disposition == QStringLiteral("HIDE") || disposition == QStringLiteral("STOP-SHIPPING"))
        {
            continue;
        }

        if (disposition == QStringLiteral("ADVANCED") && !host.allowDeveloperDiagnostics())
        {
            continue;
        }

        const QString menuGroup = entry.value(QStringLiteral("menuGroup")).toString();
        QVERIFY2(allowedGroups.contains(menuGroup),
                 qPrintable(QStringLiteral("action %1 missing menu route").arg(entry.value(QStringLiteral("id")).toString())));
        QVERIFY(!entry.value(QStringLiteral("target")).toString().isEmpty());
    }
}

void ShellWorkspaceTest::developerDiagnosticsFollowReleaseProfile()
{
    EditorHost host;
#ifdef LOOP_LOOP_DISTRIBUTION_BUILD
    QVERIFY(!host.allowDeveloperDiagnostics());
#else
    QVERIFY(host.allowDeveloperDiagnostics());
#endif
}

void ShellWorkspaceTest::helpMatchesEveryCatalogFinding()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    OperatorHelp help(settings, directory.filePath(QStringLiteral("samples")));
    QVERIFY(help.diagnostic().isEmpty());
    const QVariantMap catalog = help.catalog();
    const QVariantMap checks = catalog.value(QStringLiteral("checks")).toMap();
    for (const QVariant& checkId : catalog.value(QStringLiteral("registry")).toList())
    {
        const QVariantMap check = checks.value(checkId.toString()).toMap();
        QVERIFY(!check.isEmpty());
        for (const QVariant& severity : check.value(QStringLiteral("severity")).toList())
        {
            const QString type = severity.toMap().value(QStringLiteral("finding_type")).toString();
            const QVariantMap topic = help.findingHelp(checkId.toString(), type);
            for (const QString& field : { QStringLiteral("measures"), QStringLiteral("limitations"), QStringLiteral("coverage") })
            {
                QCOMPARE(topic.value(field), check.value(field));
            }
            QCOMPARE(topic.value(QStringLiteral("condition")), severity.toMap().value(QStringLiteral("condition")));
        }
    }
    QCOMPARE(help.guide().value(QStringLiteral("workspaces")).toList().size(), 7);
    for (const EditorHost::LoopWorkspace workspace : kWorkspaces)
    {
        QVERIFY(!help.workspaceHelp(workspace).isEmpty());
    }
    QVERIFY(help.findingHelp(QStringLiteral("unregistered"), QString()).value(QStringLiteral("coverage")).isNull());
}

void ShellWorkspaceTest::welcomePreferenceSurvivesRestart()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        OperatorHelp help(settings, directory.path());
        QVERIFY(help.welcomeNeeded());
        QSignalSpy changed(&help, &OperatorHelp::welcomeChanged);
        QVERIFY(help.acknowledgeWelcome());
        QCOMPARE(changed.size(), 1);
        QVERIFY(!help.welcomeNeeded());
    }
    QSettings settings(path, QSettings::IniFormat);
    OperatorHelp restarted(settings, directory.path());
    QVERIFY(!restarted.welcomeNeeded());
}

void ShellWorkspaceTest::sampleCopiesPreserveOperatorEdits()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    OperatorHelp help(settings, directory.filePath(QStringLiteral("samples")));
    QVERIFY(!help.prepareSample(QStringLiteral("../outside")).value(QStringLiteral("ok")).toBool());
    const QVariantMap copy = help.prepareSample(QStringLiteral("correction"));
    QVERIFY2(copy.value(QStringLiteral("ok")).toBool(), qPrintable(copy.value(QStringLiteral("error")).toString()));
    const QString path = copy.value(QStringLiteral("pdf")).toUrl().toLocalFile();
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray edited("operator edit");
    QCOMPARE(file.write(edited), edited.size());
    file.close();
    const QVariantMap refused = help.prepareSample(QStringLiteral("correction"));
    QVERIFY(!refused.value(QStringLiteral("ok")).toBool());
    QVERIFY(!refused.value(QStringLiteral("error")).toString().isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), edited);
}

void ShellWorkspaceTest::samplesExerciseRealCoreOutcomes()
{
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    OperatorHelp help(settings, directory.filePath(QStringLiteral("samples")));
    for (const QVariant& job : help.sampleJobs())
    {
        const QVariantMap descriptor = job.toMap();
        const QVariantMap sample = help.prepareSample(descriptor.value(QStringLiteral("id")).toString());
        QVERIFY(sample.value(QStringLiteral("ok")).toBool());
        EditorHost host;
        QVERIFY(host.importPreflightProfileFileUrl(sample.value(QStringLiteral("profile")).toUrl()));
        host.openFileUrl(sample.value(QStringLiteral("pdf")).toUrl());
        QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
        QVERIFY(host.runPreflight());
        auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
        QVERIFY(preflight);
        QTRY_VERIFY_WITH_TIMEOUT(preflight->state() != pdfinteraction::PreflightController::State::Running, 30000);
        const QString expected = descriptor.value(QStringLiteral("expected")).toString();
        const auto expectedState = expected == QLatin1String("pass")         ? pdfinteraction::PreflightController::State::Pass
                                   : expected == QLatin1String("incomplete") ? pdfinteraction::PreflightController::State::Incomplete
                                                                             : pdfinteraction::PreflightController::State::Findings;
        QCOMPARE(preflight->state(), expectedState);
    }
}

void ShellWorkspaceTest::correctionSampleRunsGovernedCore()
{
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    OperatorHelp help(settings, directory.filePath(QStringLiteral("samples")));
    const QVariantMap sample = help.prepareSample(QStringLiteral("correction"));
    QVERIFY(sample.value(QStringLiteral("ok")).toBool());
    EditorHost host;
    QVERIFY(host.importPreflightProfileFileUrl(sample.value(QStringLiteral("profile")).toUrl()));
    QVERIFY(host.importActionListRecipe(sample.value(QStringLiteral("recipe")).toUrl()));
    host.openFileUrl(sample.value(QStringLiteral("pdf")).toUrl());
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QLatin1String("planned"), 60000);
    QVERIFY2(host.fixLifecycleStateName() == QLatin1String("preview-ready"), qPrintable(host.fixLifecycleSummary()));
    QVERIFY(host.approveActionListPlan());
    QVERIFY(host.executeApprovedActionListPlan());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QLatin1String("executing"), 120000);
    QVERIFY2(host.fixLifecycleStateName() == QLatin1String("succeeded"), qPrintable(host.fixLifecycleSummary()));
    QCOMPARE(host.fixSignOff().value(QStringLiteral("status")).toString(), QStringLiteral("signed-off"));
    const QVariantMap governed = host.fixSignOff().value(QStringLiteral("governed")).toMap();
    QVERIFY(governed.value(QStringLiteral("revalidation")).toMap().value(QStringLiteral("bytes_verified")).toBool());
    QVERIFY(!governed.value(QStringLiteral("sign_off")).toMap().value(QStringLiteral("published_sha256")).toString().isEmpty());
}

void ShellWorkspaceTest::helpDialogShowsCatalogLimitsAndWelcome()
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    OperatorHelp help(settings, directory.filePath(QStringLiteral("samples")));
    QQmlEngine engine;
    EditorHost host;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(LOOP_OPERATOR_SOURCE_DIR "/LoopEditor/qml/OperatorHelpDialog.qml")));
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 10000);
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    QQuickWindow window;
    window.resize(800, 700);
    window.show();
    std::unique_ptr<QObject> dialog(component.createWithInitialProperties({ { QStringLiteral("helpController"), QVariant::fromValue(&help) },
                                                                            { QStringLiteral("host"), QVariant::fromValue(&host) },
                                                                            { QStringLiteral("parent"), QVariant::fromValue(window.contentItem()) } }));
    QVERIFY2(dialog, qPrintable(component.errorString()));
    QVERIFY(dialog->property("title").toString().contains(QStringLiteral("Welcome")));
    QVERIFY(QMetaObject::invokeMethod(dialog.get(), "open"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    const QString screenshot = qEnvironmentVariable("LOOP_OPERATOR_HELP_SCREENSHOT");
    if (!screenshot.isEmpty())
    {
        QTest::qWait(300);
        QVERIFY(window.grabWindow().save(screenshot));
    }
    QObject* selector = dialog->findChild<QObject*>(QStringLiteral("findingHelpSelector"));
    QVERIFY(selector);
    const QVariantList registry = help.catalog().value(QStringLiteral("registry")).toList();
    const QVariantMap checks = help.catalog().value(QStringLiteral("checks")).toMap();
    for (int i = 0; i < registry.size(); ++i)
    {
        selector->setProperty("currentIndex", i);
        const QVariantMap check = checks.value(registry.at(i).toString()).toMap();
        for (const QString& field : { QStringLiteral("Coverage"), QStringLiteral("Measures"), QStringLiteral("Limitations") })
        {
            QObject* label = dialog->findChild<QObject*>(QStringLiteral("findingHelp") + field);
            QVERIFY(label);
            QTRY_COMPARE(label->property("text").toString(), field + QStringLiteral(": ") + check.value(field.toLower()).toString());
        }
    }
    QObject* dismiss = dialog->findChild<QObject*>(QStringLiteral("dismissOperatorWelcome"));
    QVERIFY(dismiss);
    QVERIFY(QMetaObject::invokeMethod(dismiss, "clicked"));
    QVERIFY(!help.welcomeNeeded());
    QVERIFY(QMetaObject::invokeMethod(dialog.get(), "openSample", Q_ARG(QVariant, QStringLiteral("correction"))));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 30000);
    QCOMPARE(host.workspace(), EditorHost::Preflight);
    QVERIFY(dialog->property("sampleError").toString().isEmpty());
    QVERIFY(host.runPreflight());
    auto* preflight = qobject_cast<pdfinteraction::PreflightController*>(host.preflight());
    QVERIFY(preflight);
    QTRY_VERIFY_WITH_TIMEOUT(preflight->state() != pdfinteraction::PreflightController::State::Running, 30000);
    QCOMPARE(preflight->state(), pdfinteraction::PreflightController::State::Findings);
    QVERIFY(host.validateActionListRecipe());
    QTRY_VERIFY_WITH_TIMEOUT(host.actionList()->property("validationReady").toBool(), 30000);
    QVERIFY(host.planActionList());
    QTRY_VERIFY_WITH_TIMEOUT(host.fixLifecycleStateName() != QStringLiteral("planned"), 60000);
    QVERIFY2(host.fixLifecycleStateName() == QStringLiteral("preview-ready"), qPrintable(host.fixLifecycleSummary()));
}

QTEST_MAIN(ShellWorkspaceTest)
#include "tst_shellworkspacetest.moc"
