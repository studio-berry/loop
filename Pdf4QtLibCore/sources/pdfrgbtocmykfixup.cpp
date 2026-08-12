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

#include "pdfrgbtocmykfixup.h"

#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "pdfparser.h"
#include "pdfstreamfilters.h"

#include <QCryptographicHash>

#include <lcms2.h>

#include <cmath>
#include <set>
#include <utility>
#include <vector>

namespace pdf
{

namespace
{

struct StreamReference
{
    PDFObjectReference reference;
    PDFInteger pageIndex = -1;
    PDFRgbToCmykObjectKind kind = PDFRgbToCmykObjectKind::VectorPaint;
};

struct Token
{
    PDFLexicalAnalyzer::TokenType type = PDFLexicalAnalyzer::TokenType::EndOfFile;
    QVariant data;
};

bool isNumber(const Token& token)
{
    return token.type == PDFLexicalAnalyzer::TokenType::Integer
        || token.type == PDFLexicalAnalyzer::TokenType::Real;
}

PDFReal numberValue(const Token& token)
{
    return token.type == PDFLexicalAnalyzer::TokenType::Integer
        ? PDFReal(token.data.toLongLong())
        : token.data.toDouble();
}

QByteArray formatNumber(PDFReal value)
{
    if (!std::isfinite(value))
    {
        return QByteArrayLiteral("0");
    }

    QByteArray result = QByteArray::number(value, 'f', 8);
    while (result.endsWith('0'))
    {
        result.chop(1);
    }
    if (result.endsWith('.'))
    {
        result.chop(1);
    }
    if (result.isEmpty() || result == QByteArrayLiteral("-0"))
    {
        result = QByteArrayLiteral("0");
    }
    return result;
}

QByteArray serializeToken(const Token& token)
{
    switch (token.type)
    {
        case PDFLexicalAnalyzer::TokenType::Boolean:
            return token.data.toBool() ? QByteArrayLiteral("true") : QByteArrayLiteral("false");

        case PDFLexicalAnalyzer::TokenType::Integer:
            return QByteArray::number(token.data.toLongLong());

        case PDFLexicalAnalyzer::TokenType::Real:
            return formatNumber(token.data.toDouble());

        case PDFLexicalAnalyzer::TokenType::String:
            return QByteArrayLiteral("<") + token.data.toByteArray().toHex() + QByteArrayLiteral(">");

        case PDFLexicalAnalyzer::TokenType::Name:
            return QByteArrayLiteral("/") + token.data.toByteArray();

        case PDFLexicalAnalyzer::TokenType::ArrayStart:
            return QByteArrayLiteral("[");

        case PDFLexicalAnalyzer::TokenType::ArrayEnd:
            return QByteArrayLiteral("]");

        case PDFLexicalAnalyzer::TokenType::DictionaryStart:
            return QByteArrayLiteral("<<");

        case PDFLexicalAnalyzer::TokenType::DictionaryEnd:
            return QByteArrayLiteral(">>");

        case PDFLexicalAnalyzer::TokenType::Null:
            return QByteArrayLiteral("null");

        case PDFLexicalAnalyzer::TokenType::Command:
            return token.data.toByteArray();

        case PDFLexicalAnalyzer::TokenType::EndOfFile:
            break;
    }

    return QByteArray();
}

PDFOperationResult validateTargetProfile(const PDFRgbToCmykSettings& settings)
{
    if (settings.targetIccData.isEmpty())
    {
        return PDFTranslationContext::tr("A target CMYK ICC profile is required.");
    }

    cmsHPROFILE profile = cmsOpenProfileFromMem(settings.targetIccData.constData(),
                                                static_cast<cmsUInt32Number>(settings.targetIccData.size()));
    if (!profile)
    {
        return PDFTranslationContext::tr("The target ICC profile could not be opened.");
    }

    const bool isCmyk = cmsGetColorSpace(profile) == cmsSigCmykData;
    cmsCloseProfile(profile);
    if (!isCmyk)
    {
        return PDFTranslationContext::tr("The target ICC profile is not a CMYK profile.");
    }

    return true;
}

QByteArray targetProfileId(const PDFRgbToCmykSettings& settings)
{
    if (!settings.targetIccId.isEmpty())
    {
        return settings.targetIccId;
    }
    return QCryptographicHash::hash(settings.targetIccData, QCryptographicHash::Sha256);
}

void appendContentReferences(const PDFObject& contentObject,
                             const PDFObjectStorage* storage,
                             std::vector<PDFObjectReference>& references,
                             std::vector<PDFObject>* directStreams = nullptr)
{
    const PDFObject object = storage->getObject(contentObject);
    if (object.isStream())
    {
        if (contentObject.isReference())
        {
            references.push_back(contentObject.getReference());
        }
        else if (directStreams)
        {
            directStreams->push_back(object);
        }
        return;
    }

    if (!object.isArray())
    {
        return;
    }

    for (const PDFObject& item : *object.getArray())
    {
        const PDFObject dereferenced = storage->getObject(item);
        if (dereferenced.isStream() && item.isReference())
        {
            references.push_back(item.getReference());
        }
        else if (dereferenced.isStream() && directStreams)
        {
            directStreams->push_back(dereferenced);
        }
    }
}

bool isRgbColorSpaceName(const QByteArray& name)
{
    return name == QByteArrayLiteral("DeviceRGB") || name == QByteArrayLiteral("RGB");
}

struct RewriteResult
{
    QByteArray content;
    int converted = 0;
    bool changed = false;
};

PDFOperationResult rewriteRgbOperators(const QByteArray& input,
                                       const PDFRgbToCmykSettings& settings,
                                       const PDFCMS* cms,
                                       const StreamReference& owner,
                                       PDFRgbToCmykReport* report,
                                       RewriteResult* result)
{
    std::vector<Token> tokens;
    try
    {
        PDFLexicalAnalyzer analyzer(input.constData(), input.constData() + input.size());
        while (true)
        {
            const PDFLexicalAnalyzer::Token token = analyzer.fetch();
            if (token.type == PDFLexicalAnalyzer::TokenType::EndOfFile)
            {
                break;
            }
            tokens.push_back(Token{ token.type, token.data });
        }
    }
    catch (const PDFException& exception)
    {
        return PDFTranslationContext::tr("Unable to parse a PDF content stream: %1")
            .arg(QString::fromUtf8(exception.what()));
    }

    std::vector<Token> output;
    output.reserve(tokens.size() + 8);
    bool fillRgb = false;
    bool strokeRgb = false;
    int converted = 0;
    bool changed = false;

    auto addUnsupported = [&](PDFRgbToCmykObjectKind kind, const QString& reason)
    {
        if (report)
        {
            PDFRgbToCmykUnsupportedItem item;
            item.pageIndex = owner.pageIndex;
            item.objectReference = owner.reference;
            item.kind = kind;
            item.reason = reason;
            report->unsupported.append(item);
        }
    };

    for (const Token& token : tokens)
    {
        if (token.type != PDFLexicalAnalyzer::TokenType::Command)
        {
            output.push_back(token);
            continue;
        }

        const QByteArray command = token.data.toByteArray();
        const bool stroke = command == QByteArrayLiteral("RG") || command == QByteArrayLiteral("CS");
        const bool fill = command == QByteArrayLiteral("rg") || command == QByteArrayLiteral("cs");

        if ((command == QByteArrayLiteral("rg") || command == QByteArrayLiteral("RG"))
            && output.size() >= 3 && isNumber(output[output.size() - 1])
            && isNumber(output[output.size() - 2]) && isNumber(output[output.size() - 3]))
        {
            std::vector<PDFColorComponent> source = {
                PDFColorComponent(numberValue(output[output.size() - 3])),
                PDFColorComponent(numberValue(output[output.size() - 2])),
                PDFColorComponent(numberValue(output[output.size() - 1]))
            };
            std::vector<PDFColorComponent> target(4);
            PDFCMS::ColorSpaceTransformParams params;
            params.sourceType = settings.fallbackRgbIccData.isEmpty()
                ? PDFCMS::ColorSpaceType::DeviceRGB : PDFCMS::ColorSpaceType::ICC;
            params.targetType = PDFCMS::ColorSpaceType::ICC;
            params.sourceIccId = settings.fallbackRgbIccId;
            params.sourceIccData = settings.fallbackRgbIccData;
            params.targetIccId = targetProfileId(settings);
            params.targetIccData = settings.targetIccData;
            params.input = PDFColorBuffer(source.data(), source.size());
            params.output = PDFColorBuffer(target.data(), target.size());
            params.intent = settings.intent;

            if (!cms->transformColorSpace(params))
            {
                return PDFTranslationContext::tr("LittleCMS could not convert an RGB vector paint.");
            }

            output.resize(output.size() - 3);
            for (PDFColorComponent component : target)
            {
                output.push_back(Token{ PDFLexicalAnalyzer::TokenType::Real, QVariant(double(component)) });
            }
            output.push_back(Token{ PDFLexicalAnalyzer::TokenType::Command,
                                    command == QByteArrayLiteral("RG") ? QByteArrayLiteral("K") : QByteArrayLiteral("k") });
            ++converted;
            changed = true;
            if (command == QByteArrayLiteral("RG"))
            {
                strokeRgb = false;
            }
            else
            {
                fillRgb = false;
            }
            continue;
        }

        if ((command == QByteArrayLiteral("sc") || command == QByteArrayLiteral("SC"))
            && output.size() >= 3 && isNumber(output[output.size() - 1])
            && isNumber(output[output.size() - 2]) && isNumber(output[output.size() - 3])
            && ((command == QByteArrayLiteral("sc") && fillRgb)
                || (command == QByteArrayLiteral("SC") && strokeRgb)))
        {
            std::vector<PDFColorComponent> source = {
                PDFColorComponent(numberValue(output[output.size() - 3])),
                PDFColorComponent(numberValue(output[output.size() - 2])),
                PDFColorComponent(numberValue(output[output.size() - 1]))
            };
            std::vector<PDFColorComponent> target(4);
            PDFCMS::ColorSpaceTransformParams params;
            params.sourceType = settings.fallbackRgbIccData.isEmpty()
                ? PDFCMS::ColorSpaceType::DeviceRGB : PDFCMS::ColorSpaceType::ICC;
            params.targetType = PDFCMS::ColorSpaceType::ICC;
            params.sourceIccId = settings.fallbackRgbIccId;
            params.sourceIccData = settings.fallbackRgbIccData;
            params.targetIccId = targetProfileId(settings);
            params.targetIccData = settings.targetIccData;
            params.input = PDFColorBuffer(source.data(), source.size());
            params.output = PDFColorBuffer(target.data(), target.size());
            params.intent = settings.intent;

            if (!cms->transformColorSpace(params))
            {
                return PDFTranslationContext::tr("LittleCMS could not convert an RGB color-space paint.");
            }

            output.resize(output.size() - 3);
            for (PDFColorComponent component : target)
            {
                output.push_back(Token{ PDFLexicalAnalyzer::TokenType::Real, QVariant(double(component)) });
            }
            output.push_back(token);
            ++converted;
            changed = true;
            continue;
        }

        if ((command == QByteArrayLiteral("cs") || command == QByteArrayLiteral("CS"))
            && !output.empty() && output.back().type == PDFLexicalAnalyzer::TokenType::Name)
        {
            const QByteArray name = output.back().data.toByteArray();
            if (isRgbColorSpaceName(name))
            {
                output.back().data = QByteArrayLiteral("DeviceCMYK");
                if (fill)
                {
                    fillRgb = true;
                }
                else if (stroke)
                {
                    strokeRgb = true;
                }
                changed = true;
                output.push_back(token);
                continue;
            }
            if (name == QByteArrayLiteral("DeviceCMYK"))
            {
                if (fill)
                {
                    fillRgb = false;
                }
                else if (stroke)
                {
                    strokeRgb = false;
                }
            }
        }

        if ((command == QByteArrayLiteral("scn") || command == QByteArrayLiteral("SCN"))
            && ((command == QByteArrayLiteral("scn") && fillRgb)
                || (command == QByteArrayLiteral("SCN") && strokeRgb)))
        {
            addUnsupported(PDFRgbToCmykObjectKind::VectorPaint,
                           PDFTranslationContext::tr("RGB pattern or extended color paint is not supported."));
        }

        output.push_back(token);
    }

    if (result)
    {
        result->converted = converted;
        result->changed = changed;
        if (changed)
        {
            QByteArray serialized;
            for (const Token& token : output)
            {
                if (!serialized.isEmpty())
                {
                    serialized.append('\n');
                }
                serialized.append(serializeToken(token));
            }
            result->content = qMove(serialized);
        }
        else
        {
            result->content = input;
        }
    }

    return true;
}

void scanImageResources(const PDFObject& resourcesObject,
                        const PDFObjectStorage* storage,
                        PDFInteger pageIndex,
                        PDFRgbToCmykReport* report)
{
    const PDFObject resources = storage->getObject(resourcesObject);
    if (!resources.isDictionary())
    {
        return;
    }

    const PDFObject xObject = storage->getObject(resources.getDictionary()->get("XObject"));
    if (!xObject.isDictionary())
    {
        return;
    }

    for (size_t i = 0; i < xObject.getDictionary()->getCount(); ++i)
    {
        const PDFObject object = storage->getObject(xObject.getDictionary()->getValue(i));
        if (!object.isStream())
        {
            continue;
        }

        const PDFDictionary* dictionary = object.getStream()->getDictionary();
        if (dictionary->get("Subtype").isName()
            && dictionary->get("Subtype").getString() == QByteArrayLiteral("Image"))
        {
            const PDFObject colorSpace = storage->getObject(dictionary->get("ColorSpace"));
            if (colorSpace.isName() && isRgbColorSpaceName(colorSpace.getString()) && report)
            {
                PDFRgbToCmykUnsupportedItem item;
                item.pageIndex = pageIndex;
                item.kind = PDFRgbToCmykObjectKind::Image;
                item.reason = PDFTranslationContext::tr("RGB image XObjects require image-sample conversion.");
                if (xObject.getDictionary()->getValue(i).isReference())
                {
                    item.objectReference = xObject.getDictionary()->getValue(i).getReference();
                }
                report->unsupported.append(item);
            }
        }
    }
}

std::vector<StreamReference> collectPageStreams(const PDFDocument* document,
                                                PDFInteger pageIndex,
                                                const PDFPage* page)
{
    std::vector<StreamReference> result;
    std::vector<PDFObjectReference> references;
    // PDFPage::getContents() returns the already-dereferenced stream object, which loses
    // reference identity and makes appendContentReferences() treat it as an inline stream
    // (it only checks contentObject.isReference()). Read the raw, still-possibly-a-reference
    // /Contents entry straight from the page dictionary instead, so the underlying object can
    // actually be found and later rewritten via its PDFObjectReference.
    const PDFObject pageDictionaryObject = document->getStorage().getObjectByReference(page->getPageReference());
    const PDFObject rawContents = pageDictionaryObject.isDictionary()
        ? pageDictionaryObject.getDictionary()->get("Contents")
        : PDFObject();
    appendContentReferences(rawContents, &document->getStorage(), references);
    for (const PDFObjectReference reference : references)
    {
        result.push_back(StreamReference{ reference, pageIndex, PDFRgbToCmykObjectKind::VectorPaint });
    }
    return result;
}

void collectFormStreamsFromResources(const PDFObject& resourcesObject,
                                     const PDFObjectStorage* storage,
                                     PDFInteger pageIndex,
                                     std::vector<StreamReference>& result,
                                     std::set<PDFObjectReference>& visited)
{
    const PDFObject resources = storage->getObject(resourcesObject);
    if (!resources.isDictionary())
    {
        return;
    }

    const PDFObject xObject = storage->getObject(resources.getDictionary()->get("XObject"));
    if (!xObject.isDictionary())
    {
        return;
    }

    for (size_t i = 0; i < xObject.getDictionary()->getCount(); ++i)
    {
        const PDFObject referenceObject = xObject.getDictionary()->getValue(i);
        const PDFObject object = storage->getObject(referenceObject);
        if (!object.isStream())
        {
            continue;
        }
        const PDFDictionary* dictionary = object.getStream()->getDictionary();
        if (!dictionary->get("Subtype").isName()
            || dictionary->get("Subtype").getString() != QByteArrayLiteral("Form")
            || !referenceObject.isReference())
        {
            continue;
        }

        const PDFObjectReference reference = referenceObject.getReference();
        if (visited.insert(reference).second)
        {
            result.push_back(StreamReference{ reference, pageIndex, PDFRgbToCmykObjectKind::Form });
            collectFormStreamsFromResources(dictionary->get("Resources"), storage, pageIndex, result, visited);
        }
    }
}

void collectFormStreamsFromAnnotations(const PDFDocument* document,
                                       const PDFPage* page,
                                       PDFInteger pageIndex,
                                       std::vector<StreamReference>& result,
                                       std::set<PDFObjectReference>& visited)
{
    const PDFObjectStorage* storage = &document->getStorage();
    for (const PDFObjectReference annotationReference : page->getAnnotations())
    {
        const PDFObject annotation = storage->getObjectByReference(annotationReference);
        if (!annotation.isDictionary())
        {
            continue;
        }
        const PDFObject appearance = storage->getObject(annotation.getDictionary()->get("AP"));
        if (!appearance.isDictionary())
        {
            continue;
        }
        const PDFDictionary* appearanceDictionary = appearance.getDictionary();
        for (size_t i = 0; i < appearanceDictionary->getCount(); ++i)
        {
            const PDFObject value = appearanceDictionary->getValue(i);
            const PDFObject appearanceStream = storage->getObject(value);
            if (!appearanceStream.isStream() || !value.isReference())
            {
                continue;
            }
            const PDFDictionary* dictionary = appearanceStream.getStream()->getDictionary();
            if (dictionary->get("Subtype").isName()
                && dictionary->get("Subtype").getString() == QByteArrayLiteral("Form")
                && visited.insert(value.getReference()).second)
            {
                result.push_back(StreamReference{ value.getReference(), pageIndex,
                                                  PDFRgbToCmykObjectKind::AnnotationAppearance });
                collectFormStreamsFromResources(dictionary->get("Resources"), storage, pageIndex, result, visited);
            }
        }
    }
}

std::vector<PDFInteger> selectPageIndices(const PDFDocument* document,
                                          const QString& pageRange,
                                          QString* errorMessage)
{
    std::vector<PDFInteger> result;
    const PDFInteger pageCount = document ? PDFInteger(document->getCatalog()->getPageCount()) : 0;
    if (!document)
    {
        if (errorMessage)
        {
            *errorMessage = PDFTranslationContext::tr("Invalid document.");
        }
        return result;
    }

    std::set<PDFInteger> selected;
    const QString rangeText = pageRange.simplified();
    if (!rangeText.isEmpty())
    {
        QString parseError;
        const PDFClosedIntervalSet ranges = PDFClosedIntervalSet::parse(1, pageCount, rangeText, &parseError);
        if (!parseError.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = parseError;
            }
            return {};
        }
        for (const PDFInteger pageNumber : ranges.unfold())
        {
            selected.insert(pageNumber);
        }
    }

