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

#include "pdfaccessibility.h"
#include "pdfuitheme.h"

#include <QAction>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>

#include <QtTest/QtTest>

#include <algorithm>

class AccessibilityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void contrastTargets();
    void menuAuditFindsMissingAndDuplicateMnemonics();
    void widgetAuditFindsMissingNames();
    void actionNamesAndHighDpiSizingAreDerived();
};

void AccessibilityTest::contrastTargets()
{
    QCOMPARE(pdf::PDFAccessibility::contrastRatio(Qt::black, Qt::white), 21.0);
    QVERIFY(pdf::PDFUITheme::meetsContrast(QColor(176, 0, 32), Qt::white, 4.5));
    QVERIFY(pdf::PDFUITheme::meetsContrast(QColor(150, 75, 0), Qt::white, 4.5));
}

void AccessibilityTest::menuAuditFindsMissingAndDuplicateMnemonics()
{
    QMainWindow window;
    QMenu* fileMenu = window.menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("Open"));
    fileMenu->addAction(QStringLiteral("&Print"));
    fileMenu->addAction(QStringLiteral("&Preferences"));

    const QVector<pdf::MnemonicProblem> problems = pdf::PDFAccessibility::auditMenus(&window);
    QVERIFY(std::any_of(problems.cbegin(), problems.cend(), [](const pdf::MnemonicProblem& problem) {
        return problem.kind == pdf::MnemonicProblem::Kind::Missing
               && problem.actionText == QStringLiteral("Open");
    }));
    QVERIFY(std::count_if(problems.cbegin(), problems.cend(), [](const pdf::MnemonicProblem& problem) {
        return problem.kind == pdf::MnemonicProblem::Kind::Duplicate;
    }) >= 2);
}

void AccessibilityTest::widgetAuditFindsMissingNames()
{
    QWidget root;
    QLineEdit lineEdit(&root);
    lineEdit.setObjectName(QStringLiteral("pageFilter"));
    QToolButton iconButton(&root);
    iconButton.setObjectName(QStringLiteral("iconButton"));

    const QVector<pdf::AccessibilityFinding> findings = pdf::PDFAccessibility::auditWidgetTree(&root);
    QVERIFY(std::any_of(findings.cbegin(), findings.cend(), [](const pdf::AccessibilityFinding& finding) {
        return finding.code == QStringLiteral("missing-accessible-name")
               && finding.objectPath.endsWith(QStringLiteral("pageFilter"));
    }));
    QVERIFY(std::any_of(findings.cbegin(), findings.cend(), [](const pdf::AccessibilityFinding& finding) {
        return finding.code == QStringLiteral("missing-icon-button-name")
               && finding.objectPath.endsWith(QStringLiteral("iconButton"));
    }));

    lineEdit.setAccessibleName(QStringLiteral("Page filter"));
    iconButton.setAccessibleName(QStringLiteral("Open tools"));
    QVERIFY(pdf::PDFAccessibility::auditWidgetTree(&root).isEmpty());
}

void AccessibilityTest::actionNamesAndHighDpiSizingAreDerived()
{
    QAction action(QStringLiteral("&Open"));
    action.setToolTip(QStringLiteral("Open a PDF document."));
    QToolButton button;
    button.setDefaultAction(&action);
    pdf::PDFAccessibility::applyActionAccessibility(&button, &action);
    QCOMPARE(button.accessibleName(), QStringLiteral("Open"));
    QCOMPARE(button.accessibleDescription(), action.toolTip());

    QSpinBox spinBox;
    const int width = pdf::PDFAccessibility::minimumSpinBoxWidth(&spinBox, QStringLiteral("88888"));
    QVERIFY(width > spinBox.fontMetrics().horizontalAdvance(QStringLiteral("88888")));
}

QTEST_MAIN(AccessibilityTest)

#include "tst_accessibilitytest.moc"
