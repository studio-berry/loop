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

// Regenerates the golden PDF fixtures under loupe-preflight/testdata/fixtures/.
// The fixtures are committed to git; re-run this tool only when a fixture's
// geometry needs to change (see loupe-preflight/README.md).
//
// Usage: generate_fixtures [output-directory]  (defaults to the current directory)

#include "pdfdocumentbuilder.h"
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
    // amount_pt required by loupe-default.json's bleed check.
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - bleed adequate");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: adequate bleed");

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
    builder.setDocumentTitle("Loupe fixture - bleed missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: missing bleed");

    builder.appendPage(QRectF(0, 0, 200, 200));

    writeFixture(outputDir, "bleed-missing.pdf", builder.build());
}

void generateContentBleedAdequateFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - content bleed adequate");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: artwork extends into bleed");

    appendTieredBleedPage(builder, QRectF(0, 0, MEDIA_SIZE_PT, MEDIA_SIZE_PT));
    writeFixture(outputDir, "content-bleed-adequate.pdf", builder.build());
}

void generateContentBleedMissingFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - content bleed missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: artwork stops at trim");

    appendTieredBleedPage(builder, QRectF(TRIM_INSET_PT, TRIM_INSET_PT, TRIM_SIZE_PT, TRIM_SIZE_PT));
    writeFixture(outputDir, "content-bleed-missing.pdf", builder.build());
}

void generateContentBleedRasterConfirmFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - content bleed raster confirm");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: fast bounds flags bleed gap");

    appendTieredBleedPage(builder, QRectF(TRIM_INSET_PT, TRIM_INSET_PT, TRIM_SIZE_PT, TRIM_SIZE_PT));
    writeFixture(outputDir, "content-bleed-raster-confirm.pdf", builder.build());
}

void generateContentBleedThreeOfFourFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - content bleed three of four");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: bleed on three edges only");

    // Artwork covers left, right, and top bleed strips but stops above the bottom strip.
    appendTieredBleedPage(builder, QRectF(0, TRIM_INSET_PT + 1.0, MEDIA_SIZE_PT, MEDIA_SIZE_PT - TRIM_INSET_PT - 1.0));
    writeFixture(outputDir, "content-bleed-three-of-four.pdf", builder.build());
}

void generateOutputIntentCmykFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - output intent CMYK");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: valid CMYK output intent");
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
    builder.setDocumentTitle("Loupe fixture - output intent missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: missing output intent");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-missing.pdf", builder.build());
}

void generateOutputIntentProfileMissingFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - output intent profile missing");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: output intent without ICC profile");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setOutputIntents(builder, { appendOutputIntent(builder, "CGATS TR 001", "CMYK", QByteArray()) });
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-profile-missing.pdf", builder.build());
}

void generateOutputIntentRgbFixture(const QDir& outputDir)
{
    pdf::PDFDocumentBuilder builder;
    builder.setDocumentTitle("Loupe fixture - output intent RGB");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: disallowed RGB output intent");
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
    builder.setDocumentTitle("Loupe fixture - output intent profile invalid");
    builder.setDocumentCreator(QCoreApplication::applicationName());
    builder.setDocumentSubject("loupe-preflight golden corpus: invalid output intent ICC profile");
    builder.appendPage(QRectF(0, 0, 200, 200));

    setOutputIntents(builder, { appendOutputIntent(builder,
                                                    "CGATS TR 001",
                                                    "CMYK",
                                                    QByteArrayLiteral("not an ICC profile")) });
    setDeterministicMetadata(builder);
    writeFixture(outputDir, "output-intent-profile-invalid.pdf", builder.build());
}

}   // namespace

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName("MelkaJ");
    QCoreApplication::setApplicationName("LoupeGenerateFixtures");

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

    return 0;
}