    for (PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        if (!selected.empty() && !selected.count(pageIndex + 1))
        {
            continue;
        }
        result.push_back(pageIndex);
    }
    return result;
}

PDFOperationResult embedOutputIntent(PDFDocumentBuilder* builder,
                                     const PDFRgbToCmykSettings& settings,
                                     PDFRgbToCmykReport* report)
{
    if (!settings.embedOutputIntent)
    {
        return true;
    }

    QByteArray compressed = PDFFlateDecodeFilter::compress(settings.targetIccData);
    PDFDictionary profileDictionary;
    profileDictionary.addEntry(PDFInplaceOrMemoryString("N"), PDFObject::createInteger(4));
    profileDictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(compressed.size()));
    profileDictionary.addEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
    const PDFObjectReference profileReference = builder->addObject(
        PDFObject::createStream(std::make_shared<PDFStream>(qMove(profileDictionary), qMove(compressed))));

    PDFDictionary intentDictionary;
    intentDictionary.addEntry(PDFInplaceOrMemoryString("Type"), PDFObject::createName("OutputIntent"));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("S"), PDFObject::createName("GTS_PDFX"));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("OutputConditionIdentifier"),
                              PDFObject::createString((settings.targetProfileName.isEmpty()
                                  ? QString::fromLatin1(targetProfileId(settings).toHex())
                                  : settings.targetProfileName).toUtf8()));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("OutputCondition"),
                              PDFObject::createString(settings.targetProfileName.toUtf8()));
    intentDictionary.addEntry(PDFInplaceOrMemoryString("DestOutputProfile"),
                              PDFObject::createReference(profileReference));
    const PDFObjectReference intentReference = builder->addObject(
        PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(intentDictionary))));

    PDFArray outputIntents;
    outputIntents.appendItem(PDFObject::createReference(intentReference));
    PDFDictionary catalogUpdate;
    catalogUpdate.addEntry(PDFInplaceOrMemoryString("OutputIntents"),
                           PDFObject::createArray(std::make_shared<PDFArray>(qMove(outputIntents))));
    builder->mergeTo(builder->getCatalogReference(),
                     PDFObject::createDictionary(std::make_shared<PDFDictionary>(qMove(catalogUpdate))));
    if (report)
    {
        report->outputIntentChanged = true;
    }
    return true;
}

