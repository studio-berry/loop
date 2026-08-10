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

#include "preflightengine.h"
#include "pdfcolorinventory.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentsession.h"
#include "pdfimage.h"
#include "pdfinkcoverageprobe.h"

#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <vector>

class PreflightEngineTest : public QObject
{
    Q_OBJECT

private slots:
    void parseProfile_rejectsMissingName();
    void parseProfile_rejectsEmptyChecks();
    void parseProfile_rejectsInvalidDownsampleTargetDpi();
    void parseProfile_rejectsInkCoverageWithoutPositiveMax();
    void parseProfile_acceptsInkCoverageDefaults();
    void parseProfile_rejectsInvalidInkCoverageParameters();
    void parseProfile_acceptsInkCoverageAnalysisBox();
    void parseProfile_acceptsZeroInkCoverageRegionCap();
    void inkCoverageThreshold_isStrictlyGreater();
    void parseProfile_acceptsThinStrokeOverridesAndDefaults();
    void parseProfile_rejectsInvalidThinStrokeThreshold();
    void parseProfile_rejectsOutputIntentInvalidAllowedColorSpace();
    void parseProfile_acceptsPDFXTargetAndRevision();
    void parseProfile_rejectsUnknownPDFXTarget();
    void pdfxStatusReduction_prioritizesFailureAndIncomplete();
    void pdfxStatusReduction_marksMandatoryNotApplicableIncomplete();
    void run_pdfxWithoutDocumentIsIncompleteAndSerialized();
    void run_pdfxEmitsStableRuleIdsAndEvidence();
    void run_bleedCheckFailsWhenBoxMissing();
    void run_bleedCheckPassesWhenBoxAdequate();
    void run_unknownCheckIdIsIgnored();
    void run_thinStrokes_detectsPaintedThinStroke();
    void fontIntegrity_checkIsRegistered();
    void run_fontIntegrity_keepsValidEmbeddedFixtureClean();
    void hiddenContent_checksAreRegistered();
    void run_offPageContent_detectsMarksOutsideToleratedBox();
    void run_includesProfileFixups();
    void run_synthesizesAddBleedWhenGapAndNoProfileFixup();
    void run_removesAddBleedWhenNoGap();
    void run_doesNotAdvertiseUnimplementedFixups();
    void run_invalidProfileEmitsDocumentScopeFinding();
    void findingStableId_ignoresMessageAndBbox();
    void decisionRejectsMissingJustification();
    void decisionRoundTripAndStalenessAreDeterministic();
    void decisionReopenDoesNotCountForSignoff();
    void run_contentBleedWithoutRaster_emitsContentBleedAndNeedsAutoBleed();
    void run_contentBleedRasterConfirm_emitsBleedMarginEmptyAndNeedsAutoBleed();
    void run_whiteOverprint_emitsWarningForWhitePaintWithOverprint();
    void run_whiteOverprint_passesWhenOverprintOff();
    void run_whiteOverprint_emitsWarningInsideFormXObject();
    void run_inkCoverage_emitsRegionalWarningForOverLimitFixture();
    void run_inkCoverage_passesBelowLimitFixture();
    void run_inkCoverage_budgetAbortIsIncomplete();
    void inkCoverageProbe_usesAnalysisBoxAndReportsBudget();
    void run_downsampleFixupAdvertisedForHighDpiImage();
    void run_downsampleFixupHiddenWhenNoCandidateExists();
    void run_downsampleFixupCarriesTargetDpi();
    void run_colorRgbFixtureFailsColorMode();
    void colorInventory_isRegisteredAndInfoOnly();
    void colorInventory_rejectsInvalidParameters();
    void richBlackPredicate_distinguishesChromaticBlack();
    void run_outputIntent_missingIntentEmitsError();
    void run_outputIntent_notRequiredPassesWithoutIntent();
    void run_outputIntent_emptyIdentifierEmitsIdentityFinding();
    void run_outputIntent_identifierNotInAllowListEmitsIdentityFinding();
    void run_outputIntent_profileCsDisagreesWithEmbeddedProfile();
    void run_outputIntent_conflictingIntentsEmitConflictFinding();
    void run_outputIntent_strictMultipleIntentsEmitAmbiguityFinding();
    void run_outputIntent_optionalEmbeddedProfileCanBeAbsent();
    void run_outputIntent_malformedArrayEntryEmitsFinding();
    void run_outputIntent_severityWarningRoutesToWarnings();
};

namespace
{

struct OutputIntentSpec
{
    QByteArray identifier;
    QByteArray profileCS;
    QByteArray profileContent;
    bool includeProfile = true;
};

QByteArray loadOutputIntentProfile(const QString& fixturePath)
{
    if (!QFile::exists(fixturePath))
    {
        return QByteArray();
    }

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK ||
        document.getCatalog()->getOutputIntents().empty())
    {
        return QByteArray();
    }

    const pdf::PDFOutputIntent& intent = document.getCatalog()->getOutputIntents().front();
    const pdf::PDFObject profileObject = document.getObject(intent.getOutputProfile());
    if (!profileObject.isStream())
    {
        return QByteArray();
    }

    return *profileObject.getStream()->getContent();
}

pdf::PDFDocument buildOutputIntentDocument(const std::vector<OutputIntentSpec>& specs)
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));

    pdf::PDFArray outputIntents;
    for (const OutputIntentSpec& spec : specs)
    {
        pdf::PDFDictionary intentDictionary;
        intentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("OutputIntent"));
        intentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("S"), pdf::PDFObject::createName("GTS_PDFX"));
        intentDictionary.addEntry(
            pdf::PDFInplaceOrMemoryString("OutputConditionIdentifier"), pdf::PDFObject::createString(spec.identifier));

        if (spec.includeProfile)
        {
            pdf::PDFDictionary streamDictionary;
            streamDictionary.addEntry(
                pdf::PDFInplaceOrMemoryString("N"), pdf::PDFObject::createInteger(spec.profileCS == QByteArrayLiteral("RGB") ? 3 : 4));
            streamDictionary.addEntry(
                pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(spec.profileContent.size()));
            const pdf::PDFObjectReference profileReference = builder.addObject(
                pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
                    std::move(streamDictionary), QByteArray(spec.profileContent))));
            intentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("DestOutputProfile"), pdf::PDFObject::createReference(profileReference));

            pdf::PDFDictionary profileInfo;
            profileInfo.addEntry(pdf::PDFInplaceOrMemoryString("ProfileCS"), pdf::PDFObject::createString(spec.profileCS));
            intentDictionary.addEntry(
                pdf::PDFInplaceOrMemoryString("DestOutputProfileRef"),
                pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(profileInfo))));
        }

        const pdf::PDFObjectReference intentReference = builder.addObject(
            pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(intentDictionary))));
        outputIntents.appendItem(pdf::PDFObject::createReference(intentReference));
    }

    if (!specs.empty())
    {
        pdf::PDFDictionary catalog;
        catalog.addEntry(
            pdf::PDFInplaceOrMemoryString("OutputIntents"),
            pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(outputIntents))));
        builder.mergeTo(
            builder.getCatalogReference(),
            pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(catalog))));
    }

    return builder.build();
}

