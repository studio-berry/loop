#include "pdfrgbtocmykimagefixup.h"

#include "pdfrgbtocmykhelpers.h"

#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "pdfimage.h"
#include "pdfstreamfilters.h"

#include <limits>
#include <utility>
#include <vector>

namespace pdf
{

namespace
{

bool isConvertibleRgbImage(const PDFDocument* document,
                           const PDFObject& object,
                           PDFRgbToCmykUnsupportedItem* unsupported)
{
    if (!object.isStream())
    {
        return false;
    }

    try
    {
        const PDFImage image = PDFImage::createImage(document,
                                                     object.getStream(),
                                                     PDFColorSpacePointer(new PDFDeviceRGBColorSpace()),
                                                     false,
                                                     RenderingIntent::Perceptual,
                                                     nullptr);
        const PDFImageData& imageData = image.getImageData();
        if (imageData.getComponents() == 3 && imageData.getWidth() > 0 && imageData.getHeight() > 0 &&
            imageData.getBitsPerComponent() > 0 && imageData.getBitsPerComponent() <= 16 &&
            imageData.getMaskingType() != PDFImageData::MaskingType::ColorKeyMasking)
        {
            return true;
        }
    }
    catch (const PDFException&)
    {
    }

    if (unsupported)
    {
        unsupported->kind = PDFRgbToCmykObjectKind::Image;
        unsupported->reason = PDFTranslationContext::tr(
            "RGB image XObjects must contain convertible RGB image samples.");
    }
    return false;
}

PDFObject createCmykImageColorSpace(PDFObjectReference profileReference)
{
    auto colorSpace = std::make_shared<PDFArray>();
    colorSpace->appendItem(PDFObject::createName("ICCBased"));
    colorSpace->appendItem(PDFObject::createReference(profileReference));
    return PDFObject::createArray(qMove(colorSpace));
}

PDFOperationResult convertRgbImage(PDFDocumentBuilder* builder,
                                   const PDFDocument* document,
                                   PDFObjectReference imageReference,
                                   PDFObject imageObject,
                                   const PDFRgbToCmykSettings& settings,
                                   const PDFCMS* cms,
                                   PDFObjectReference profileReference)
{
    if (!imageObject.isStream())
    {
        return PDFTranslationContext::tr("RGB image XObject is not a stream.");
    }

    PDFImage image;
    try
    {
        image = PDFImage::createImage(document,
                                      imageObject.getStream(),
                                      PDFColorSpacePointer(new PDFDeviceRGBColorSpace()),
                                      false,
                                      RenderingIntent::Perceptual,
                                      nullptr);
    }
    catch (const PDFException& exception)
    {
        return PDFTranslationContext::tr("Unable to decode an RGB image XObject: %1")
            .arg(QString::fromUtf8(exception.what()));
    }

    const PDFImageData& source = image.getImageData();
    if (source.getComponents() != 3 || source.getWidth() == 0 || source.getHeight() == 0 ||
        source.getBitsPerComponent() == 0 || source.getBitsPerComponent() > 16 ||
        source.getMaskingType() == PDFImageData::MaskingType::ColorKeyMasking)
    {
        return PDFTranslationContext::tr("RGB image XObject has unsupported image samples.");
    }

    const std::vector<PDFReal>& decode = source.getDecode();
    if (!decode.empty() && decode.size() != 6)
    {
        return PDFTranslationContext::tr("RGB image XObject has an invalid decode array.");
    }

    const size_t pixelCount = static_cast<size_t>(source.getWidth()) * static_cast<size_t>(source.getHeight());
    if (pixelCount > std::numeric_limits<size_t>::max() / 3)
    {
        return PDFTranslationContext::tr("RGB image XObject is too large to convert.");
    }

    std::vector<PDFColorComponent> input(pixelCount * 3);
    std::vector<PDFColorComponent> output(pixelCount * 4);
    PDFBitReader reader(&source.getData(), source.getBitsPerComponent());
    const double maximum = reader.max();
    if (maximum <= 0.0)
    {
        return PDFTranslationContext::tr("RGB image XObject has invalid sample precision.");
    }

    for (unsigned int y = 0; y < source.getHeight(); ++y)
    {
        reader.seek(y * source.getStride());
        for (unsigned int x = 0; x < source.getWidth(); ++x)
        {
            for (unsigned int component = 0; component < 3; ++component)
            {
                const double normalized = static_cast<double>(reader.read()) / maximum;
                input[(static_cast<size_t>(y) * source.getWidth() + x) * 3 + component] =
                    decode.empty()
                        ? static_cast<PDFColorComponent>(normalized)
                        : static_cast<PDFColorComponent>(decode[component * 2] + normalized * (decode[component * 2 + 1] - decode[component * 2]));
            }
        }
    }

    PDFCMS::ColorSpaceTransformParams params;
    params.sourceType = settings.fallbackRgbIccData.isEmpty()
                            ? PDFCMS::ColorSpaceType::DeviceRGB
                            : PDFCMS::ColorSpaceType::ICC;
    params.targetType = PDFCMS::ColorSpaceType::ICC;
    params.sourceIccId = settings.fallbackRgbIccId;
    params.sourceIccData = settings.fallbackRgbIccData;
    params.targetIccId = targetProfileId(settings);
    params.targetIccData = settings.targetIccData;
    params.input = PDFColorBuffer(input.data(), input.size());
    params.output = PDFColorBuffer(output.data(), output.size());
    params.intent = settings.intent;
    if (!cms->transformColorSpace(params))
    {
        return PDFTranslationContext::tr("LittleCMS could not convert RGB image samples.");
    }

    QByteArray encoded;
    encoded.resize(static_cast<qsizetype>(pixelCount * 4));
    for (size_t i = 0; i < output.size(); ++i)
    {
        const PDFColorComponent component = qBound<PDFColorComponent>(0.0f, output[i], 1.0f);
        encoded[static_cast<qsizetype>(i)] = static_cast<char>(qRound(component * 255.0f));
    }

    PDFDictionary dictionary = *imageObject.getStream()->getDictionary();
    dictionary.setEntry(PDFInplaceOrMemoryString("ColorSpace"), createCmykImageColorSpace(profileReference));
    dictionary.setEntry(PDFInplaceOrMemoryString("BitsPerComponent"), PDFObject::createInteger(8));
    dictionary.setEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
    dictionary.removeEntry("DecodeParms");
    PDFArray decodeArray;
    for (int i = 0; i < 4; ++i)
    {
        decodeArray.appendItem(PDFObject::createReal(0.0));
        decodeArray.appendItem(PDFObject::createReal(1.0));
    }
    dictionary.setEntry(PDFInplaceOrMemoryString("Decode"),
                        PDFObject::createArray(std::make_shared<PDFArray>(qMove(decodeArray))));
    dictionary.setEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(encoded.size()));
    dictionary.setEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
    QByteArray compressed = PDFFlateDecodeFilter::compress(encoded);
    dictionary.setEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(compressed.size()));
    builder->setObject(imageReference,
                       PDFObject::createStream(std::make_shared<PDFStream>(qMove(dictionary), qMove(compressed))));
    return true;
}

}   // namespace

