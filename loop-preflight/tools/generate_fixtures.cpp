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

// Regenerates the golden PDF fixtures under loop-preflight/testdata/fixtures/.
// The fixtures are committed to git; re-run this tool only when a fixture's
// geometry needs to change (see loop-preflight/README.md).
//
// Usage: generate_fixtures [output-directory]  (defaults to the current directory)

#include "pdfdocumentbuilder.h"
#include "pdfapplicationidentity.h"
#include "pdfconstants.h"
#include "pdfdocumentwriter.h"

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QGuiApplication>
#include <QPainter>

#include <lcms2.h>

#include <cstring>
#include <vector>

namespace
{

constexpr qreal MEDIA_SIZE_PT = 220.0;
constexpr qreal TRIM_INSET_PT = 10.0;
constexpr qreal TRIM_SIZE_PT = 200.0;

QByteArray createIccProfile(cmsColorSpaceSignature space)
{
    cmsHPROFILE profile = nullptr;
    if (space == cmsSigRgbData)
    {
        profile = cmsCreate_sRGBProfile();
    }
    else
    {
        profile = cmsCreateProfilePlaceholder(nullptr);
        if (profile)
        {
            cmsSetColorSpace(profile, space);
            cmsSetPCS(profile, cmsSigLabData);
            cmsSetDeviceClass(profile, cmsSigOutputClass);
            cmsSetProfileVersion(profile, 4.3);
        }
    }

    if (!profile)
    {
        qFatal("Failed to create synthetic ICC profile.");
    }

    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile, nullptr, &size) || size == 0)
    {
        cmsCloseProfile(profile);
        qFatal("Failed to determine synthetic ICC profile size.");
    }

    QByteArray content(static_cast<int>(size), '\0');
    if (!cmsSaveProfileToMem(profile, content.data(), &size))
    {
        cmsCloseProfile(profile);
        qFatal("Failed to save synthetic ICC profile.");
    }
    cmsCloseProfile(profile);

    // lcms writes the current time and a profile ID into the ICC header. Both
    // fields are irrelevant to these fixtures and would make regeneration
    // produce byte-different PDFs.
    if (content.size() >= 36)
    {
        std::memset(content.data() + 24, 0, 12);
    }
    if (content.size() >= 100)
    {
        std::memset(content.data() + 84, 0, 16);
    }

    return content;
}

void setDeterministicMetadata(pdf::PDFDocumentBuilder& builder)
{
    const QDateTime dateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
    builder.setDocumentCreationDate(dateTime);

    pdf::PDFObjectFactory factory;
    factory.beginDictionary();
    factory.beginDictionaryItem("ModDate");
    factory << dateTime;
    factory.endDictionaryItem();
    factory.endDictionary();
    builder.mergeTo(builder.getDocumentInfo(), factory.takeObject());
}

pdf::PDFDocument stabilizeDocumentMetadata(const pdf::PDFDocument& document)
{
    pdf::PDFObjectStorage storage(document.getStorage());
    const pdf::PDFDictionary* trailerDictionary = storage.getTrailerDictionary().getDictionary();
    const pdf::PDFObject infoObject = trailerDictionary ? trailerDictionary->get("Info") : pdf::PDFObject();
    if (infoObject.isReference())
    {
        const pdf::PDFObject currentInfo = storage.getObject(infoObject);
        if (currentInfo.isDictionary())
        {
            const QDateTime dateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
            pdf::PDFObjectFactory factory;
            factory.beginDictionary();
            factory.beginDictionaryItem("CreationDate");
            factory << dateTime;
            factory.endDictionaryItem();
            factory.beginDictionaryItem("ModDate");
            factory << dateTime;
            factory.endDictionaryItem();
            factory.endDictionary();

            storage.setObject(
                infoObject.getReference(),
                pdf::PDFObjectManipulator::merge(currentInfo,
                                                 factory.takeObject(),
                                                 pdf::PDFObjectManipulator::RemoveNullObjects));
        }
    }

    return pdf::PDFDocument(std::move(storage), document.getInfo()->version, document.getSourceDataHash());
}