pdf::PDFDocument buildTieredBleedGapPage()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 220, 220));
    builder.setPageTrimBox(page, QRectF(10, 10, 200, 200));

    pdf::PDFPageContentStreamBuilder pageContentStreamBuilder(&builder,
                                                              pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = pageContentStreamBuilder.begin(page))
    {
        painter->fillRect(QRectF(10, 10, 200, 200), Qt::black);
        pageContentStreamBuilder.end(painter);
    }

    return builder.build();
}

QJsonObject tieredBleedProfile(bool rasterConfirm)
{
    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Tiered bleed test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("content-bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("warning") },
        { QStringLiteral("raster_confirm"), rasterConfirm }
    });
    profile.insert(QStringLiteral("checks"), checks);
    return profile;
}

pdf::PDFDocument buildHighDpiImagePage(int sourcePixels = 1200)
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 144, 144));

    QImage image(sourcePixels, sourcePixels, QImage::Format_ARGB32);
    for (int y = 0; y < sourcePixels; ++y)
    {
        for (int x = 0; x < sourcePixels; ++x)
        {
            image.setPixel(x, y, qRgb((x * 17 + y * 13) % 256,
                                      (x * 7 + y * 29) % 256,
                                      (x * 31 + y * 3) % 256));
        }
    }

    pdf::PDFImage::ImageEncodeOptions imageOptions;
    imageOptions.compression = pdf::PDFImage::ImageCompression::Flate;
    imageOptions.colorMode = pdf::PDFImage::ImageColorMode::Preserve;
    const pdf::PDFObjectReference imageReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFImage::createStreamFromImage(image, imageOptions))));

    QByteArray content("q 144 0 0 144 0 0 cm /Im1 Do Q");
    pdf::PDFDictionary contentDictionary;
    contentDictionary.addEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH),
                                pdf::PDFObject::createInteger(content.size()));
    const pdf::PDFObjectReference contentReference = builder.addObject(
        pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
            pdf::PDFStream(std::move(contentDictionary), std::move(content)))));

    pdf::PDFDictionary xObject;
    xObject.addEntry(pdf::PDFInplaceOrMemoryString("Im1"), pdf::PDFObject::createReference(imageReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObject))));
    pdf::PDFDictionary pageUpdate;
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.addEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(pageReference,
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    return builder.build();
}

} // namespace

void PreflightEngineTest::parseProfile_rejectsMissingName()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;

    QVERIFY(!engine.parseProfile(QJsonObject(), profile, errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void PreflightEngineTest::parseProfile_rejectsEmptyChecks()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;

    QJsonObject profileObject;
    profileObject.insert(QStringLiteral("name"), QStringLiteral("Test"));
    profileObject.insert(QStringLiteral("checks"), QJsonArray());

    QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void PreflightEngineTest::parseProfile_rejectsInvalidDownsampleTargetDpi()
{
    pdf::PreflightEngine engine(nullptr);
    for (const QJsonValue& targetDpi : { QJsonValue(0), QJsonValue(71), QJsonValue(1201), QJsonValue(300.5), QJsonValue(QStringLiteral("300")) })
    {
        pdf::PreflightProfileData profile;
        QString errorMessage;
        const QJsonObject profileObject{
            { QStringLiteral("name"), QStringLiteral("Downsample") },
            { QStringLiteral("checks"), QJsonArray{
                QJsonObject{ { QStringLiteral("id"), QStringLiteral("bleed") } }
            } },
            { QStringLiteral("fixups"), QJsonArray{
                QJsonObject{
                    { QStringLiteral("id"), QStringLiteral("downsample-images") },
                    { QStringLiteral("target_dpi"), targetDpi }
                }
            } }
        };

        QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
        QVERIFY(errorMessage.contains(QStringLiteral("target_dpi")));
    }
}

void PreflightEngineTest::parseProfile_rejectsInkCoverageWithoutPositiveMax()
{
    pdf::PreflightEngine engine(nullptr);

    for (const QJsonValue& maxValue : { QJsonValue(), QJsonValue(0), QJsonValue(-1) })
    {
        QJsonObject profileObject;
        profileObject.insert(QStringLiteral("name"), QStringLiteral("Ink coverage"));
        profileObject.insert(QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("ink-coverage") },
                { QStringLiteral("max_ink_pct"), maxValue }
            }
        });

        pdf::PreflightProfileData profile;
        QString errorMessage;
        QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
        QCOMPARE(errorMessage, QStringLiteral("Check 'ink-coverage' requires positive max_ink_pct."));
    }
}

void PreflightEngineTest::parseProfile_acceptsInkCoverageDefaults()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    QJsonObject profileObject;
    profileObject.insert(QStringLiteral("name"), QStringLiteral("Ink coverage"));
    profileObject.insert(QStringLiteral("checks"), QJsonArray{
        QJsonObject{
            { QStringLiteral("id"), QStringLiteral("ink-coverage") },
            { QStringLiteral("max_ink_pct"), 300 }
        }
    });

    QVERIFY(engine.parseProfile(profileObject, profile, errorMessage));
    QCOMPARE(profile.checks.size(), 1);
    QCOMPARE(profile.checks.first().maxInkPct, 300.0);
    QCOMPARE(profile.checks.first().probeDpi, 150);
    QCOMPARE(profile.checks.first().minRegionAreaPct, 0.05);
    QCOMPARE(profile.checks.first().maxRegionsPerPage, 20);
    QCOMPARE(profile.checks.first().maxRasterPixels, qint64(250LL * 1000 * 1000));
    QCOMPARE(profile.checks.first().inkCoverageAnalysisBox, QStringLiteral("bleed"));
}

void PreflightEngineTest::parseProfile_rejectsInvalidInkCoverageParameters()
{
    const QList<QPair<QString, QJsonValue>> invalidValues{
        { QStringLiteral("max_ink_pct"), QJsonValue(QStringLiteral("300")) },
        { QStringLiteral("max_ink_pct"), QJsonValue(true) },
        { QStringLiteral("probe_dpi"), QJsonValue(0) },
        { QStringLiteral("probe_dpi"), QJsonValue(150.5) },
        { QStringLiteral("min_region_area_pct"), QJsonValue(-1) },
        { QStringLiteral("min_region_area_pct"), QJsonValue(101) },
        { QStringLiteral("min_region_area_pct"), QJsonValue(QStringLiteral("0.05")) },
        { QStringLiteral("max_regions_per_page"), QJsonValue(1.5) },
        { QStringLiteral("max_regions_per_page"), QJsonValue(QStringLiteral("20")) },
        { QStringLiteral("max_raster_pixels"), QJsonValue(0) },
        { QStringLiteral("max_raster_pixels"), QJsonValue(1.5) },
        { QStringLiteral("max_raster_pixels"), QJsonValue(QStringLiteral("250000000")) },
        { QStringLiteral("analysis_box"), QJsonValue(QStringLiteral("art")) },
        { QStringLiteral("analysis_box"), QJsonValue(42) }
    };

    for (const auto& invalid : invalidValues)
    {
        pdf::PreflightEngine engine(nullptr);
        pdf::PreflightProfileData profile;
        QString errorMessage;
        const QJsonObject check{
            { QStringLiteral("id"), QStringLiteral("ink-coverage") },
            { QStringLiteral("max_ink_pct"), 300 },
            { invalid.first, invalid.second }
        };
        const QJsonObject profileObject{
            { QStringLiteral("name"), QStringLiteral("Ink coverage") },
            { QStringLiteral("checks"), QJsonArray{ check } }
        };

        QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
        QVERIFY2(!errorMessage.isEmpty(), qPrintable(invalid.first));
    }
}

