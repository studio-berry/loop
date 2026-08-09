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
#include "pdfinkcoverageprobe.h"
#include "pdfdocumentbuilder.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentsession.h"

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
    void parseProfile_rejectsInkCoverageWithoutPositiveMax();
    void parseProfile_rejectsInkCoverageInvalidParameters();
    void parseProfile_acceptsInkCoverageDefaults();
    void parseProfile_acceptsThinStrokeOverridesAndDefaults();
    void parseProfile_rejectsInvalidThinStrokeThreshold();
    void parseProfile_rejectsOutputIntentInvalidAllowedColorSpace();
    void run_bleedCheckFailsWhenBoxMissing();
    void run_bleedCheckPassesWhenBoxAdequate();
    void run_unknownCheckIdIsIgnored();
    void run_thinStrokes_detectsPaintedThinStroke();
    void run_includesProfileFixups();
    void run_synthesizesAddBleedWhenGapAndNoProfileFixup();
    void run_removesAddBleedWhenNoGap();
    void run_doesNotAdvertiseUnimplementedFixups();
    void run_invalidProfileEmitsDocumentScopeFinding();
    void run_contentBleedWithoutRaster_emitsContentBleedAndNeedsAutoBleed();
    void run_contentBleedRasterConfirm_emitsBleedMarginEmptyAndNeedsAutoBleed();
    void run_whiteOverprint_emitsWarningForWhitePaintWithOverprint();
    void run_whiteOverprint_passesWhenOverprintOff();
    void run_whiteOverprint_emitsWarningInsideFormXObject();
    void run_inkCoverage_emitsRegionalWarningForOverLimitFixture();
    void run_inkCoverage_passesBelowLimitFixture();
    void run_inkCoverage_exactLimitDoesNotFail();
    void run_inkCoverage_budgetAbortMarksInspectionIncomplete();
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

void PreflightEngineTest::parseProfile_rejectsInkCoverageInvalidParameters()
{
    pdf::PreflightEngine engine(nullptr);

    const auto parseCheck = [&](const QJsonObject& checkObject)
    {
        QJsonObject profileObject{
            { QStringLiteral("name"), QStringLiteral("Ink coverage") },
            { QStringLiteral("checks"), QJsonArray{ checkObject } }
        };
        pdf::PreflightProfileData profile;
        QString errorMessage;
        QVERIFY(!engine.parseProfile(profileObject, profile, errorMessage));
        QVERIFY(!errorMessage.isEmpty());
    };

    parseCheck(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("ink-coverage") },
        { QStringLiteral("max_ink_pct"), QStringLiteral("300") }
    });
    parseCheck(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("ink-coverage") },
        { QStringLiteral("max_ink_pct"), 300 },
        { QStringLiteral("min_region_area_pct"), -0.1 }
    });
    parseCheck(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("ink-coverage") },
        { QStringLiteral("max_ink_pct"), 300 },
        { QStringLiteral("max_regions_per_page"), -1 }
    });
    parseCheck(QJsonObject{
        { QStringLiteral("id"), QStringLiteral("ink-coverage") },
        { QStringLiteral("max_ink_pct"), 300 },
        { QStringLiteral("analysis_box"), QStringLiteral("slug") }
    });
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
    QCOMPARE(profile.checks.first().maxRasterPixels, qint64(250) * 1000 * 1000);
    QCOMPARE(profile.checks.first().analysisBox, pdf::PDFInkCoverageAnalysisBox::Bleed);
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

void PreflightEngineTest::run_inkCoverage_exactLimitDoesNotFail()
{
    const QString fixturePath = QStringLiteral(LOUPE_PREFLIGHT_SOURCE_DIR "/testdata/fixtures/ink-coverage-over.pdf");
    QVERIFY(QFile::exists(fixturePath));

    pdf::PDFDocumentReader reader(nullptr, [](bool*) { return QString(); }, true, false);
    pdf::PDFDocument document = reader.readFromFile(fixturePath);
    QCOMPARE(reader.getReadingResult(), pdf::PDFDocumentReader::Result::OK);

    pdf::PDFDocumentSession session(&document);
    pdf::PDFInkCoverageProbe probe(&session);
    pdf::PDFInkCoverageProbeSettings settings;
    settings.maxInkCoverage = 0.0;
    settings.minRegionAreaRatio = 0.0;
    const pdf::PDFPage* page = document.getCatalog()->getPage(0);
    const pdf::PDFInkCoverageProbeResult baseline = probe.probe(page, 0, settings);
    QVERIFY(baseline.rasterized);
    QVERIFY(baseline.peakInkCoverage > 0.0);

    settings.maxInkCoverage = baseline.peakInkCoverage;
    const pdf::PDFInkCoverageProbeResult atLimit = probe.probe(page, 0, settings);
    QVERIFY(atLimit.rasterized);
    QCOMPARE(atLimit.regions.size(), size_t(0));

    settings.maxInkCoverage = baseline.peakInkCoverage - 0.0001;
    const pdf::PDFInkCoverageProbeResult aboveLimit = probe.probe(page, 0, settings);
    QVERIFY(aboveLimit.rasterized);
    QVERIFY(!aboveLimit.regions.empty());
}

void PreflightEngineTest::run_inkCoverage_budgetAbortMarksInspectionIncomplete()
{
    pdf::PDFDocumentBuilder builder;
    builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFDocument document = builder.build();

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);
    const QJsonObject profile{
        { QStringLiteral("name"), QStringLiteral("Ink coverage budget") },
        { QStringLiteral("checks"), QJsonArray{
            QJsonObject{
                { QStringLiteral("id"), QStringLiteral("ink-coverage") },
                { QStringLiteral("max_ink_pct"), 300 },
                { QStringLiteral("max_raster_pixels"), 1 },
                { QStringLiteral("severity"), QStringLiteral("warning") }
            }
        } }
    };

    const pdf::PreflightResult result = engine.run(profile);
    QVERIFY(!result.pass);
    QVERIFY(!result.inspectionComplete);
    QCOMPARE(result.errors.size(), 0);
    QCOMPARE(result.warnings.size(), 1);
    QCOMPARE(result.warnings.first().type, QStringLiteral("ink-coverage-skipped"));
    QCOMPARE(result.warnings.first().checkId, QStringLiteral("ink-coverage"));
    QCOMPARE(result.checkStatuses.size(), 1);
    QCOMPARE(result.checkStatuses.first().status, QStringLiteral("skipped"));
    QCOMPARE(result.checkStatuses.first().reason, QStringLiteral("raster pixel budget exceeded"));

    const QJsonObject report = result.toJson();
    QVERIFY(!report.value(QStringLiteral("inspection_complete")).toBool());
    QCOMPARE(report.value(QStringLiteral("checks")).toArray().first().toObject().value(QStringLiteral("status")).toString(), QStringLiteral("skipped"));
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
