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

#include "pdfdocumentbuilder.h"
#include "pdfthumbnailsrenderer.h"

#include <QtTest>
#include <QSignalSpy>

class ThumbnailsRendererTest : public QObject
{
    Q_OBJECT

private slots:
    void nullDocument_returnsEmptyImage();
    void outOfRangePage_returnsEmptyImage();
    void getPageImage_rendersAsynchronously();
    void cachedImage_isReused();
    void invalidatePage_triggersReRender();
    void clear_dropsPendingAndCachedImages();
    void setDocument_waitsForRunningBatch();

private:
    /// Waits until a new page render is reported (or the timeout expires).
    /// Returns true, when a new render finished meanwhile.
    static bool waitForRendered(QSignalSpy& spy, int timeoutMs)
    {
        const int baseline = spy.count();
        while (spy.count() == baseline)
        {
            if (!spy.wait(timeoutMs))
            {
                return false;
            }
        }
        return true;
    }
};

void ThumbnailsRendererTest::nullDocument_returnsEmptyImage()
{
    pdf::PDFThumbnailsRenderer renderer(nullptr);
    QVERIFY(renderer.getPageImage(0, 100).isNull());
}

void ThumbnailsRendererTest::outOfRangePage_returnsEmptyImage()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFThumbnailsRenderer renderer(&document);
    QVERIFY(renderer.getPageImage(1, 100).isNull());
    QVERIFY(renderer.getPageImage(-1, 100).isNull());
}

void ThumbnailsRendererTest::getPageImage_rendersAsynchronously()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFThumbnailsRenderer renderer(&document);

    // The first call schedules the render and returns an empty image.
    QImage image = renderer.getPageImage(0, 100);
    QVERIFY(image.isNull());

    QSignalSpy spy(&renderer, &pdf::PDFThumbnailsRenderer::pageImageReady);
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not rendered in time");

    image = renderer.getPageImage(0, 100);
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 100);
    QCOMPARE(image.height(), 100);
}

void ThumbnailsRendererTest::cachedImage_isReused()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFThumbnailsRenderer renderer(&document);

    QSignalSpy spy(&renderer, &pdf::PDFThumbnailsRenderer::pageImageReady);
    renderer.getPageImage(0, 100);
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not rendered in time");

    // The second call must hit the cache - no signal should be emitted.
    const QImage first = renderer.getPageImage(0, 100);
    QVERIFY(!first.isNull());
    QCOMPARE(spy.count(), 1);

    const QImage second = renderer.getPageImage(0, 100);
    QVERIFY(!second.isNull());
    QCOMPARE(second.size(), first.size());
    QCOMPARE(spy.count(), 1);
}

void ThumbnailsRendererTest::invalidatePage_triggersReRender()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFThumbnailsRenderer renderer(&document);

    QSignalSpy spy(&renderer, &pdf::PDFThumbnailsRenderer::pageImageReady);
    renderer.getPageImage(0, 100);
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not rendered in time");

    renderer.invalidatePage(0);

    // The cache entry for the page has been dropped, so a new render is scheduled.
    QVERIFY(renderer.getPageImage(0, 100).isNull());
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not re-rendered in time");
    QVERIFY(!renderer.getPageImage(0, 100).isNull());
}

void ThumbnailsRendererTest::clear_dropsPendingAndCachedImages()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFThumbnailsRenderer renderer(&document);

    QSignalSpy spy(&renderer, &pdf::PDFThumbnailsRenderer::pageImageReady);
    renderer.getPageImage(0, 100);
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not rendered in time");

    renderer.clear();
    QVERIFY(renderer.getPageImage(0, 100).isNull());
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not re-rendered in time");
    QVERIFY(!renderer.getPageImage(0, 100).isNull());
}

void ThumbnailsRendererTest::setDocument_waitsForRunningBatch()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFThumbnailsRenderer renderer(&document);

    QSignalSpy spy(&renderer, &pdf::PDFThumbnailsRenderer::pageImageReady);
    renderer.getPageImage(0, 100);
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not rendered in time");

    // A rebind must not crash and must wait for the pending document batch.
    renderer.setDocument(&document);
    QCOMPARE(spy.count(), 1);
    QVERIFY(renderer.getPageImage(0, 100).isNull());
    QVERIFY2(waitForRendered(spy, 5000), "thumbnail was not re-rendered in time");
    QVERIFY(!renderer.getPageImage(0, 100).isNull());
}

QTEST_MAIN(ThumbnailsRendererTest)

#include "tst_thumbnailsrenderertest.moc"