void PreflightEngineTest::parseProfile_acceptsZeroInkCoverageRegionCap()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Ink coverage without region findings") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("ink-coverage") },
                { QStringLiteral("max_ink_pct"), 300 },
                { QStringLiteral("max_regions_per_page"), 0 },
                { QStringLiteral("max_raster_pixels"), 1000000 }
            }
        } }
    };

    QVERIFY(engine.parseProfile(profileObject, profile, errorMessage));
    QCOMPARE(profile.checks.first().maxRegionsPerPage, 0);
    QCOMPARE(profile.checks.first().maxRasterPixels, qint64(1000000));
}

void PreflightEngineTest::inkCoverageThreshold_isStrictlyGreater()
{
    QVERIFY(!pdf::inkCoverageExceedsLimit(3.0, 3.0));
    QVERIFY(!pdf::inkCoverageExceedsLimit(2.999, 3.0));
    QVERIFY(pdf::inkCoverageExceedsLimit(3.001, 3.0));
}

void PreflightEngineTest::parseProfile_acceptsInkCoverageAnalysisBox()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Ink coverage") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("ink-coverage") },
                { QStringLiteral("max_ink_pct"), 300 },
                { QStringLiteral("analysis_box"), QStringLiteral("media") }
            }
        } }
    };

    QVERIFY(engine.parseProfile(profileObject, profile, errorMessage));
    QCOMPARE(profile.checks.first().inkCoverageAnalysisBox, QStringLiteral("media"));
}

void PreflightEngineTest::parseProfile_acceptsThinStrokeOverridesAndDefaults()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Thin strokes") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("thin-strokes") },
                { QStringLiteral("severity"), QStringLiteral("warning") },
                { QStringLiteral("min_effective_width_pt"), 0.25 },
                { QStringLiteral("zero_width_epsilon_pt"), 0.0001 },
                { QStringLiteral("hairline_severity"), QStringLiteral("error") }
            }
        } }
    };

    QVERIFY(engine.parseProfile(profileObject, profile, errorMessage));
    QCOMPARE(profile.checks.first().minEffectiveStrokeWidthPt, 0.25);
    QCOMPARE(profile.checks.first().zeroWidthEpsilonPt, 0.0001);
    QCOMPARE(profile.checks.first().hairlineSeverity, QStringLiteral("error"));
    QCOMPARE(profile.checks.first().thinStrokeSeverity, QStringLiteral("warning"));
}

void PreflightEngineTest::parseProfile_rejectsInvalidThinStrokeThreshold()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Thin strokes") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("thin-strokes") },
                { QStringLiteral("min_effective_width_pt"), 0.0 }
            }
        } }
    };

    QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
    QCOMPARE(errorMessage, QStringLiteral("Check 'thin-strokes' requires positive min_effective_width_pt."));
}

void PreflightEngineTest::run_thinStrokes_detectsPaintedThinStroke()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFPageContentStreamBuilder contentBuilder(&builder,
                                                    pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    QPainter* painter = contentBuilder.begin(page);
    QVERIFY(painter != nullptr);

    QPen thinPen(Qt::black);
    thinPen.setWidthF(0.1);
    painter->setPen(thinPen);
    painter->drawLine(QPointF(20, 20), QPointF(180, 20));
    contentBuilder.end(painter);

    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Thin strokes") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("thin-strokes") },
                { QStringLiteral("min_effective_width_pt"), 0.25 },
                { QStringLiteral("thin_stroke_severity"), QStringLiteral("warning") }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().scope, QStringLiteral("object"));
    QCOMPARE(result.warnings.first().type, QStringLiteral("thin-stroke"));
    QCOMPARE(result.warnings.first().checkId, QStringLiteral("thin-strokes"));
    QVERIFY(result.warnings.first().bbox.isValid());
}

void PreflightEngineTest::fontIntegrity_checkIsRegistered()
{
    pdf::PreflightEngine engine(nullptr);
    QVERIFY(engine.hasCheck(QStringLiteral("font-integrity")));
}

void PreflightEngineTest::run_fontIntegrity_keepsValidEmbeddedFixtureClean()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/font-embedded.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Font integrity") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("font-integrity") } }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QVERIFY(result.errors.isEmpty());
    QVERIFY(result.warnings.isEmpty());
}

void PreflightEngineTest::hiddenContent_checksAreRegistered()
{
    pdf::PreflightEngine engine(nullptr);
    QVERIFY(engine.hasCheck(QStringLiteral("invisible-content")));
    QVERIFY(engine.hasCheck(QStringLiteral("hidden-layers")));
    QVERIFY(engine.hasCheck(QStringLiteral("off-page-content")));
    QVERIFY(engine.hasCheck(QStringLiteral("obscured-content")));
}

void PreflightEngineTest::run_offPageContent_detectsMarksOutsideToleratedBox()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFPageContentStreamBuilder contentBuilder(&builder,
                                                    pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    QPainter* painter = contentBuilder.begin(page);
    QVERIFY(painter != nullptr);
    painter->setPen(Qt::NoPen);
    painter->setBrush(Qt::black);
    painter->drawRect(QRectF(300, 300, 20, 20));
    contentBuilder.end(painter);

    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Hidden content") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("off-page-content") },
                { QStringLiteral("severity"), QStringLiteral("warning") },
                { QStringLiteral("amount_pt"), 0 }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().type, QStringLiteral("off-page-content"));
    QCOMPARE(result.warnings.first().checkId, QStringLiteral("off-page-content"));
    QVERIFY(result.warnings.first().bbox.isValid());
}

void PreflightEngineTest::parseProfile_rejectsOutputIntentInvalidAllowedColorSpace()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Output intent test") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("output-intent") },
                { QStringLiteral("allowed"), QJsonArray{ QStringLiteral("Lab") } }
            }
        } }
    };

    QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("unknown allowed color space")));
}

void PreflightEngineTest::parseProfile_acceptsPDFXTargetAndRevision()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("PDF/X-4") },
        { QStringLiteral("pdfx"), QJsonObject{
            { QStringLiteral("target"), QStringLiteral("PDF/X-4") },
            { QStringLiteral("policyVersion"), 1 }
        } },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("output-intent") } }
        } }
    };

    QVERIFY(engine.parseProfile(profileObject, profile, errorMessage));
    QVERIFY(profile.pdfx.has_value());
    QCOMPARE(profile.pdfx->flavor, pdf::PDFXFlavor::X4);
    QCOMPARE(profile.pdfx->policyVersion, QStringLiteral("1"));
}

