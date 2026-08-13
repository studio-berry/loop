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

#include "pdfundoredomanager.h"

#include <QSignalSpy>
#include <QtTest>

class UndoRedoManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void namedStepsAreRetained();
    void stepLimitReportsTruncation();
};

void UndoRedoManagerTest::namedStepsAreRetained()
{
    pdfviewer::PDFUndoRedoManager manager(nullptr);
    manager.setMaximumSteps(4, 4);

    manager.createUndo(pdf::PDFModifiedDocument(), nullptr, QStringLiteral("Add Bleed"));
    QCOMPARE(manager.getUndoNames(), QStringList{ QStringLiteral("Add Bleed") });
    QCOMPARE(manager.getHistoryMemoryBytes(), size_t(0));
    QVERIFY(!manager.isHistoryTruncated());
}

void UndoRedoManagerTest::stepLimitReportsTruncation()
{
    pdfviewer::PDFUndoRedoManager manager(nullptr);
    manager.setMaximumSteps(1, 1);
    QSignalSpy truncationSpy(&manager, &pdfviewer::PDFUndoRedoManager::undoHistoryTruncated);

    manager.createUndo(pdf::PDFModifiedDocument(), nullptr, QStringLiteral("First edit"));
    manager.createUndo(pdf::PDFModifiedDocument(), nullptr, QStringLiteral("Second edit"));

    QCOMPARE(manager.getUndoNames(), QStringList{ QStringLiteral("Second edit") });
    QVERIFY(manager.isHistoryTruncated());
    QCOMPARE(truncationSpy.count(), 1);
}

QTEST_MAIN(UndoRedoManagerTest)

#include "tst_undoredomanagertest.moc"