void scanImageResources(const PDFObject& resourcesObject,
                        const PDFDocument* document,
                        PDFInteger pageIndex,
                        PDFRgbToCmykReport* report)
{
    const PDFObjectStorage* storage = &document->getStorage();
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
        if (dictionary->get("Subtype").isName() && dictionary->get("Subtype").getString() == QByteArrayLiteral("Image"))
        {
            const PDFObject colorSpace = storage->getObject(dictionary->get("ColorSpace"));
            if (colorSpace.isName() && isRgbColorSpaceName(colorSpace.getString()) && report)
            {
                PDFRgbToCmykUnsupportedItem item;
                item.pageIndex = pageIndex;
                if (xObject.getDictionary()->getValue(i).isReference())
                {
                    item.objectReference = xObject.getDictionary()->getValue(i).getReference();
                }
                if (isConvertibleRgbImage(document, object, &item))
                {
                    ++report->imagesConverted;
                }
                else
                {
                    report->unsupported.append(item);
                }
            }
        }
    }
}

PDFObjectReference addIccProfileObject(PDFDocumentBuilder* builder, const PDFRgbToCmykSettings& settings)
{
    QByteArray compressed = PDFFlateDecodeFilter::compress(settings.targetIccData);
    PDFDictionary profileDictionary;
    profileDictionary.addEntry(PDFInplaceOrMemoryString("N"), PDFObject::createInteger(4));
    profileDictionary.addEntry(PDFInplaceOrMemoryString("Length"), PDFObject::createInteger(compressed.size()));
    profileDictionary.addEntry(PDFInplaceOrMemoryString("Filter"), PDFObject::createName("FlateDecode"));
    return builder->addObject(
        PDFObject::createStream(std::make_shared<PDFStream>(qMove(profileDictionary), qMove(compressed))));
}

PDFOperationResult convertRgbImages(const PDFObject& resourcesObject,
                                    const PDFDocument* document,
                                    PDFDocumentBuilder* builder,
                                    const PDFRgbToCmykSettings& settings,
                                    const PDFCMS* cms,
                                    PDFObjectReference profileReference,
                                    PDFRgbToCmykReport* report)
{
    const PDFObject resources = builder->getStorage()->getObject(resourcesObject);
    if (!resources.isDictionary())
    {
        return true;
    }
    const PDFObject xObject = builder->getStorage()->getObject(resources.getDictionary()->get("XObject"));
    if (!xObject.isDictionary())
    {
        return true;
    }

    for (size_t i = 0; i < xObject.getDictionary()->getCount(); ++i)
    {
        const PDFObject objectReference = xObject.getDictionary()->getValue(i);
        const PDFObject object = builder->getStorage()->getObject(objectReference);
        if (!object.isStream())
        {
            continue;
        }
        const PDFDictionary* dictionary = object.getStream()->getDictionary();
        const PDFObject colorSpace = builder->getStorage()->getObject(dictionary->get("ColorSpace"));
        if (!dictionary->get("Subtype").isName() || dictionary->get("Subtype").getString() != QByteArrayLiteral("Image") ||
            !colorSpace.isName() || !isRgbColorSpaceName(colorSpace.getString()))
        {
            continue;
        }
        if (!objectReference.isReference())
        {
            return PDFTranslationContext::tr("RGB image XObject must be referenceable before conversion.");
        }
        const PDFOperationResult result = convertRgbImage(builder,
                                                          document,
                                                          objectReference.getReference(),
                                                          object,
                                                          settings,
                                                          cms,
                                                          profileReference);
        if (!result)
        {
            return result;
        }
        if (report)
        {
            ++report->imagesConverted;
        }
    }
    return true;
}

}   // namespace pdf