void PreflightEngineTest::parseProfile_rejectsUnknownPDFXTarget()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    const QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Unknown PDF/X") },
        { QStringLiteral("pdfx"), QJsonObject{
            { QStringLiteral("target"), QStringLiteral("PDF/X-3:2002") }
        } },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("output-intent") } }
        } }
    };

    QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("Unsupported PDF/X target")));
}

void PreflightEngineTest::pdfxStatusReduction_prioritizesFailureAndIncomplete()
{
    const QVector<pdf::PDFXRuleResult> rules{
        { QStringLiteral("pdfx.first"), true, pdf::PDFXRuleState::NotInspected },
        { QStringLiteral("pdfx.second"), true, pdf::PDFXRuleState::Failed },
    };
    QStringList failed;
    QStringList incomplete;

    QCOMPARE(pdf::reducePDFXStatus(rules, &failed, &incomplete), pdf::PDFXConformanceStatus::NonConformant);
    QCOMPARE(failed, QStringList{ QStringLiteral("pdfx.second") });
    QCOMPARE(incomplete, QStringList{ QStringLiteral("pdfx.first") });
}

void PreflightEngineTest::pdfxStatusReduction_marksMandatoryNotApplicableIncomplete()
{
    const QVector<pdf::PDFXRuleResult> rules{
        { QStringLiteral("pdfx.future-rule"), true, pdf::PDFXRuleState::NotApplicable },
    };
    QStringList incomplete;

    QCOMPARE(pdf::reducePDFXStatus(rules, nullptr, &incomplete), pdf::PDFXConformanceStatus::Incomplete);
    QCOMPARE(incomplete, QStringList{ QStringLiteral("pdfx.future-rule") });
}

void PreflightEngineTest::run_pdfxWithoutDocumentIsIncompleteAndSerialized()
{
    pdf::PreflightEngine engine(nullptr);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("PDF/X-4") },
        { QStringLiteral("pdfx"), QJsonObject{
            { QStringLiteral("target"), QStringLiteral("PDF/X-4") }
        } },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("output-intent") } }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(result.pdfx.has_value());
    QCOMPARE(result.pdfx->status, pdf::PDFXConformanceStatus::Incomplete);
    QVERIFY(!result.pdfx->incompleteRuleIds.isEmpty());
    QVERIFY(result.toJson().contains(QStringLiteral("pdfx")));
    QVERIFY(!result.toJson().value(QStringLiteral("pdfx")).toObject()
                .value(QStringLiteral("incompleteRuleIds")).toArray().isEmpty());
}

void PreflightEngineTest::run_pdfxEmitsStableRuleIdsAndEvidence()
{
    const QByteArray profileContent = loadOutputIntentProfile(
        QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/output-intent-cmyk.pdf"));
    QVERIFY(!profileContent.isEmpty());

    pdf::PDFDocument document = buildOutputIntentDocument({
        { QByteArrayLiteral("CGATS TR 001"), QByteArrayLiteral("CMYK"), profileContent }
    });
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("PDF/X-4") },
        { QStringLiteral("pdfx"), QJsonObject{
            { QStringLiteral("target"), QStringLiteral("PDF/X-4") }
        } },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("output-intent") } }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pdfx.has_value());
    QCOMPARE(result.pdfx->status, pdf::PDFXConformanceStatus::NonConformant);
    QVERIFY(result.pdfx->failedRuleIds.contains(QStringLiteral("pdfx.metadata.identification")));
    QVERIFY(std::any_of(result.errors.cbegin(), result.errors.cend(), [](const pdf::PreflightFinding& finding)
    {
        return finding.checkId == QStringLiteral("pdfx.metadata.identification")
            && finding.evidence.value(QStringLiteral("rule_id")).toString() == QStringLiteral("pdfx.metadata.identification");
    }));

    const QJsonObject report = result.toJson();
    const QJsonObject pdfx = report.value(QStringLiteral("pdfx")).toObject();
    QCOMPARE(pdfx.value(QStringLiteral("target")).toString(), QStringLiteral("PDF/X-4"));
    QCOMPARE(pdfx.value(QStringLiteral("policyVersion")).toString(), QStringLiteral("1"));
    QCOMPARE(pdfx.value(QStringLiteral("status")).toString(), QStringLiteral("non-conformant"));
}

void PreflightEngineTest::run_bleedCheckFailsWhenBoxMissing()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().type, QStringLiteral("bleed"));
}

void PreflightEngineTest::run_bleedCheckPassesWhenBoxAdequate()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 220, 220));
    builder.setPageTrimBox(page, QRectF(10, 10, 200, 200));
    builder.setPageBleedBox(page, QRectF(0, 0, 220, 220));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QVERIFY(result.errors.isEmpty());
}

void PreflightEngineTest::run_unknownCheckIdIsIgnored()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("not-a-real-check") },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(!result.inspectionComplete);
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().type, QStringLiteral("profile"));
    QCOMPARE(result.checkStatuses.size(), 1);
    QCOMPARE(result.checkStatuses.first().status, QStringLiteral("unsupported"));
}

void PreflightEngineTest::run_includesProfileFixups()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);
    QJsonArray fixups;
    fixups.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("add-bleed") },
        { QStringLiteral("amount_pt"), 9 }
    });
    profile.insert(QStringLiteral("fixups"), fixups);

    pdf::PreflightResult result = engine.run(profile);
    QCOMPARE(result.fixupsAvailable.size(), 1);
    QCOMPARE(result.fixupsAvailable.first().id, QStringLiteral("add-bleed"));
    QCOMPARE(result.fixupsAvailable.first().amountPt, 9.0);

    const QJsonObject report = result.toJson();
    const QJsonArray reportFixups = report.value(QStringLiteral("fixups_available")).toArray();
    QCOMPARE(reportFixups.size(), 1);
    const QJsonObject params = reportFixups.first().toObject().value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("amount_pt")).toDouble(), 9.0);
    QCOMPARE(params.value(QStringLiteral("mode")).toString(), QStringLiteral("mirror"));
}

void PreflightEngineTest::run_synthesizesAddBleedWhenGapAndNoProfileFixup()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    pdf::PreflightResult result = engine.run(profile);
    QCOMPARE(result.fixupsAvailable.size(), 1);
    QCOMPARE(result.fixupsAvailable.first().id, QStringLiteral("add-bleed"));
    QCOMPARE(result.fixupsAvailable.first().amountPt, 9.0);

    const QJsonObject report = result.toJson();
    const QJsonArray reportFixups = report.value(QStringLiteral("fixups_available")).toArray();
    QCOMPARE(reportFixups.size(), 1);
    const QJsonObject params = reportFixups.first().toObject().value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("amount_pt")).toDouble(), 9.0);
    QCOMPARE(params.value(QStringLiteral("mode")).toString(), QStringLiteral("mirror"));
}