PDFOperationResult analyzeImpl(const PDFDocument* document,
                               const PDFRgbToCmykSettings& settings,
                               PDFRgbToCmykReport* report,
                               const PDFCMS* cms,
                               const PDFObjectStorage* storage)
{
    QString pageSelectionError;
    const std::vector<PDFInteger> pageIndices = selectPageIndices(document, settings.pageRange, &pageSelectionError);
    if (!pageSelectionError.isEmpty())
    {
        return pageSelectionError;
    }

    std::set<PDFObjectReference> visited;
    for (const PDFInteger pageIndex : pageIndices)
    {
        const PDFPage* page = document->getCatalog()->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        scanImageResources(page->getResources(), storage, pageIndex, report);
        std::vector<StreamReference> streams = collectPageStreams(document, pageIndex, page);
        std::set<PDFObjectReference> formReferences;
        collectFormStreamsFromResources(page->getResources(), &document->getStorage(), pageIndex, streams, formReferences);
        collectFormStreamsFromAnnotations(document, page, pageIndex, streams, formReferences);
        for (const StreamReference& stream : streams)
        {
            if (!visited.insert(stream.reference).second)
            {
                continue;
            }
            const PDFObject object = storage->getObjectByReference(stream.reference);
            if (!object.isStream())
            {
                continue;
            }

            RewriteResult rewrite;
            const PDFOperationResult result = rewriteRgbOperators(
                storage->getDecodedStream(object.getStream()), settings, cms, stream, report, &rewrite);
            if (!result)
            {
                return result;
            }
            if (report)
            {
                report->vectorPaintsConverted += rewrite.converted;
                if (stream.kind == PDFRgbToCmykObjectKind::Form)
                {
                    ++report->formsVisited;
                }
                else if (stream.kind == PDFRgbToCmykObjectKind::AnnotationAppearance)
                {
                    ++report->annotationAppearancesVisited;
                }
            }
        }
    }
    return true;
}

} // namespace

