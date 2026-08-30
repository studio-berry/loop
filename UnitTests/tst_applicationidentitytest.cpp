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

#include "pdfapplicationidentity.h"
#include "pdfconstants.h"
#include "pdfsettings.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <set>
#include <utility>

class ApplicationIdentityTest : public QObject
{
    Q_OBJECT

private slots:
    void surfacesUseCanonicalIdentity();
    void legacySettingsAreMigratedOnce();
    void corruptLegacySettingsDoNotBlockStartup();
};

void ApplicationIdentityTest::surfacesUseCanonicalIdentity()
{
    using Surface = pdf::PDFApplicationSurface;
    const std::array<std::pair<Surface, const char*>, 9> surfaces = {{
        { Surface::LoopEditor, "LoopEditor" },
        { Surface::PdfTool, "PdfTool" },
        { Surface::CodeGenerator, "CodeGenerator" },
        { Surface::Jbig2Viewer, "JBIG2Viewer" },
        { Surface::PdfExampleGenerator, "PdfExampleGenerator" },
        { Surface::LoopPreflightFixtureGenerator, "LoopGenerateFixtures" },
        { Surface::QuickShellSmoke, "QuickShellSmoke" },
        { Surface::ProductQuickAccessibilitySmoke, "ProductQuickAccessibilitySmoke" },
        { Surface::CanvasBenchmark, "CanvasBenchmark" },
    }};

    std::set<QString> applicationNames;
    for (const auto& [surface, expectedApplicationName] : surfaces)
    {
        const pdf::PDFApplicationIdentity identity = pdf::getApplicationIdentity(surface);
        QCOMPARE(identity.productName, QStringLiteral("Loop"));
        QCOMPARE(identity.organizationName, QStringLiteral("Loop"));
        QCOMPARE(identity.organizationDomain, QStringLiteral("io.github.mberrys"));
        QCOMPARE(identity.packageId, QStringLiteral("io.github.mberrys.Loop-pdf"));
        QCOMPARE(identity.applicationName, QString::fromLatin1(expectedApplicationName));
        QVERIFY(identity.displayName.startsWith(QStringLiteral("Loop")));
        QCOMPARE(identity.appUserModelId, identity.packageId + QLatin1Char('.') + identity.applicationName);
        QCOMPARE(identity.version, QString::fromLatin1(pdf::PDF_LIBRARY_VERSION));
        QVERIFY(applicationNames.insert(identity.applicationName).second);
    }
}

void ApplicationIdentityTest::legacySettingsAreMigratedOnce()
{
    QTemporaryDir settingsDirectory;
    QVERIFY(settingsDirectory.isValid());

    pdf::PDFSettings::setSettingsPath(settingsDirectory.path());
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopEditor);

    const QString legacyApplicationName = QStringLiteral("LO") + QStringLiteral("UPE Editor");
    QSettings legacy(QSettings::IniFormat,
                     QSettings::UserScope,
                     QStringLiteral("MelkaJ"),
                     legacyApplicationName);
    legacy.setValue(QStringLiteral("ColorScheme/colorScheme"), 2);
    legacy.sync();
    QCOMPARE(legacy.status(), QSettings::NoError);

    pdf::PDFSettings::migrateLegacySettings();

    QSettings target(QSettings::IniFormat,
                     QSettings::UserScope,
                     QStringLiteral("Loop"),
                     QStringLiteral("LoopEditor"));
    QCOMPARE(target.value(QStringLiteral("ColorScheme/colorScheme")).toInt(), 2);
    QCOMPARE(target.value(QStringLiteral("migration/legacyIdentityV1")).toInt(), 1);

    target.setValue(QStringLiteral("ColorScheme/colorScheme"), 3);
    target.sync();
    legacy.setValue(QStringLiteral("ColorScheme/colorScheme"), 1);
    legacy.sync();

    pdf::PDFSettings::migrateLegacySettings();

    QSettings migratedTarget(QSettings::IniFormat,
                             QSettings::UserScope,
                             QStringLiteral("Loop"),
                             QStringLiteral("LoopEditor"));
    QCOMPARE(migratedTarget.value(QStringLiteral("ColorScheme/colorScheme")).toInt(), 3);
}

void ApplicationIdentityTest::corruptLegacySettingsDoNotBlockStartup()
{
    QTemporaryDir settingsDirectory;
    QVERIFY(settingsDirectory.isValid());

    pdf::PDFSettings::setSettingsPath(settingsDirectory.path());
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopEditor);

    const QString legacyApplicationName = QStringLiteral("LO") + QStringLiteral("UPE Editor");
    QSettings legacy(QSettings::IniFormat,
                     QSettings::UserScope,
                     QStringLiteral("MelkaJ"),
                     legacyApplicationName);
    const QString legacyFileName = legacy.fileName();
    legacy.clear();
    legacy.sync();

    QFile corruptSettings(legacyFileName);
    QVERIFY(corruptSettings.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(corruptSettings.write("[broken\nkey=value\n") > 0);
    corruptSettings.close();

    pdf::PDFSettings::migrateLegacySettings();

    QSettings target(QSettings::IniFormat,
                     QSettings::UserScope,
                     QStringLiteral("Loop"),
                     QStringLiteral("LoopEditor"));
    QCOMPARE(target.value(QStringLiteral("migration/legacyIdentityV1")).toInt(), 1);
    QVERIFY(!target.contains(QStringLiteral("key")));
}

QTEST_GUILESS_MAIN(ApplicationIdentityTest)

#include "tst_applicationidentitytest.moc"