void PreflightEngineTest::run_removesAddBleedWhenNoGap()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 220, 220));
    builder.setPageTrimBox(page, QRectF(10, 10, 200, 200));
    builder.setPageBleedBox(page, QRectF(0, 0, 220, 220));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);
    QJsonArray fixups;
    fixups.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("add-bleed") },
        { QStringLiteral("amount_pt"), 9 }
    });
    profile.insert(QStringLiteral("fixups"), fixups);

    pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    for (const pdf::PreflightFixupConfig& fixup : result.fixupsAvailable)
    {
        QVERIFY(fixup.id != QStringLiteral("add-bleed"));
    }
}

void PreflightEngineTest::run_doesNotAdvertiseUnimplementedFixups()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("bleed") },
        { QStringLiteral("amount_pt"), 9 },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);
    QJsonArray fixups;
    fixups.append(QJsonObject{ { QStringLiteral("id"), QStringLiteral("rgb-to-cmyk") } });
    fixups.append(QJsonObject{ { QStringLiteral("id"), QStringLiteral("add-bleed") }, { QStringLiteral("amount_pt"), 9 } });
    fixups.append(QJsonObject{ { QStringLiteral("id"), QStringLiteral("downsample-images") }, { QStringLiteral("target_dpi"), 300 } });
    fixups.append(QJsonObject{ { QStringLiteral("id"), QStringLiteral("stale-fixup") } });
    profile.insert(QStringLiteral("fixups"), fixups);

    pdf::PreflightResult result = engine.run(profile);
    QCOMPARE(result.fixupsAvailable.size(), 1);
    QCOMPARE(result.fixupsAvailable.first().id, QStringLiteral("add-bleed"));

    const QJsonArray reportFixups = result.toJson().value(QStringLiteral("fixups_available")).toArray();
    QCOMPARE(reportFixups.size(), 1);
    QCOMPARE(reportFixups.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("add-bleed"));
}

void PreflightEngineTest::run_invalidProfileEmitsDocumentScopeFinding()
{
    pdf::PreflightEngine engine(nullptr);
    const pdf::PreflightResult result = engine.run(QJsonObject{
        { QStringLiteral("name"), QStringLiteral("Broken") }
    });

    QVERIFY(!result.pass);
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().scope, QString::fromLatin1(pdf::PREFLIGHT_FINDING_SCOPE_DOCUMENT));
    QVERIFY(!result.errors.first().bbox.isValid());

    const QJsonObject report = result.toJson();
    QCOMPARE(report.value(QStringLiteral("schema_version")).toInt(), pdf::PREFLIGHT_REPORT_SCHEMA_VERSION);
    const QJsonObject finding = report.value(QStringLiteral("errors")).toArray().at(0).toObject();
    QCOMPARE(finding.value(QStringLiteral("scope")).toString(), QStringLiteral("document"));
    QVERIFY(!finding.contains(QStringLiteral("page")));
    QVERIFY(!finding.contains(QStringLiteral("bbox")));
}

void PreflightEngineTest::findingStableId_ignoresMessageAndBbox()
{
    pdf::PreflightFinding first;
    first.scope = QStringLiteral("page");
    first.page = 2;
    first.objectId = QStringLiteral("12 0 R");
    first.type = QStringLiteral("bleed");
    first.checkId = QStringLiteral("bleed");
    first.message = QStringLiteral("English message");
    first.bbox = QRectF(1.0, 2.0, 3.0, 4.0);

    pdf::PreflightFinding second = first;
    second.message = QStringLiteral("Translated message");
    second.bbox = QRectF(50.0, 60.0, 70.0, 80.0);
    QCOMPARE(first.stableId(), second.stableId());

    second.checkId = QStringLiteral("trim");
    QVERIFY(first.stableId() != second.stableId());
}

void PreflightEngineTest::decisionRejectsMissingJustification()
{
    pdf::PreflightDecision decision;
    QString errorMessage;
    QVERIFY(!pdf::PreflightDecision::fromJson(QJsonObject{
        { QStringLiteral("finding_id"), QStringLiteral("0123456789abcdef") },
        { QStringLiteral("kind"), QStringLiteral("waive") },
        { QStringLiteral("operator"), QStringLiteral("m.berry") },
        { QStringLiteral("timestamp_utc"), QStringLiteral("2026-08-09T14:22:03.000Z") },
        { QStringLiteral("document_revision_digest"), QString(64, QLatin1Char('a')) },
        { QStringLiteral("effective_profile_digest"), QString(64, QLatin1Char('b')) }
    }, decision, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("justification")));
}

void PreflightEngineTest::decisionRoundTripAndStalenessAreDeterministic()
{
    const QString documentDigest(64, QLatin1Char('a'));
    const QString profileDigest(64, QLatin1Char('b'));
    pdf::PreflightDecision original;
    original.findingId = QStringLiteral("0123456789abcdef");
    original.kind = pdf::PreflightDecisionKind::Waive;
    original.justification = QStringLiteral("Client approved the supplied artwork.");
    original.operatorIdentity = QStringLiteral("m.berry");
    original.timestampUtc = QDateTime::fromString(QStringLiteral("2026-08-09T14:22:03.000Z"), Qt::ISODateWithMs);
    original.externalReference = QStringLiteral("JOB-4471");
    original.documentRevisionDigest = documentDigest;
    original.effectiveProfileDigest = profileDigest;

    const QJsonObject exported = original.toJson(documentDigest, profileDigest);
    QCOMPARE(exported.value(QStringLiteral("state")).toString(), QStringLiteral("active"));

    pdf::PreflightDecision imported;
    QString errorMessage;
    QVERIFY(pdf::PreflightDecision::fromJson(exported, imported, errorMessage));
    QCOMPARE(imported.findingId, original.findingId);
    QCOMPARE(imported.justification, original.justification);
    QCOMPARE(imported.timestampUtc, original.timestampUtc);
    QCOMPARE(imported.resolveState(documentDigest, profileDigest), pdf::PreflightDecisionState::Active);
    QCOMPARE(imported.resolveState(QString(64, QLatin1Char('c')), profileDigest), pdf::PreflightDecisionState::StaleDocument);
    QCOMPARE(imported.resolveState(documentDigest, QString(64, QLatin1Char('d'))), pdf::PreflightDecisionState::StaleProfile);

    QList<pdf::PreflightDecision> decisions{ original };
    const QJsonObject document = pdf::preflightDecisionsToJson(decisions);
    QList<pdf::PreflightDecision> roundTripped;
    QVERIFY(pdf::preflightDecisionsFromJson(document, roundTripped, errorMessage));
    QCOMPARE(roundTripped.size(), 1);
    QCOMPARE(roundTripped.first().toJson(), original.toJson());
}

