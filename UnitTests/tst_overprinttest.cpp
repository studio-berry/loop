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

#include "pdfpagecontentprocessor.h"
#include "pdftransparencyrenderer.h"

#include <QtTest>

class OverprintTest : public QObject
{
    Q_OBJECT

private slots:
    void overprintMode_appliesToContent_respectsFillStrokeFlags();
    void selectBlendOverprintMode_respectsFillStrokeGating();
    void floatBitmapBlend_mode0_selectsBackdropForInactiveChannels();
    void floatBitmapBlend_mode1_selectsNonOneSourceOrBackdropForSubtractiveChannels();
    void floatBitmapBlend_mode1_selectsNonZeroSourceOrBackdropForAdditiveChannels();
};

void OverprintTest::overprintMode_appliesToContent_respectsFillStrokeFlags()
{
    pdf::PDFOverprintMode mode;
    mode.overprintFilling = true;
    mode.overprintStroking = false;

    QVERIFY(mode.appliesToContent(true, false));
    QVERIFY(!mode.appliesToContent(false, true));
    QVERIFY(!mode.appliesToContent(false, false));

    mode.overprintFilling = false;
    mode.overprintStroking = true;

    QVERIFY(!mode.appliesToContent(true, false));
    QVERIFY(mode.appliesToContent(false, true));

    mode.overprintFilling = true;
    mode.overprintStroking = true;

    QVERIFY(mode.appliesToContent(true, false));
    QVERIFY(mode.appliesToContent(false, true));
    QVERIFY(!mode.appliesToContent(false, false));
}

void OverprintTest::selectBlendOverprintMode_respectsFillStrokeGating()
{
    pdf::PDFOverprintMode mode;
    mode.overprintFilling = true;
    mode.overprintStroking = false;
    mode.overprintMode = 0;

    QCOMPARE(pdf::selectBlendOverprintMode(mode, true, false),
             pdf::PDFFloatBitmap::OverprintMode::Overprint_Mode_0);
    QCOMPARE(pdf::selectBlendOverprintMode(mode, false, true),
             pdf::PDFFloatBitmap::OverprintMode::NoOveprint);
    QCOMPARE(pdf::selectBlendOverprintMode(mode, false, false),
             pdf::PDFFloatBitmap::OverprintMode::NoOveprint);

    mode.overprintFilling = false;
    mode.overprintStroking = true;

    QCOMPARE(pdf::selectBlendOverprintMode(mode, false, true),
             pdf::PDFFloatBitmap::OverprintMode::Overprint_Mode_0);
    QCOMPARE(pdf::selectBlendOverprintMode(mode, true, false),
             pdf::PDFFloatBitmap::OverprintMode::NoOveprint);

    mode.overprintMode = 1;
    QCOMPARE(pdf::selectBlendOverprintMode(mode, false, true),
             pdf::PDFFloatBitmap::OverprintMode::Overprint_Mode_1);
}

void OverprintTest::floatBitmapBlend_mode0_selectsBackdropForInactiveChannels()
{
    const pdf::PDFPixelFormat format = pdf::PDFPixelFormat::createFormat(4, 0, true, true, true);
    pdf::PDFFloatBitmap source(1, 1, format);
    pdf::PDFFloatBitmap target(1, 1, format);
    pdf::PDFFloatBitmap backdrop(1, 1, format);
    pdf::PDFFloatBitmap initialBackdrop(1, 1, format);
    const pdf::PDFFloatBitmap softMask = pdf::PDFFloatBitmap::createOpaqueSoftMask(1, 1);

    const pdf::PDFColor sourceColor{ 0.2f, 0.3f, 0.4f, 0.5f };
    const pdf::PDFColor backdropColor{ 0.7f, 0.8f, 0.9f, 1.0f };
    for (pdf::PDFFloatBitmap* bitmap : { &source, &target, &backdrop, &initialBackdrop })
    {
        pdf::PDFColorBuffer pixel = bitmap->getPixel(0, 0);
        for (size_t i = 0; i < backdropColor.size(); ++i)
        {
            pixel[i] = backdropColor[i];
        }
        pixel[4] = 1.0f;
        pixel[5] = 1.0f;
    }
    for (size_t i = 0; i < sourceColor.size(); ++i)
    {
        source.getPixel(0, 0)[i] = sourceColor[i];
    }
    source.getPixel(0, 0)[4] = 1.0f;
    source.getPixel(0, 0)[5] = 1.0f;
    source.setPixelActiveColorMask(0, 0, 1u << 0);

    pdf::PDFFloatBitmap::blend(source, target, backdrop, initialBackdrop, softMask, false, 1.0f,
                               pdf::BlendMode::Normal, false,
                               pdf::PDFFloatBitmap::OverprintMode::Overprint_Mode_0,
                               QRect(0, 0, 1, 1));

    const pdf::PDFFloatBitmap& constTarget = target;
    const pdf::PDFConstColorBuffer result = constTarget.getPixel(0, 0);
    QCOMPARE(result[0], sourceColor[0]);
    QCOMPARE(result[1], backdropColor[1]);
    QCOMPARE(result[2], backdropColor[2]);
    QCOMPARE(result[3], backdropColor[3]);
}