pdf::PDFObjectReference appendOutputIntent(pdf::PDFDocumentBuilder& builder,
                                           const QByteArray& identifier,
                                           const QByteArray& profileCS,
                                           const QByteArray& profileContent)
{
    pdf::PDFDictionary intentDictionary;
    intentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("OutputIntent"));
    intentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("S"), pdf::PDFObject::createName("GTS_PDFX"));
    intentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("OutputConditionIdentifier"), pdf::PDFObject::createString(identifier));

    if (!profileContent.isNull())
    {
        pdf::PDFDictionary streamDictionary;
        streamDictionary.addEntry(pdf::PDFInplaceOrMemoryString("N"), pdf::PDFObject::createInteger(profileCS == QByteArrayLiteral("RGB") ? 3 : 4));
        streamDictionary.addEntry(pdf::PDFInplaceOrMemoryString("Length"), pdf::PDFObject::createInteger(profileContent.size()));
        const pdf::PDFObjectReference profileReference = builder.addObject(
            pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(
                std::move(streamDictionary), QByteArray(profileContent))));
        intentDictionary.addEntry(pdf::PDFInplaceOrMemoryString("DestOutputProfile"), pdf::PDFObject::createReference(profileReference));

        pdf::PDFDictionary profileInfo;
        profileInfo.addEntry(pdf::PDFInplaceOrMemoryString("ProfileCS"), pdf::PDFObject::createString(profileCS));
        intentDictionary.addEntry(
            pdf::PDFInplaceOrMemoryString("DestOutputProfileRef"),
            pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(profileInfo))));
    }

    return builder.addObject(
        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(intentDictionary))));
}

void setOutputIntents(pdf::PDFDocumentBuilder& builder, const std::vector<pdf::PDFObjectReference>& references)
{
    pdf::PDFArray intents;
    for (const pdf::PDFObjectReference reference : references)
    {
        intents.appendItem(pdf::PDFObject::createReference(reference));
    }

    pdf::PDFDictionary catalog;
    catalog.addEntry(
        pdf::PDFInplaceOrMemoryString("OutputIntents"),
        pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(intents))));
    builder.mergeTo(
        builder.getCatalogReference(),
        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(catalog))));
}

void writeFixture(const QDir& outputDir, const QString& fileName, pdf::PDFDocument document)
{
    document = stabilizeDocumentMetadata(document);
    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult result = writer.write(outputDir.filePath(fileName), &document, false);
    if (!result)
    {
        qFatal("Failed to write fixture '%s': %s", qPrintable(fileName), qPrintable(result.getErrorMessage()));
    }
}

pdf::PDFObject createNumberArray(std::initializer_list<qreal> values)
{
    pdf::PDFArray array;
    for (qreal value : values)
    {
        array.appendItem(pdf::PDFObject::createReal(value));
    }
    return pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(array)));
}

pdf::PDFObjectReference addContentStream(pdf::PDFDocumentBuilder& builder, const QByteArray& content)
{
    pdf::PDFDictionary dictionary;
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH), pdf::PDFObject::createInteger(content.size()));
    QByteArray streamData = content;
    return builder.addObject(pdf::PDFObject::createStream(std::make_shared<pdf::PDFStream>(std::move(dictionary), std::move(streamData))));
}

pdf::PDFObjectReference addExtGState(pdf::PDFDocumentBuilder& builder,
                                     bool overprintFilling,
                                     bool overprintStroking,
                                     int overprintMode,
                                     const char* blendMode = nullptr)
{
    pdf::PDFDictionary dictionary;
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("ExtGState"));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("OP"), pdf::PDFObject::createBool(overprintFilling));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("op"), pdf::PDFObject::createBool(overprintStroking));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("OPM"), pdf::PDFObject::createInteger(overprintMode));
    if (blendMode)
    {
        dictionary.setEntry(pdf::PDFInplaceOrMemoryString("BM"), pdf::PDFObject::createName(blendMode));
    }
    return builder.addObject(pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(dictionary))));
}