void PreflightEngineTest::decisionReopenDoesNotCountForSignoff()
{
    const QString documentDigest(64, QLatin1Char('a'));
    const QString profileDigest(64, QLatin1Char('b'));
    pdf::PreflightDecision decision;
    decision.findingId = QStringLiteral("0123456789abcdef");
    decision.kind = pdf::PreflightDecisionKind::Reopen;
    decision.justification = QStringLiteral("Reopened for review.");
    decision.operatorIdentity = QStringLiteral("m.berry");
    decision.timestampUtc = QDateTime::currentDateTimeUtc();
    decision.documentRevisionDigest = documentDigest;
    decision.effectiveProfileDigest = profileDigest;
    QVERIFY(!decision.countsForSignoff(documentDigest, profileDigest));
}

void PreflightEngineTest::run_contentBleedWithoutRaster_emitsContentBleedAndNeedsAutoBleed()
{
    pdf::PDFDocument document = buildTieredBleedGapPage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const pdf::PreflightResult result = engine.run(tieredBleedProfile(false));
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 2);

    QCOMPARE(result.warnings.at(0).type, QStringLiteral("content-bleed"));
    QCOMPARE(result.warnings.at(0).checkId, QStringLiteral("content-bleed"));
    QCOMPARE(result.warnings.at(1).type, QStringLiteral("needs-auto-bleed"));
    QCOMPARE(result.fixupsAvailable.size(), 1);
    QCOMPARE(result.fixupsAvailable.first().id, QStringLiteral("add-bleed"));
}

void PreflightEngineTest::run_contentBleedRasterConfirm_emitsBleedMarginEmptyAndNeedsAutoBleed()
{
    pdf::PDFDocument document = buildTieredBleedGapPage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const pdf::PreflightResult result = engine.run(tieredBleedProfile(true));
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QVERIFY(result.warnings.size() >= 5);

    int bleedMarginEmptyCount = 0;
    bool hasNeedsAutoBleed = false;
    for (const pdf::PreflightFinding& finding : result.warnings)
    {
        if (finding.type == QStringLiteral("bleed-margin-empty"))
        {
            ++bleedMarginEmptyCount;
            QCOMPARE(finding.checkId, QStringLiteral("content-bleed"));
        }
        if (finding.type == QStringLiteral("needs-auto-bleed"))
        {
            hasNeedsAutoBleed = true;
        }
        QVERIFY(finding.type != QStringLiteral("content-bleed"));
    }

    QCOMPARE(bleedMarginEmptyCount, 4);
    QVERIFY(hasNeedsAutoBleed);
    QCOMPARE(result.fixupsAvailable.size(), 1);
    QCOMPARE(result.fixupsAvailable.first().id, QStringLiteral("add-bleed"));
}

void PreflightEngineTest::run_whiteOverprint_emitsWarningForWhitePaintWithOverprint()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/white-overprint.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("White overprint test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("white-overprint") },
        { QStringLiteral("severity"), QStringLiteral("warning") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().type, QStringLiteral("white-overprint"));
    QCOMPARE(result.warnings.first().checkId, QStringLiteral("white-overprint"));
}

void PreflightEngineTest::run_whiteOverprint_passesWhenOverprintOff()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/white-overprint-ok.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("White overprint ok"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("white-overprint") },
        { QStringLiteral("severity"), QStringLiteral("warning") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 0);
}

void PreflightEngineTest::run_whiteOverprint_emitsWarningInsideFormXObject()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/white-overprint-form.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("White overprint form"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("white-overprint") },
        { QStringLiteral("severity"), QStringLiteral("warning") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().type, QStringLiteral("white-overprint"));
    QCOMPARE(result.warnings.first().checkId, QStringLiteral("white-overprint"));
}

void PreflightEngineTest::run_inkCoverage_emitsRegionalWarningForOverLimitFixture()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/ink-coverage-over.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Ink coverage over limit"));
    profile.insert(QStringLiteral("checks"), QJsonArray{
        QJsonObject{
            { QStringLiteral("id"), QStringLiteral("ink-coverage") },
            { QStringLiteral("max_ink_pct"), 300 },
            { QStringLiteral("probe_dpi"), 150 },
            { QStringLiteral("min_region_area_pct"), 0.05 },
            { QStringLiteral("max_regions_per_page"), 20 },
            { QStringLiteral("severity"), QStringLiteral("warning") }
        }
    });

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().scope, QStringLiteral("object"));
    QCOMPARE(result.warnings.first().type, QStringLiteral("ink-coverage"));
    QCOMPARE(result.warnings.first().checkId, QStringLiteral("ink-coverage"));
    QVERIFY(result.warnings.first().bbox.isValid());
    QVERIFY(result.warnings.first().bbox.width() > 0.0);
    QVERIFY(result.warnings.first().bbox.height() > 0.0);
    QVERIFY(result.warnings.first().evidence.value(QStringLiteral("peak_ink_pct")).toDouble() > 300.0);
    QCOMPARE(result.warnings.first().evidence.value(QStringLiteral("max_ink_pct")).toDouble(), 300.0);
    QVERIFY(result.warnings.first().evidence.value(QStringLiteral("area_mm2")).toDouble() > 0.0);
    QCOMPARE(result.warnings.first().evidence.value(QStringLiteral("analysis_box")).toString(), QStringLiteral("bleed"));
    QVERIFY(result.warnings.first().message.contains(QStringLiteral("Total ink coverage")));
}

void PreflightEngineTest::run_inkCoverage_passesBelowLimitFixture()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/ink-coverage-ok.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Ink coverage below limit"));
    profile.insert(QStringLiteral("checks"), QJsonArray{
        QJsonObject{
            { QStringLiteral("id"), QStringLiteral("ink-coverage") },
            { QStringLiteral("max_ink_pct"), 300 },
            { QStringLiteral("probe_dpi"), 150 },
            { QStringLiteral("min_region_area_pct"), 0.05 },
            { QStringLiteral("max_regions_per_page"), 20 },
            { QStringLiteral("severity"), QStringLiteral("warning") }
        }
    });

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 0);
}

void PreflightEngineTest::run_inkCoverage_budgetAbortIsIncomplete()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 11000, 11000));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Ink coverage budget") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("ink-coverage") },
                { QStringLiteral("max_ink_pct"), 300 },
                { QStringLiteral("probe_dpi"), 72 },
                { QStringLiteral("max_raster_pixels"), 1000000 },
                { QStringLiteral("analysis_box"), QStringLiteral("media") }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(!result.inspectionComplete);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().type, QStringLiteral("check-incomplete"));
    QCOMPARE(result.checkStatuses.size(), 1);
    QCOMPARE(result.checkStatuses.first().status, QStringLiteral("skipped"));
    QCOMPARE(result.checkStatuses.first().reason, QStringLiteral("ink coverage raster exceeds the pixel budget"));
}

