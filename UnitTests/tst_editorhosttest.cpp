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

#include <QtTest>

#include <QSignalSpy>
#include <QThread>

#include <atomic>
#include <memory>

#include "documentviewsession.h"
#include "editorhost.h"

#include "pdfdocumentbuilder.h"
#include "pdfdocumentwriter.h"

#include <QTemporaryDir>

class EditorHostTest : public QObject
{
    Q_OBJECT

private slots:
    void startsWithNoDocument();
    void exposesCatalogDescriptorsWithoutMutating();
    void navigationCommandsStayDisabledUntilOpen();
    void sessionTeardownDrainsWorkersBeforeAdapters();
    void openLargeDocument();
};

void EditorHostTest::startsWithNoDocument()
{
    EditorHost host;
    QCOMPARE(host.documentState(), QStringLiteral("empty"));
    QVERIFY(!host.hasDocument());
    QCOMPARE(host.pageCount(), 0);
}

void EditorHostTest::exposesCatalogDescriptorsWithoutMutating()
{
    EditorHost host;
    const QVariantList descriptors = host.commandDescriptors();
    QVERIFY(descriptors.size() > 100);

    bool sawOpen = false;
    for (const QVariant& entryVariant : descriptors)
    {
        const QVariantMap entry = entryVariant.toMap();
        if (entry.value(QStringLiteral("id")).toString() == QStringLiteral("actionOpen"))
        {
            sawOpen = true;
            QVERIFY(entry.value(QStringLiteral("implemented")).toBool());
            QVERIFY(!host.shortcutForCommand(QStringLiteral("actionOpen")).isEmpty() || entry.contains(QStringLiteral("shortcut")));
        }
    }
    QVERIFY(sawOpen);
}

void EditorHostTest::navigationCommandsStayDisabledUntilOpen()
{
    EditorHost host;
    QVERIFY(!host.isCommandEnabled(QStringLiteral("actionGoToNextPage")));
    QCOMPARE(host.invokeCommand(QStringLiteral("actionGoToNextPage")), quint64(0));
}

void EditorHostTest::sessionTeardownDrainsWorkersBeforeAdapters()
{
    auto session = std::make_unique<DocumentViewSession>();
    DocumentViewSession* rawSession = session.get();
    std::atomic_bool started = false;
    std::atomic_bool adapterReached = false;

    pdf::PDFJobSpec spec;
    spec.kind = pdf::PDFJobKind::Other;
    spec.priority = pdf::PDFJobPriority::Background;
    rawSession->scheduler().submit(spec,
                                   [rawSession, &started, &adapterReached](pdf::PDFJobContext& context)
                                   {
                                       started.store(true, std::memory_order_release);
                                       while (!context.isCancellationRequested())
                                       {
                                           QThread::yieldCurrentThread();
                                       }

                                       // The session destructor must join this
                                       // work before destroying the renderer.
                                       rawSession->renderer().shedPrefetchAndQuality();
                                       adapterReached.store(true, std::memory_order_release);
                                   });

    QTRY_VERIFY_WITH_TIMEOUT(started.load(std::memory_order_acquire), 1000);
    session.reset();
    QVERIFY(adapterReached.load(std::memory_order_acquire));
}

void EditorHostTest::openLargeDocument()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    pdf::PDFDocumentBuilder builder;
    constexpr int pageCount = 256;
    for (int page = 0; page < pageCount; ++page)
    {
        builder.appendPage(QRectF(0, 0, 612, 792));
    }

    const QString path = directory.filePath(QStringLiteral("large.pdf"));
    {
        const pdf::PDFDocument document = builder.build();
        pdf::PDFDocumentWriter writer(nullptr);
        QVERIFY(writer.write(path, &document, true));
    }

    EditorHost host;
    host.openFileUrl(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(host.hasDocument(), 60000);
    QCOMPARE(host.pageCount(), pageCount);
    host.setViewportGeometry(96.0 / 25.4, 1.0, 1024, 768);
    QVERIFY(host.isCommandEnabled(QStringLiteral("actionGoToDocumentEnd")));
    QVERIFY(host.invokeCommand(QStringLiteral("actionGoToDocumentEnd")) != 0);
    QTRY_COMPARE_WITH_TIMEOUT(host.currentPage(), pageCount - 1, 10000);
}

QTEST_GUILESS_MAIN(EditorHostTest)

#include "tst_editorhosttest.moc"