void OverprintTest::floatBitmapBlend_mode1_selectsNonOneSourceOrBackdropForSubtractiveChannels()
{
    const pdf::PDFPixelFormat format = pdf::PDFPixelFormat::createFormat(4, 0, true, true, true);
    pdf::PDFFloatBitmap source(1, 1, format);
    pdf::PDFFloatBitmap target(1, 1, format);
    pdf::PDFFloatBitmap backdrop(1, 1, format);
    pdf::PDFFloatBitmap initialBackdrop(1, 1, format);
    const pdf::PDFFloatBitmap softMask = pdf::PDFFloatBitmap::createOpaqueSoftMask(1, 1);
    const pdf::PDFColor sourceColor{ 0.2f, 0.3f, 0.4f, 0.0f };
    const pdf::PDFColor backdropColor{ 0.7f, 0.8f, 0.9f, 0.6f };

    for (pdf::PDFFloatBitmap* bitmap : { &source, &target, &backdrop, &initialBackdrop })
    {
        pdf::PDFColorBuffer pixel = bitmap->getPixel(0, 0);
        for (size_t i = 0; i < backdropColor.size(); ++i)
        {
            pixel[i] = backdropColor[i];
        }
        pixel[4] = 1.0f;
        pixel[5] = 1.0f;
    }
    for (size_t i = 0; i < sourceColor.size(); ++i)
    {
        source.getPixel(0, 0)[i] = sourceColor[i];
    }
    source.getPixel(0, 0)[4] = 1.0f;
    source.getPixel(0, 0)[5] = 1.0f;
    source.setPixelActiveColorMask(0, 0, pdf::PDFPixelFormat::getAllColorsMask());

    pdf::PDFFloatBitmap::blend(source, target, backdrop, initialBackdrop, softMask, false, 1.0f,
                               pdf::BlendMode::Normal, false,
                               pdf::PDFFloatBitmap::OverprintMode::Overprint_Mode_1,
                               QRect(0, 0, 1, 1));

    const pdf::PDFFloatBitmap& constTarget = target;
    const pdf::PDFConstColorBuffer result = constTarget.getPixel(0, 0);
    QCOMPARE(result[0], sourceColor[0]);
    QCOMPARE(result[1], sourceColor[1]);
    QCOMPARE(result[2], sourceColor[2]);
    QCOMPARE(result[3], backdropColor[3]);
}

void OverprintTest::floatBitmapBlend_mode1_selectsNonZeroSourceOrBackdropForAdditiveChannels()
{
    const pdf::PDFPixelFormat format = pdf::PDFPixelFormat::createFormat(3, 0, true, false, true);
    pdf::PDFFloatBitmap source(1, 1, format);
    pdf::PDFFloatBitmap target(1, 1, format);
    pdf::PDFFloatBitmap backdrop(1, 1, format);
    pdf::PDFFloatBitmap initialBackdrop(1, 1, format);
    const pdf::PDFFloatBitmap softMask = pdf::PDFFloatBitmap::createOpaqueSoftMask(1, 1);
    const pdf::PDFColor sourceColor{ 0.0f, 0.3f, 0.4f };
    const pdf::PDFColor backdropColor{ 0.7f, 0.8f, 0.9f };

    for (pdf::PDFFloatBitmap* bitmap : { &source, &target, &backdrop, &initialBackdrop })
    {
        pdf::PDFColorBuffer pixel = bitmap->getPixel(0, 0);
        for (size_t i = 0; i < backdropColor.size(); ++i)
        {
            pixel[i] = backdropColor[i];
        }
        pixel[3] = 1.0f;
        pixel[4] = 1.0f;
    }
    for (size_t i = 0; i < sourceColor.size(); ++i)
    {
        source.getPixel(0, 0)[i] = sourceColor[i];
    }
    source.getPixel(0, 0)[3] = 1.0f;
    source.getPixel(0, 0)[4] = 1.0f;
    source.setPixelActiveColorMask(0, 0, pdf::PDFPixelFormat::getAllColorsMask());

    pdf::PDFFloatBitmap::blend(source, target, backdrop, initialBackdrop, softMask, false, 1.0f,
                               pdf::BlendMode::Normal, false,
                               pdf::PDFFloatBitmap::OverprintMode::Overprint_Mode_1,
                               QRect(0, 0, 1, 1));

    const pdf::PDFFloatBitmap& constTarget = target;
    const pdf::PDFConstColorBuffer result = constTarget.getPixel(0, 0);
    QCOMPARE(result[0], backdropColor[0]);
    QCOMPARE(result[1], sourceColor[1]);
    QCOMPARE(result[2], sourceColor[2]);
}

QTEST_APPLESS_MAIN(OverprintTest)
#include "tst_overprinttest.moc"