PDFOperationResult PDFRgbToCmykFixup::previewRgbToCmyk(const PDFDocument* document,
                                             const PDFRgbToCmykSettings& settings,
                                             PDFRgbToCmykReport* report)
{
    if (report)
    {
        *report = PDFRgbToCmykReport();
    }
    if (!document)
    {
        return PDFTranslationContext::tr("Invalid document.");
    }

    const PDFOperationResult profileResult = validateTargetProfile(settings);
    if (!profileResult)
    {
        return profileResult;
    }

    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSSettings cmsSettings = cmsManager.getDefaultSettings();
    cmsSettings.isBlackPointCompensationActive = settings.blackPointCompensation;
    cmsManager.setSettings(cmsSettings);
    const PDFCMSPointer cms = cmsManager.getCurrentCMS();
    if (!cms)
    {
        return PDFTranslationContext::tr("No color-management system is available.");
    }

    return analyzeImpl(document, settings, report, cms.data(), &document->getStorage());
}

PDFOperationResult PDFRgbToCmykFixup::writeRgbToCmyk(PDFDocument* document,
                                           const PDFRgbToCmykSettings& settings,
                                           PDFRgbToCmykReport* report)
{
    if (report)
    {
        *report = PDFRgbToCmykReport();
    }

    PDFRgbToCmykReport localReport;
    const PDFOperationResult analysisResult = previewRgbToCmyk(document, settings, &localReport);
    if (!analysisResult)
    {
        return analysisResult;
    }
    if (!localReport.unsupported.isEmpty())
    {
        return PDFTranslationContext::tr(
            "RGB-to-CMYK conversion cannot be completed safely: %1 unsupported RGB object(s) were found.")
            .arg(localReport.unsupported.size());
    }
    if (settings.dryRunOnly)
    {
        localReport.postflightPassed = true;
        if (report)
        {
            *report = qMove(localReport);
        }
        return true;
    }

    PDFDocumentModifier modifier(document);
    PDFDocumentBuilder* builder = modifier.getBuilder();
    std::set<PDFObjectReference> visited;
    PDFCMSManager cmsManager(nullptr);
    cmsManager.setDocument(document);
    PDFCMSSettings cmsSettings = cmsManager.getDefaultSettings();
    cmsSettings.isBlackPointCompensationActive = settings.blackPointCompensation;
    cmsManager.setSettings(cmsSettings);
    const PDFCMSPointer cms = cmsManager.getCurrentCMS();

    // The first analysis is a safety gate. Start the mutation report at zero
    // so conversion counts describe the committed pass rather than both passes.
    localReport.vectorPaintsConverted = 0;
    localReport.imagesConverted = 0;
    localReport.indexedPalettesConverted = 0;
    localReport.formsVisited = 0;
    localReport.annotationAppearancesVisited = 0;

    QString pageSelectionError;
    const std::vector<PDFInteger> pageIndices = selectPageIndices(document, settings.pageRange, &pageSelectionError);
    if (!pageSelectionError.isEmpty())
    {
        return pageSelectionError;
    }

    for (const PDFInteger pageIndex : pageIndices)
    {
        const PDFPage* page = document->getCatalog()->getPage(pageIndex);
        if (!page)
        {
            continue;
        }

        std::vector<StreamReference> streams = collectPageStreams(document, pageIndex, page);
        std::set<PDFObjectReference> formReferences;
        collectFormStreamsFromResources(page->getResources(), &document->getStorage(), pageIndex, streams, formReferences);
        collectFormStreamsFromAnnotations(document, page, pageIndex, streams, formReferences);
        for (const StreamReference& stream : streams)
        {
            if (!visited.insert(stream.reference).second)
            {
                continue;
            }
            const PDFObject object = builder->getObjectByReference(stream.reference);
            if (!object.isStream())
            {
                continue;
            }

            RewriteResult rewrite;
            const PDFOperationResult result = rewriteRgbOperators(
                builder->getDecodedStream(object.getStream()), settings, cms.data(), stream, &localReport, &rewrite);
            if (!result)
            {
                return result;
            }
            if (!rewrite.changed)
            {
                continue;
            }

            PDFDictionary dictionary = *object.getStream()->getDictionary();
            QByteArray encoded = PDFFlateDecodeFilter::compress(rewrite.content);
            dictionary.setEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(encoded.size()));
            dictionary.setEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
            builder->setObject(stream.reference,
                               PDFObject::createStream(std::make_shared<PDFStream>(qMove(dictionary), qMove(encoded))));
        }
    }

    const PDFOperationResult outputIntentResult = embedOutputIntent(builder, settings, &localReport);
    if (!outputIntentResult)
    {
        return outputIntentResult;
    }

    modifier.markReset();
    modifier.markPageContentsChanged();
    if (!modifier.finalize())
    {
        return PDFTranslationContext::tr("Failed to finalize RGB-to-CMYK conversion.");
    }

    PDFDocumentPointer candidate = modifier.getDocument();
    if (settings.revalidate)
    {
        PDFRgbToCmykReport postflight;
        const PDFOperationResult postflightResult = previewRgbToCmyk(candidate.data(), settings, &postflight);
        if (!postflightResult)
        {
            return postflightResult;
        }
        if (!postflight.unsupported.isEmpty() || postflight.vectorPaintsConverted > 0)
        {
            return PDFTranslationContext::tr("Post-conversion validation still found RGB content.");
        }
    }

    *document = *candidate;
    localReport.postflightPassed = true;
    if (report)
    {
        *report = qMove(localReport);
    }
    return true;
}

} // namespace pdf