pdf::PDFObjectReference addType2TintFunction(pdf::PDFDocumentBuilder& builder,
                                             std::initializer_list<qreal> c1)
{
    pdf::PDFDictionary dictionary;
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("FunctionType"), pdf::PDFObject::createInteger(2));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("Domain"), createNumberArray({ 0.0, 1.0 }));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("C0"), createNumberArray({ 0.0, 0.0, 0.0, 0.0 }));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("C1"), createNumberArray(c1));
    dictionary.setEntry(pdf::PDFInplaceOrMemoryString("N"), pdf::PDFObject::createReal(1.0));
    return builder.addObject(pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(dictionary))));
}

pdf::PDFObject createSeparationColorSpace(pdf::PDFObjectReference tintFunction)
{
    pdf::PDFArray array;
    array.appendItem(pdf::PDFObject::createName("Separation"));
    array.appendItem(pdf::PDFObject::createName("Spot"));
    array.appendItem(pdf::PDFObject::createName("DeviceCMYK"));
    array.appendItem(pdf::PDFObject::createReference(tintFunction));
    return pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(array)));
}

pdf::PDFObject createDeviceNColorSpace(pdf::PDFObjectReference tintFunction)
{
    pdf::PDFArray colorants;
    colorants.appendItem(pdf::PDFObject::createName("Spot"));

    pdf::PDFArray array;
    array.appendItem(pdf::PDFObject::createName("DeviceN"));
    array.appendItem(pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(colorants))));
    array.appendItem(pdf::PDFObject::createName("DeviceCMYK"));
    array.appendItem(pdf::PDFObject::createReference(tintFunction));
    return pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(array)));
}

pdf::PDFDocument createOverprintDocument(bool enabled,
                                         int overprintMode,
                                         const QByteArray& overlay,
                                         const char* blendMode = nullptr,
                                         bool group = false)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - overprint render");
    builder.setDocumentCreator(QCoreApplication::applicationName());

    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    const pdf::PDFObjectReference state = addExtGState(builder, enabled, enabled, overprintMode, blendMode);
    const pdf::PDFObjectReference tintFunction = addType2TintFunction(builder, { 0.0, 0.0, 1.0, 0.0 });

    pdf::PDFDictionary resources;
    pdf::PDFDictionary extGStates;
    extGStates.addEntry(pdf::PDFInplaceOrMemoryString("GS"), pdf::PDFObject::createReference(state));
    resources.addEntry(pdf::PDFInplaceOrMemoryString("ExtGState"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(extGStates))));

    pdf::PDFDictionary colorSpaces;
    colorSpaces.addEntry(pdf::PDFInplaceOrMemoryString("SpotCS"), createSeparationColorSpace(tintFunction));
    resources.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(colorSpaces))));

    QByteArray content;
    if (group)
    {
        pdf::PDFDictionary formResources;
        pdf::PDFDictionary formExtGStates;
        formExtGStates.addEntry(pdf::PDFInplaceOrMemoryString("GS"), pdf::PDFObject::createReference(state));
        formResources.addEntry(pdf::PDFInplaceOrMemoryString("ExtGState"),
                               pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(formExtGStates))));
        pdf::PDFDictionary formDictionary;
        formDictionary.setEntry(pdf::PDFInplaceOrMemoryString("Type"), pdf::PDFObject::createName("XObject"));
        formDictionary.setEntry(pdf::PDFInplaceOrMemoryString("Subtype"), pdf::PDFObject::createName("Form"));
        formDictionary.setEntry(pdf::PDFInplaceOrMemoryString("BBox"), createNumberArray({ 0.0, 0.0, 200.0, 200.0 }));
        formDictionary.setEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                                pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(formResources))));
        pdf::PDFDictionary transparencyGroup;
        transparencyGroup.setEntry(pdf::PDFInplaceOrMemoryString("S"), pdf::PDFObject::createName("Transparency"));
        formDictionary.setEntry(pdf::PDFInplaceOrMemoryString("Group"),
                                pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(transparencyGroup))));
        const QByteArray formContent = "q /GS gs /DeviceCMYK cs 1 0 0 0 scn 20 20 160 160 re f 0 0 0 0.5 scn 35 35 130 130 re B Q";
        formDictionary.setEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH), pdf::PDFObject::createInteger(formContent.size()));
        const pdf::PDFObjectReference form = builder.addObject(pdf::PDFObject::createStream(
            std::make_shared<pdf::PDFStream>(std::move(formDictionary), QByteArray(formContent))));

        pdf::PDFDictionary xObjects;
        xObjects.addEntry(pdf::PDFInplaceOrMemoryString("Form"), pdf::PDFObject::createReference(form));
        resources.addEntry(pdf::PDFInplaceOrMemoryString("XObject"),
                           pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(xObjects))));
        content = "q /Form Do Q";
    }
    else
    {
        content = "q /GS gs /DeviceCMYK cs 1 0 0 0 scn 0 0 200 200 re f ";
        content += overlay;
        content += " Q";
    }

    const pdf::PDFObjectReference contentReference = addContentStream(builder, content);
    pdf::PDFDictionary pageUpdate;
    pageUpdate.setEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.setEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
    return builder.build();
}

