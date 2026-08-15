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

// Render goldens are refreshed with LOUPE_UPDATE_SNAPSHOTS=1. The comparison is
// intentionally tolerant of small Qt/platform rasterization differences while
// still requiring a bounded number of differing pixels. Mismatch images are
// written beside the committed baselines for CI artifact inspection.

#include "pdfcms.h"
#include "pdfdocument.h"
#include "pdfdocumentreader.h"
#include "pdfoptionalcontent.h"
#include "pdfrenderer.h"
#include "pdftransparencyrenderer.h"

#include <QtTest>

#include <limits>

namespace
{

struct RenderCase
{
    const char* name;
    bool separationSimulation;
};

const RenderCase RENDER_CASES[] = {
    { "overprint-cmyk-mode0", false },
    { "overprint-cmyk-mode1", false },
    { "overprint-white", false },
    { "overprint-separation", true },
    { "overprint-multiply", false },
    { "overprint-group-stroke", false },
};

QString fixturesDirectory()
{
    return QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures");
}

QString rendersDirectory()
{
    return QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/renders");
}

bool updateSnapshotsRequested()
{
    return qEnvironmentVariableIntValue("LOUPE_UPDATE_SNAPSHOTS") == 1;
}

QImage renderFixture(const QString& fixturePath, bool separationSimulation)
{
    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        return QImage();
    }

    const pdf::PDFPage* page = document.getCatalog()->getPage(0);
    if (!page)
    {
        return QImage();
    }

    pdf::PDFCMSGeneric cms;
    pdf::PDFOptionalContentActivity activity(&document, pdf::OCUsage::View, nullptr);
    pdf::PDFModifiedDocument modifiedDocument(&document, &activity);
    pdf::PDFFontCache fontCache(32, 32);
    fontCache.setDocument(modifiedDocument);

    pdf::PDFInkMapper inkMapper(nullptr, &document);
    inkMapper.createSpotColors(true);

    pdf::PDFTransparencyRendererSettings settings;
    settings.tileSize = QSize(32, 32);
    settings.multithreadingPathSampleThreshold = std::numeric_limits<int>::max();
    settings.flags.setFlag(pdf::PDFTransparencyRendererSettings::SeparationSimulation, separationSimulation);
    const QSize imageSize(128, 128);
    const QTransform pagePointToDevicePoint = pdf::PDFRenderer::createPagePointToDevicePointMatrix(page, QRect(QPoint(0, 0), imageSize));
    pdf::PDFTransparencyRenderer renderer(page, &document, &fontCache, &cms, &activity, &inkMapper, settings, pagePointToDevicePoint);

    renderer.beginPaint(imageSize);
    const auto errors = renderer.processContents();
    if (!errors.isEmpty())
    {
        return QImage();
    }
    renderer.endPaint();
    return renderer.toImage(false, true, pdf::PDFRGB{ 1.0f, 1.0f, 1.0f });
}

void compareRender(const QString& name, const QImage& actual, const QImage& expected)
{
    QVERIFY2(!actual.isNull(), qPrintable(QStringLiteral("Renderer returned no image for %1").arg(name)));
    if (updateSnapshotsRequested())
    {
        QVERIFY2(actual.save(rendersDirectory() + QLatin1Char('/') + name),
                 qPrintable(QStringLiteral("Could not write baseline for %1").arg(name)));
        return;
    }

    QVERIFY2(!expected.isNull(), qPrintable(QStringLiteral("Missing baseline for %1").arg(name)));
    QCOMPARE(actual.size(), expected.size());

    const QImage actualRgba = actual.convertToFormat(QImage::Format_RGBA8888);
    const QImage expectedRgba = expected.convertToFormat(QImage::Format_RGBA8888);
    constexpr int maxChannelDelta = 2;
    constexpr int differingPixelBudget = 64;
    int differingPixels = 0;
    int observedMaxDelta = 0;

    for (int y = 0; y < actualRgba.height(); ++y)
    {
        const uchar* actualLine = actualRgba.constScanLine(y);
        const uchar* expectedLine = expectedRgba.constScanLine(y);
        for (int x = 0; x < actualRgba.width(); ++x)
        {
            const int offset = x * 4;
            int pixelMaxDelta = 0;
            for (int channel = 0; channel < 4; ++channel)
            {
                pixelMaxDelta = qMax(pixelMaxDelta, qAbs(int(actualLine[offset + channel]) - int(expectedLine[offset + channel])));
            }
            observedMaxDelta = qMax(observedMaxDelta, pixelMaxDelta);
            if (pixelMaxDelta > maxChannelDelta)
            {
                ++differingPixels;
            }
        }
    }

    if (differingPixels > differingPixelBudget)
    {
        const QString actualPath = rendersDirectory() + QLatin1Char('/') + name + QStringLiteral("-actual.png");
        const QString expectedPath = rendersDirectory() + QLatin1Char('/') + name + QStringLiteral("-expected.png");
        actual.save(actualPath);
        expected.save(expectedPath);
        QFAIL(qPrintable(QStringLiteral("%1 differs: %2 pixels, max channel delta %3; actual=%4 expected=%5")
                             .arg(name)
                             .arg(differingPixels)
                             .arg(observedMaxDelta)
                             .arg(actualPath)
                             .arg(expectedPath)));
    }
}

} // namespace

class OverprintRenderTest : public QObject
{
    Q_OBJECT

private slots:
    void render_data();
    void render();
    void rendererDifferentialDoesNotDriftBeyondTolerance();
};

void OverprintRenderTest::render_data()
{
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<QString>("baseline");
    QTest::addColumn<bool>("separationSimulation");

    for (const RenderCase& renderCase : RENDER_CASES)
    {
        for (const char* state : { "off", "on" })
        {
            const QString name = QString::fromLatin1(renderCase.name) + QLatin1Char('-') + QString::fromLatin1(state);
            QTest::newRow(qPrintable(name)) << (name + QStringLiteral(".pdf")) << (name + QStringLiteral(".png")) << renderCase.separationSimulation;
        }
    }
}

void OverprintRenderTest::render()
{
    QFETCH(QString, fixture);
    QFETCH(QString, baseline);
    QFETCH(bool, separationSimulation);

    const QImage actual = renderFixture(fixturesDirectory() + QLatin1Char('/') + fixture, separationSimulation);
    const QImage expected = QImage(rendersDirectory() + QLatin1Char('/') + baseline);
    compareRender(baseline, actual, expected);
}

void OverprintRenderTest::rendererDifferentialDoesNotDriftBeyondTolerance()
{
    const QString name = QStringLiteral("overprint-cmyk-mode0-off");
    const QImage actual = renderFixture(fixturesDirectory() + QLatin1Char('/') + name + QStringLiteral(".pdf"), false);
    const QImage expected = QImage(rendersDirectory() + QLatin1Char('/') + name + QStringLiteral(".png"));
    compareRender(name + QStringLiteral(".png"), actual, expected);
}

QTEST_APPLESS_MAIN(OverprintRenderTest)
#include "tst_overprintrendertest.moc"