void PreflightEngineTest::inkCoverageProbe_usesAnalysisBoxAndReportsBudget()
{
    pdf::PDFDocumentBuilder builder;
    const pdf::PDFObjectReference pageReference = builder.appendPage(QRectF(0, 0, 10000, 10000));
    builder.setPageBleedBox(pageReference, QRectF(0, 0, 100, 100));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PDFInkCoverageProbe probe(&session);
    pdf::PDFInkCoverageProbeSettings settings;
    settings.dpi = 72;
    settings.maxRasterPixels = 1000000;
    settings.analysisBox = pdf::PDFInkCoverageAnalysisBox::Bleed;

    const pdf::PDFPage* page = document.getCatalog()->getPage(0);
    QVERIFY(page != nullptr);
    const pdf::PDFInkCoverageProbeResult bleedResult = probe.probe(page, 0, settings);
    QVERIFY(bleedResult.rasterized);
    QVERIFY(!bleedResult.budgetExceeded);

    settings.analysisBox = pdf::PDFInkCoverageAnalysisBox::Media;
    const pdf::PDFInkCoverageProbeResult mediaResult = probe.probe(page, 0, settings);
    QVERIFY(!mediaResult.rasterized);
    QVERIFY(mediaResult.budgetExceeded);
}

void PreflightEngineTest::run_downsampleFixupAdvertisedForHighDpiImage()
{
    pdf::PDFDocument document = buildHighDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Downsample candidate") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("image-resolution") }, { QStringLiteral("min_dpi"), 300 } }
        } },
        { QStringLiteral("fixups"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("downsample-images") }, { QStringLiteral("target_dpi"), 300 } }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QCOMPARE(result.fixupsAvailable.size(), 1);
    QCOMPARE(result.fixupsAvailable.first().id, QStringLiteral("downsample-images"));
    QCOMPARE(result.fixupsAvailable.first().params.value(QStringLiteral("target_dpi")).toInt(), 300);
    QCOMPARE(result.fixupsAvailable.first().params.value(QStringLiteral("candidate_count")).toInt(), 1);
}

void PreflightEngineTest::run_downsampleFixupHiddenWhenNoCandidateExists()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 144, 144));
    pdf::PDFDocument document = builder.build();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("No downsample candidate") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("bleed") }, { QStringLiteral("amount_pt"), 9 } }
        } },
        { QStringLiteral("fixups"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("downsample-images") }, { QStringLiteral("target_dpi"), 300 } }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(std::none_of(result.fixupsAvailable.cbegin(), result.fixupsAvailable.cend(), [](const pdf::PreflightFixupConfig& fixup)
    {
        return fixup.id == QStringLiteral("downsample-images");
    }));
}

void PreflightEngineTest::run_downsampleFixupCarriesTargetDpi()
{
    pdf::PDFDocument document = buildHighDpiImagePage();
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Downsample custom target") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("image-resolution") }, { QStringLiteral("min_dpi"), 300 } }
        } },
        { QStringLiteral("fixups"), QJsonArray{
            QJsonObject{ { QStringLiteral("id"), QStringLiteral("downsample-images") }, { QStringLiteral("target_dpi"), 450 } }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QCOMPARE(result.fixupsAvailable.size(), 1);
    const pdf::PreflightFixupConfig& fixup = result.fixupsAvailable.first();
    QCOMPARE(fixup.params.value(QStringLiteral("target_dpi")).toInt(), 450);
    QCOMPARE(fixup.params.value(QStringLiteral("quality")).toInt(), 90);
    QCOMPARE(fixup.params.value(QStringLiteral("preserve_color")).toBool(), true);
    QCOMPARE(fixup.params.value(QStringLiteral("preserve_transparency")).toBool(), true);
}

void PreflightEngineTest::run_colorRgbFixtureFailsColorMode()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/color-rgb.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    QJsonObject profile;
    profile.insert(QStringLiteral("name"), QStringLiteral("Color mode test"));
    QJsonArray checks;
    checks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("color-mode") },
        { QStringLiteral("allowed"), QJsonArray{ QStringLiteral("CMYK"), QStringLiteral("Grayscale") } },
        { QStringLiteral("severity"), QStringLiteral("error") }
    });
    profile.insert(QStringLiteral("checks"), checks);

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().type, QStringLiteral("color-mode"));
    QCOMPARE(result.errors.first().checkId, QStringLiteral("color-mode"));
    QVERIFY(result.inspectionComplete);

    const QJsonObject report = result.toJson();
    QCOMPARE(report.value(QStringLiteral("schema_version")).toInt(), pdf::PREFLIGHT_REPORT_SCHEMA_VERSION);
    QVERIFY(report.value(QStringLiteral("inspection_complete")).toBool());
}

void PreflightEngineTest::colorInventory_isRegisteredAndInfoOnly()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    QVERIFY(engine.hasCheck(QStringLiteral("color-inventory")));

    QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Color inventory test") },
        { QStringLiteral("checks"), QJsonArray{
              QJsonObject{
                  { QStringLiteral("id"), QStringLiteral("color-inventory") },
                  { QStringLiteral("severity"), QStringLiteral("info") },
                  { QStringLiteral("probe_dpi"), 72 },
                  { QStringLiteral("rich_black_k_percent"), 10 }
              }
          } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QCOMPARE(result.errors.size(), 0);
    QVERIFY(result.warnings.size() >= 4);
    for (const pdf::PreflightFinding& finding : result.warnings)
    {
        QCOMPARE(finding.checkId, QStringLiteral("color-inventory"));
        QCOMPARE(finding.severity, QStringLiteral("info"));
    }
    QCOMPARE(result.checkStatuses.size(), 1);
    QCOMPARE(result.checkStatuses.first().status, QStringLiteral("ok"));
}

void PreflightEngineTest::colorInventory_rejectsInvalidParameters()
{
    pdf::PreflightEngine engine(nullptr);
    pdf::PreflightProfileData profile;
    QString errorMessage;
    QJsonObject profileObject{
        { QStringLiteral("name"), QStringLiteral("Invalid color inventory") },
        { QStringLiteral("checks"), QJsonArray{
              QJsonObject{
                  { QStringLiteral("id"), QStringLiteral("color-inventory") },
                  { QStringLiteral("probe_dpi"), 0 }
              }
          } }
    };

    QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("probe_dpi")));

    QJsonArray invalidPercentChecks;
    invalidPercentChecks.append(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("color-inventory") },
        { QStringLiteral("rich_black_k_percent"), 101 }
    });
    profileObject[QStringLiteral("checks")] = invalidPercentChecks;
    errorMessage.clear();
    QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("rich_black_k_percent")));
}

void PreflightEngineTest::richBlackPredicate_distinguishesChromaticBlack()
{
    const pdf::PDFPixelFormat format = pdf::PDFPixelFormat::createFormatDefaultCMYK(0);
    pdf::PDFColorComponent blackOnly[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    pdf::PDFColorComponent richBlack[] = { 0.4f, 0.3f, 0.3f, 1.0f };
    pdf::PDFColorComponent cmyOnly[] = { 0.4f, 0.3f, 0.3f, 0.0f };

    QVERIFY(!pdf::isRichBlackPixel(pdf::PDFConstColorBuffer(blackOnly, 4), format, 0.10f));
    QVERIFY(pdf::isRichBlackPixel(pdf::PDFConstColorBuffer(richBlack, 4), format, 0.10f));
    QVERIFY(!pdf::isRichBlackPixel(pdf::PDFConstColorBuffer(cmyOnly, 4), format, 0.10f));
}

void PreflightEngineTest::run_outputIntent_missingIntentEmitsError()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Output intent missing") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("output-intent") },
                { QStringLiteral("severity"), QStringLiteral("error") }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.first().scope, QStringLiteral("document"));
    QCOMPARE(result.errors.first().type, QStringLiteral("output-intent-missing"));
}