void generateOverprintRenderFixtures(const QDir& outputDir)
{
    const auto writePair = [&](const QString& name, int opm, const QByteArray& overlay, const char* blendMode = nullptr, bool group = false)
    {
        writeFixture(outputDir, name + QStringLiteral("-off.pdf"), createOverprintDocument(false, opm, overlay, blendMode, group));
        writeFixture(outputDir, name + QStringLiteral("-on.pdf"), createOverprintDocument(true, opm, overlay, blendMode, group));
    };

    writePair(QStringLiteral("overprint-cmyk-mode0"), 0, "0 0 0 0.5 scn 35 35 130 130 re f");
    writePair(QStringLiteral("overprint-cmyk-mode1"), 1, "0 0 0 0.5 scn 35 35 130 130 re f");
    writePair(QStringLiteral("overprint-white"), 0, "0 0 0 0 scn 35 35 130 130 re f");
    writePair(QStringLiteral("overprint-separation"), 0, "/SpotCS cs 1 scn 35 35 130 130 re f");
    writePair(QStringLiteral("overprint-multiply"), 0, "0 0 0 1 scn 35 35 130 130 re f", "Multiply");
    writePair(QStringLiteral("overprint-group-stroke"), 0, QByteArray(), nullptr, true);
}

void generateWhiteOverprintColorSpaceFixtures(const QDir& outputDir)
{
    const auto writeColorSpaceFixture = [&](const QString& name, bool deviceN, bool iccBased)
    {
        pdf::PDFDocumentBuilder builder;
        builder.setDocumentTitle("Loop fixture - white overprint color space");
        const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
        const pdf::PDFObjectReference state = addExtGState(builder, true, true, 0);
        const pdf::PDFObjectReference tintFunction = addType2TintFunction(builder, { 0.0, 0.0, 0.0, 0.0 });
        pdf::PDFObject colorSpace;
        QByteArray color = "0";
        if (deviceN)
        {
            colorSpace = createDeviceNColorSpace(tintFunction);
        }
        else if (iccBased)
        {
            color = "0 0 0 0";
            pdf::PDFDictionary iccDictionary;
            iccDictionary.setEntry(pdf::PDFInplaceOrMemoryString("N"), pdf::PDFObject::createInteger(4));
            iccDictionary.setEntry(pdf::PDFInplaceOrMemoryString("Alternate"), pdf::PDFObject::createName("DeviceCMYK"));
            iccDictionary.setEntry(pdf::PDFInplaceOrMemoryString(pdf::PDF_STREAM_DICT_LENGTH), pdf::PDFObject::createInteger(0));
            const pdf::PDFObjectReference iccStream = builder.addObject(pdf::PDFObject::createStream(
                std::make_shared<pdf::PDFStream>(std::move(iccDictionary), QByteArray())));
            pdf::PDFArray iccArray;
            iccArray.appendItem(pdf::PDFObject::createName("ICCBased"));
            iccArray.appendItem(pdf::PDFObject::createReference(iccStream));
            colorSpace = pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(iccArray)));
        }
        else
        {
            colorSpace = createSeparationColorSpace(tintFunction);
        }

        pdf::PDFDictionary extGStates;
        extGStates.addEntry(pdf::PDFInplaceOrMemoryString("GS"), pdf::PDFObject::createReference(state));
        pdf::PDFDictionary colorSpaces;
        colorSpaces.addEntry(pdf::PDFInplaceOrMemoryString("CS1"), std::move(colorSpace));
        pdf::PDFDictionary resources;
        resources.addEntry(pdf::PDFInplaceOrMemoryString("ExtGState"), pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(extGStates))));
        resources.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"), pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(colorSpaces))));
        const QByteArray content = "/GS gs /CS1 cs " + color + " scn 20 20 160 160 re f";
        const pdf::PDFObjectReference contentReference = addContentStream(builder, content);
        pdf::PDFDictionary pageUpdate;
        pageUpdate.setEntry(pdf::PDFInplaceOrMemoryString("Resources"), pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
        pageUpdate.setEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
        builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
        writeFixture(outputDir, name + QStringLiteral(".pdf"), builder.build());
    };

    writeColorSpaceFixture(QStringLiteral("white-overprint-separation"), false, false);
    writeColorSpaceFixture(QStringLiteral("white-overprint-devicen"), true, false);
    writeColorSpaceFixture(QStringLiteral("white-overprint-iccbased"), false, true);
}