void PreflightEngineTest::run_outputIntent_notRequiredPassesWithoutIntent()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Optional output intent") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("output-intent") },
                { QStringLiteral("required"), false }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QVERIFY(result.errors.isEmpty());
    QVERIFY(result.warnings.isEmpty());
}

void PreflightEngineTest::run_outputIntent_emptyIdentifierEmitsIdentityFinding()
{
    const QByteArray profileContent = loadOutputIntentProfile(
        QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/output-intent-cmyk.pdf"));
    QVERIFY(!profileContent.isEmpty());

    pdf::PDFDocument document = buildOutputIntentDocument({ { QByteArray(), QByteArrayLiteral("CMYK"), profileContent } });
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Output intent identity") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("output-intent") } } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(result.errors.size() >= 1);
    QCOMPARE(result.errors.first().type, QStringLiteral("output-intent-identity"));
}

void PreflightEngineTest::run_outputIntent_identifierNotInAllowListEmitsIdentityFinding()
{
    const QByteArray profileContent = loadOutputIntentProfile(
        QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/output-intent-cmyk.pdf"));
    QVERIFY(!profileContent.isEmpty());

    pdf::PDFDocument document = buildOutputIntentDocument({ { QByteArrayLiteral("Other"), QByteArrayLiteral("CMYK"), profileContent } });
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Output intent identity allow list") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("output-intent") },
                { QStringLiteral("allowed_identifiers"), QJsonArray{ QStringLiteral("CGATS TR 001") } }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(result.errors.size() >= 1);
    QCOMPARE(result.errors.first().type, QStringLiteral("output-intent-identity"));
}

void PreflightEngineTest::run_outputIntent_profileCsDisagreesWithEmbeddedProfile()
{
    const QByteArray profileContent = loadOutputIntentProfile(
        QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/output-intent-rgb.pdf"));
    QVERIFY(!profileContent.isEmpty());

    pdf::PDFDocument document = buildOutputIntentDocument({ { QByteArrayLiteral("CGATS TR 001"), QByteArrayLiteral("CMYK"), profileContent } });
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Output intent profile CS") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("output-intent") },
                { QStringLiteral("allowed"), QJsonArray{ QStringLiteral("RGB") } }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(result.errors.size() >= 1);
    QCOMPARE(result.errors.first().type, QStringLiteral("output-intent-color-mismatch"));
    QVERIFY(result.errors.first().message.contains(QStringLiteral("ProfileCS")));
}

void PreflightEngineTest::run_outputIntent_conflictingIntentsEmitConflictFinding()
{
    const QByteArray cmykProfile = loadOutputIntentProfile(
        QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/output-intent-cmyk.pdf"));
    const QByteArray rgbProfile = loadOutputIntentProfile(
        QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/output-intent-rgb.pdf"));
    QVERIFY(!cmykProfile.isEmpty());
    QVERIFY(!rgbProfile.isEmpty());

    pdf::PDFDocument document = buildOutputIntentDocument({
        { QByteArrayLiteral("CGATS TR 001"), QByteArrayLiteral("CMYK"), cmykProfile },
        { QByteArrayLiteral("sRGB"), QByteArrayLiteral("RGB"), rgbProfile }
    });
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Output intent conflict") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{ { QStringLiteral("id"), QStringLiteral("output-intent") } } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(result.errors.size() >= 1);
    const auto conflict = std::find_if(result.errors.cbegin(), result.errors.cend(), [](const pdf::PreflightFinding& finding) {
        return finding.type == QStringLiteral("output-intent-conflict");
    });
    QVERIFY(conflict != result.errors.cend());
    QVERIFY(conflict->message.contains(QStringLiteral("CMYK, RGB")));
}

void PreflightEngineTest::run_outputIntent_strictMultipleIntentsEmitAmbiguityFinding()
{
    const QByteArray cmykProfile = loadOutputIntentProfile(
        QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/output-intent-cmyk.pdf"));
    QVERIFY(!cmykProfile.isEmpty());

    pdf::PDFDocument document = buildOutputIntentDocument({
        { QByteArrayLiteral("first"), QByteArrayLiteral("CMYK"), cmykProfile },
        { QByteArrayLiteral("second"), QByteArrayLiteral("CMYK"), cmykProfile }
    });
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Strict output intent cardinality") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("output-intent") },
            { QStringLiteral("allow_multiple"), false }
        } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    const auto ambiguity = std::find_if(result.errors.cbegin(), result.errors.cend(), [](const pdf::PreflightFinding& finding) {
        return finding.type == QStringLiteral("output-intent-ambiguous");
    });
    QVERIFY(ambiguity != result.errors.cend());
    QVERIFY(ambiguity->message.contains(QStringLiteral("intent 0")));
    QVERIFY(ambiguity->message.contains(QStringLiteral("intent 1")));
}

void PreflightEngineTest::run_outputIntent_optionalEmbeddedProfileCanBeAbsent()
{
    pdf::PDFDocument document = buildOutputIntentDocument({
        { QByteArrayLiteral("metadata-only"), QByteArrayLiteral("CMYK"), QByteArray(), false }
    });
    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Optional embedded output profile") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("output-intent") },
            { QStringLiteral("require_embedded_profile"), false }
        } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QVERIFY(result.errors.isEmpty());
}

void PreflightEngineTest::run_outputIntent_malformedArrayEntryEmitsFinding()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFArray outputIntents;
    outputIntents.appendItem(pdf::PDFObject::createInteger(42));
    pdf::PDFDictionary catalog;
    catalog.addEntry(pdf::PDFInplaceOrMemoryString("OutputIntents"),
                     pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(outputIntents))));
    builder.mergeTo(builder.getCatalogReference(),
                    pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(catalog))));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Malformed output intents") },
        { QStringLiteral("checks"), QJsonArray{ QJsonObject{
            { QStringLiteral("id"), QStringLiteral("output-intent") }
        } } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    const auto malformed = std::find_if(result.errors.cbegin(), result.errors.cend(), [](const pdf::PreflightFinding& finding) {
        return finding.type == QStringLiteral("output-intent-malformed");
    });
    QVERIFY(malformed != result.errors.cend());
}

void PreflightEngineTest::run_outputIntent_severityWarningRoutesToWarnings()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Output intent warning") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("output-intent") },
                { QStringLiteral("severity"), QStringLiteral("warning") }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(result.pass);
    QVERIFY(result.errors.isEmpty());
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().type, QStringLiteral("output-intent-missing"));
}

QTEST_GUILESS_MAIN(PreflightEngineTest)

#include "tst_preflightenginetest.moc"