pdf::PDFObjectReference appendTieredBleedPage(pdf::PDFDocumentBuilder& builder, const QRectF& contentRect)
{
    // MediaBox 220x220, TrimBox 200x200 inset 10pt on every side. BleedBox is left
    // unset; per PDFPage::parse it falls back to CropBox -> MediaBox, giving a
    // 10pt margin over TrimBox (>= the 9pt amount_pt used by tiered-bleed profiles).
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, MEDIA_SIZE_PT, MEDIA_SIZE_PT));
    builder.setPageTrimBox(page, QRectF(TRIM_INSET_PT, TRIM_INSET_PT, TRIM_SIZE_PT, TRIM_SIZE_PT));

    pdf::PDFPageContentStreamBuilder pageContentStreamBuilder(&builder,
                                                              pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = pageContentStreamBuilder.begin(page))
    {
        painter->fillRect(contentRect, Qt::black);
        pageContentStreamBuilder.end(painter);
    }

    return page;
}

void generateBleedAdequateFixture(const QDir& outputDir)
{
    // MediaBox 220x220, TrimBox 200x200 inset 10pt on every side. BleedBox is left
    // unset; per PDFPage::parse (pdfpage.cpp) it then falls back to CropBox, which
    // itself falls back to MediaBox - a 10pt margin over TrimBox, above the 9pt
    // amount_pt required by loop-default.json's bleed check.
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - bleed adequate");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: adequate bleed");

    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, MEDIA_SIZE_PT, MEDIA_SIZE_PT));
    builder.setPageTrimBox(page, QRectF(TRIM_INSET_PT, TRIM_INSET_PT, TRIM_SIZE_PT, TRIM_SIZE_PT));

    writeFixture(outputDir, "bleed-adequate.pdf", builder.build());
}

void generateBleedMissingFixture(const QDir& outputDir)
{
    // Only MediaBox is set. Per PDFPage::parse (pdfpage.cpp), an unset CropBox falls
    // back to MediaBox, and unset BleedBox/TrimBox both fall back to CropBox - so
    // bleed, trim, and media all end up equal here, giving zero bleed margin and
    // failing the bleed check (< amount_pt on every edge).
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - bleed missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: missing bleed");

    builder.appendPage(QRectF(0, 0, 200, 200));

    writeFixture(outputDir, "bleed-missing.pdf", builder.build());
}

void generateContentBleedAdequateFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - content bleed adequate");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: artwork extends into bleed");

    appendTieredBleedPage(builder, QRectF(0, 0, MEDIA_SIZE_PT, MEDIA_SIZE_PT));
    writeFixture(outputDir, "content-bleed-adequate.pdf", builder.build());
}

void generateContentBleedMissingFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - content bleed missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: artwork stops at trim");

    appendTieredBleedPage(builder, QRectF(TRIM_INSET_PT, TRIM_INSET_PT, TRIM_SIZE_PT, TRIM_SIZE_PT));
    writeFixture(outputDir, "content-bleed-missing.pdf", builder.build());
}

void generateContentBleedRasterConfirmFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - content bleed raster confirm");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: fast bounds flags bleed gap");

    appendTieredBleedPage(builder, QRectF(TRIM_INSET_PT, TRIM_INSET_PT, TRIM_SIZE_PT, TRIM_SIZE_PT));
    writeFixture(outputDir, "content-bleed-raster-confirm.pdf", builder.build());
}

void generateContentBleedThreeOfFourFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - content bleed three of four");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: bleed on three edges only");

    // Artwork covers left, right, and top bleed strips but stops above the bottom strip.
    appendTieredBleedPage(builder, QRectF(0, TRIM_INSET_PT + 1.0, MEDIA_SIZE_PT, MEDIA_SIZE_PT - TRIM_INSET_PT - 1.0));
    writeFixture(outputDir, "content-bleed-three-of-four.pdf", builder.build());
}

void generateOutputIntentCmykFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - output intent CMYK");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: valid CMYK output intent");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setOutputIntents(builder, { appendOutputIntent(builder,
                                                   "CGATS TR 001",
                                                   "CMYK",
                                                   createIccProfile(cmsSigCmykData)) });
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-cmyk.pdf", builder.build());
}

void generateOutputIntentMissingFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - output intent missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: missing output intent");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-missing.pdf", builder.build());
}

void generateOutputIntentProfileMissingFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - output intent profile missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: output intent without ICC profile");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setOutputIntents(builder, { appendOutputIntent(builder, "CGATS TR 001", "CMYK", QByteArray()) });
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-profile-missing.pdf", builder.build());
}

void generateOutputIntentRgbFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - output intent RGB");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: disallowed RGB output intent");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setOutputIntents(builder, { appendOutputIntent(builder,
                                                   "CGATS TR 001",
                                                   "RGB",
                                                   createIccProfile(cmsSigRgbData)) });
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-rgb.pdf", builder.build());
}

void generateOutputIntentProfileInvalidFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - output intent profile invalid");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: invalid output intent ICC profile");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setOutputIntents(builder, { appendOutputIntent(builder,
                                                   "CGATS TR 001",
                                                   "CMYK",
                                                   QByteArrayLiteral("not an ICC profile")) });
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-profile-invalid.pdf", builder.build());
}

void appendColorInventoryPage(pdf::PDFDocumentBuilder& builder, const QByteArray& paintOps)
{
    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 400, 400));
    builder.setPageTrimBox(page, QRectF(20, 20, 360, 360));
    builder.setPageBleedBox(page, QRectF(0, 0, 400, 400));
    const QByteArray content = QByteArray("/DeviceCMYK cs\n") + paintOps + QByteArray("50 50 300 300 re\nf\n");
    const pdf::PDFObjectReference contentReference = addContentStream(builder, content);
    pdf::PDFDictionary pageUpdate;
    pageUpdate.setEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));
}

void generateColorInventoryRichBlackFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - rich black");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: rich black inventory");
    appendColorInventoryPage(builder, QByteArray("0.40 0.30 0.30 1.00 sc\n"));
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "rich-black.pdf", builder.build());
}

void generateColorInventoryBlackOnlyFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - black only");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: process black only");
    appendColorInventoryPage(builder, QByteArray("0 0 0 1 sc\n"));
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "black-only.pdf", builder.build());
}

void generateColorInventoryCmyRichNoKFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - CMY rich no K");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: chromatic CMY without K");
    appendColorInventoryPage(builder, QByteArray("0.40 0.30 0.30 0 sc\n"));
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "cmy-rich-no-k.pdf", builder.build());
}

void generateColorInventorySpotFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - color inventory spot");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: named separation colorant");

    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 400, 400));
    builder.setPageTrimBox(page, QRectF(20, 20, 360, 360));
    builder.setPageBleedBox(page, QRectF(0, 0, 400, 400));

    const pdf::PDFObjectReference tintFunction = addType2TintFunction(builder, { 0.0, 0.91, 0.76, 0.0 });
    pdf::PDFArray separationArray;
    separationArray.appendItem(pdf::PDFObject::createName("Separation"));
    separationArray.appendItem(pdf::PDFObject::createName("PANTONE#20185#20C"));
    separationArray.appendItem(pdf::PDFObject::createName("DeviceCMYK"));
    separationArray.appendItem(pdf::PDFObject::createReference(tintFunction));
    const pdf::PDFObjectReference separationReference = builder.addObject(
        pdf::PDFObject::createArray(std::make_shared<pdf::PDFArray>(std::move(separationArray))));

    pdf::PDFDictionary colorSpaces;
    colorSpaces.addEntry(pdf::PDFInplaceOrMemoryString("CS1"), pdf::PDFObject::createReference(separationReference));
    pdf::PDFDictionary resources;
    resources.addEntry(pdf::PDFInplaceOrMemoryString("ColorSpace"),
                       pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(colorSpaces))));

    const QByteArray content = QByteArray("/CS1 cs\n0.8 scn\n50 50 300 300 re\nf\n");
    const pdf::PDFObjectReference contentReference = addContentStream(builder, content);
    pdf::PDFDictionary pageUpdate;
    pageUpdate.setEntry(pdf::PDFInplaceOrMemoryString("Resources"),
                        pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(resources))));
    pageUpdate.setEntry(pdf::PDFInplaceOrMemoryString("Contents"), pdf::PDFObject::createReference(contentReference));
    builder.mergeTo(page, pdf::PDFObject::createDictionary(std::make_shared<pdf::PDFDictionary>(std::move(pageUpdate))));

    setDeterministicMetadata(builder);
    writeFixture(outputDir, "color-inventory-spot.pdf", builder.build());
}

void generateThinStrokesHairlineFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loop fixture - thin strokes hairline");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loop-preflight golden corpus: hairline stroke");

    const pdf::PDFObjectReference page = builder.appendPage(QRectF(0, 0, 200, 200));
    pdf::PDFPageContentStreamBuilder contentBuilder(&builder,
                                                    pdf::PDFContentStreamBuilder::CoordinateSystem::PDF);
    if (QPainter* painter = contentBuilder.begin(page))
    {
        QPen pen(Qt::black);
        pen.setWidthF(0.0);
        painter->setPen(pen);
        painter->drawLine(QPointF(20, 100), QPointF(180, 100));
        contentBuilder.end(painter);
    }

    setDeterministicMetadata(builder);
    writeFixture(outputDir, "thin-strokes-hairline.pdf", builder.build());
}

}   // namespace

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);
    pdf::initializeApplicationIdentity(pdf::PDFApplicationSurface::LoopPreflightFixtureGenerator);

    const QStringList arguments = QCoreApplication::arguments();
    const QDir outputDir(arguments.size() > 1 ? arguments.at(1) : QDir::currentPath());

    generateBleedAdequateFixture(outputDir);
    generateBleedMissingFixture(outputDir);
    generateContentBleedAdequateFixture(outputDir);
    generateContentBleedMissingFixture(outputDir);
    generateContentBleedRasterConfirmFixture(outputDir);
    generateContentBleedThreeOfFourFixture(outputDir);
    generateOverprintRenderFixtures(outputDir);
    generateWhiteOverprintColorSpaceFixtures(outputDir);
    generateOutputIntentCmykFixture(outputDir);
    generateOutputIntentMissingFixture(outputDir);
    generateOutputIntentProfileMissingFixture(outputDir);
    generateOutputIntentRgbFixture(outputDir);
    generateOutputIntentProfileInvalidFixture(outputDir);
    generateColorInventoryRichBlackFixture(outputDir);
    generateColorInventoryBlackOnlyFixture(outputDir);
    generateColorInventoryCmyRichNoKFixture(outputDir);
    generateColorInventorySpotFixture(outputDir);
    generateThinStrokesHairlineFixture(outputDir);

    return 0;
}
